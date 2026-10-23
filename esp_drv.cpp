// esp_drv.cpp — CS2 ESP kernel driver (C++20, manual-map ready, no SEH, no STL/RTTI/exceptions)
// build: MSVC + WDK, /std:c++20 /kernel /O2 /Ob3 /Oi /Ot /GS- /GR- /EHs-c- /EHc- /Zl
// DEBUG_LOG — определить, чтобы включить DbgPrintEx (в release НЕ определять)

#include <ntifs.h>
#include <ntddk.h>
#include <intrin.h>
#include <stdarg.h>

extern "C" NTSTATUS ZwQuerySystemInformation(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

typedef enum _SECTION_INFORMATION_CLASS {
    SectionBasicInformation = 0,
    SectionImageInformation = 1
} SECTION_INFORMATION_CLASS;

typedef struct _SECTION_BASIC_INFORMATION {
    PVOID         BaseAddress;
    ULONG         Attributes;
    LARGE_INTEGER Size;
} SECTION_BASIC_INFORMATION, *PSECTION_BASIC_INFORMATION;

extern "C" NTSTATUS ZwQuerySection(
    HANDLE SectionHandle,
    SECTION_INFORMATION_CLASS SectionInformationClass,
    PVOID SectionInformation,
    SIZE_T SectionInformationLength,
    PSIZE_T ReturnLength
);

// ---- Pool tag: рандомный, генерируется в DriverEntry ----
static ULONG g_poolTag = 'Ntfs';   // fallback

constexpr ULONG   MAX_ENTITIES   = 64;
constexpr ULONG   MAX_BONES      = 24;
constexpr ULONG   MAX_LOGS       = 256;
constexpr ULONG   LOG_LINE_MAX   = 128;
constexpr ULONG   ENTITY_STRIDE  = 112; // 0x70

// ---- Offsets: заполняются из usermode через SharedConfig ----
static ULONG64 g_offEntityList      = 0;
static ULONG64 g_offLocalPlayerPawn = 0;
static ULONG64 g_offViewMatrix      = 0;
static ULONG64 g_offGameSceneNode   = 0;
static ULONG64 g_offHealth          = 0;
static ULONG64 g_offLifeState       = 0;
static ULONG64 g_offTeamNum         = 0;
static ULONG64 g_offModelState      = 0;
static ULONG64 g_offPlayerPawn      = 0;

static ULONG64 g_pawnBatchBase      = 0;
static SIZE_T  g_pawnBatchSize      = 0;

static ULONG   g_cfgPid        = 0;
static ULONG64 g_cfgClientBase = 0;

constexpr ULONG64 PHYS_MASK_4K          = 0x000FFFFFFFFFF000ULL;
constexpr ULONG64 PHYS_MASK_2M          = 0x000FFFFFFFE00000ULL;
constexpr ULONG64 PHYS_MASK_1G          = 0x000FFFFFC0000000ULL;

#pragma pack(push, 1)

struct SHARED_CONFIG {
    ULONG64 offEntityList;
    ULONG64 offLocalPlayerPawn;
    ULONG64 offViewMatrix;
    ULONG64 offGameSceneNode;
    ULONG64 offHealth;
    ULONG64 offLifeState;
    ULONG64 offTeamNum;
    ULONG64 offModelState;
    ULONG64 offPlayerPawn;
    ULONG   pid;
    ULONG   pad0;
    ULONG64 clientBase;
    ULONG   magic;    // 0x45535043 = 'ESPC'
    ULONG   ready;
};

struct SHARED_BONE {
    ULONG xBits, yBits, zBits;
};

struct SHARED_ENTITY {
    int         valid;
    int         hp;
    int         team;
    int         pad0;
    SHARED_BONE bones[MAX_BONES];
};

struct SHARED_HEADER {
    volatile LONG seq;
    ULONG         maxEntities;
    ULONG         entityCount;
    ULONG         version;
};

struct SHARED_LOG_ENTRY {
    ULONG tick;
    char  text[LOG_LINE_MAX];
};

struct SHARED_LOG {
    volatile LONG    head;
    volatile LONG    tail;
    SHARED_LOG_ENTRY entries[MAX_LOGS];
};

struct SHARED_BLOCK {
    SHARED_CONFIG cfg;   // первое поле — usermode пишет сюда
    SHARED_HEADER hdr;
    SHARED_LOG    log;
    SHARED_ENTITY ents[256];
};
#pragma pack(pop)

static HANDLE        g_hSharedSection = nullptr;
static PVOID         g_sharedSecObj   = nullptr;
static SHARED_BLOCK* g_sharedVa       = nullptr;
static HANDLE        g_hViewSection   = nullptr;
static PVOID         g_viewSecObj     = nullptr;
static ULONG*        g_viewVa         = nullptr;

// ---- ФИКСИРОВАННЫЕ нейтральные имена (порядок загрузки не важен) ----
static const WCHAR* g_sharedName = L"\\BaseNamedObjects\\W32Sync_5A3F_s";
static const WCHAR* g_viewName   = L"\\BaseNamedObjects\\W32Sync_5A3F_v";

static ULONG64       g_cs2Cr3         = 0;
static ULONG64       g_clientBase     = 0;
static HANDLE        g_cs2Pid         = nullptr;
static ULONG64       g_localPawn      = 0;
static ULONG64       g_entitySystem   = 0;
static volatile LONG g_stop           = 0;
static KEVENT        g_workerEvent;
static PKTHREAD      g_workerThread   = nullptr;

static volatile LONG g_logTick        = 0;
static volatile LONG g_statReads      = 0;
static volatile LONG g_statReadFails  = 0;

static bool          g_offsetsReady   = false;

// ---- TLB ----
constexpr ULONG TLB_SIZE = 256;

struct TlbEntry {
    ULONG64 vaPage;
    ULONG64 physPage;
};

struct TranslationCache {
    TlbEntry entries[TLB_SIZE];
    ULONG64  pml4[512];
    BOOLEAN  pml4Valid;
};

static TranslationCache g_tlb;

static __forceinline void ResetTlbPages() {
    for (ULONG i = 0; i < TLB_SIZE; ++i) {
        g_tlb.entries[i].vaPage   = ~0ULL;
        g_tlb.entries[i].physPage = 0;
    }
}

static __forceinline void ResetTlb() {
    ResetTlbPages();
    g_tlb.pml4Valid = FALSE;
}

// ---------------------------------------------------------------------------
// Генерация рандомного pool tag
// ---------------------------------------------------------------------------
static __forceinline ULONG64 XorShift64(ULONG64& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

static void GenPoolTag() {
    LARGE_INTEGER t; KeQueryTickCount(&t);
    ULONG64 seed = (ULONG64)t.QuadPart ^ __rdtsc();
    if (seed == 0) seed = 0x9E3779B97F4A7C15ULL;

    ULONG tag = 0;
    for (int i = 0; i < 4; ++i) {
        UCHAR c = (UCHAR)(0x20 + (XorShift64(seed) % 0x5F));
        tag |= ((ULONG)c) << (8 * i);
    }
    if (tag == 0) tag = 'Ntfs';
    g_poolTag = tag;
}

// ---------------------------------------------------------------------------
// Мини-форматтер для kernel-mode: без UCRT, без FP
// ---------------------------------------------------------------------------
static char* KAppendChar(char* p, char* end, char c) {
    if (p < end) *p++ = c;
    return p;
}

static char* KAppendStr(char* p, char* end, const char* s) {
    if (!s) s = "(null)";
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char* KAppendUInt(char* p, char* end, unsigned long long v, unsigned base, bool upper) {
    char tmp[32];
    int n = 0;
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) { tmp[n++] = digits[v % base]; v /= base; }
    while (n > 0 && p < end) *p++ = tmp[--n];
    return p;
}

static char* KAppendInt(char* p, char* end, long long v) {
    if (v < 0) {
        p = KAppendChar(p, end, '-');
        return KAppendUInt(p, end, (unsigned long long)(-v), 10, false);
    }
    return KAppendUInt(p, end, (unsigned long long)v, 10, false);
}

static void KVPrintf(char* out, SIZE_T outCap, const char* fmt, va_list ap) {
    if (!out || outCap == 0) return;
    char* p = out;
    char* end = out + outCap - 1;
    while (*fmt && p < end) {
        if (*fmt != '%') { *p++ = *fmt++; continue; }
        ++fmt;
        int lcount = 0;
        while (*fmt == 'l') { ++lcount; ++fmt; }
        bool isLongLong = (lcount >= 2);
        if (*fmt == 'X') { ++fmt;
            unsigned long long v = isLongLong ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
            p = KAppendUInt(p, end, v, 16, true); continue; }
        if (*fmt == 'x') { ++fmt;
            unsigned long long v = isLongLong ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
            p = KAppendUInt(p, end, v, 16, false); continue; }
        if (*fmt == 'u') { ++fmt;
            unsigned long long v = isLongLong ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
            p = KAppendUInt(p, end, v, 10, false); continue; }
        if (*fmt == 'd' || *fmt == 'i') { ++fmt;
            long long v = isLongLong ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            p = KAppendInt(p, end, v); continue; }
        if (*fmt == 'p') { ++fmt;
            p = KAppendStr(p, end, "0x");
            p = KAppendUInt(p, end, (unsigned long long)(ULONG_PTR)va_arg(ap, void*), 16, false);
            continue; }
        if (*fmt == 's') { ++fmt; p = KAppendStr(p, end, va_arg(ap, const char*)); continue; }
        if (*fmt == 'c') { ++fmt; p = KAppendChar(p, end, (char)va_arg(ap, int)); continue; }
        p = KAppendChar(p, end, '%');
        if (*fmt) p = KAppendChar(p, end, *fmt++);
    }
    *p = 0;
}

static void LogRaw(const char* text) {
#ifdef DEBUG_LOG
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[drv] %s\n", text);
#endif
    if (!g_sharedVa) return;
    ULONG tail = (ULONG)InterlockedIncrement(&g_sharedVa->log.tail) - 1;
    ULONG slot = tail % MAX_LOGS;
    g_sharedVa->log.entries[slot].tick = (ULONG)InterlockedIncrement(&g_logTick);
    char* dst = g_sharedVa->log.entries[slot].text;
    const char* src = text;
    SIZE_T n = 0;
    while (*src && n < LOG_LINE_MAX - 1) { dst[n++] = *src++; }
    dst[n] = 0;
}

static void Log(const char* fmt, ...) {
    char buf[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    KVPrintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LogRaw(buf);
}

static __forceinline BOOLEAN ReadPhysicalCached(ULONG64 phys, PVOID buf, SIZE_T size) {
    if (!phys || !buf || !size) return FALSE;
    MM_COPY_ADDRESS src;
    PHYSICAL_ADDRESS pa;
    SIZE_T copied = 0;
    pa.QuadPart = (LONGLONG)phys;
    src.PhysicalAddress = pa;
    NTSTATUS st = MmCopyMemory(buf, src, size, MM_COPY_MEMORY_PHYSICAL, &copied);
    return NT_SUCCESS(st) && copied == size;
}

static __forceinline ULONG64 ReadPhysicalQword(ULONG64 phys) {
    ULONG64 v = 0;
    ReadPhysicalCached(phys, &v, sizeof(v));
    return v;
}

static __forceinline ULONG64 VirtualToPhysical(ULONG64 cr3, ULONG64 va) {
    ULONG64 vaPage = va & ~0xFFFULL;
    ULONG   slot   = (ULONG)((vaPage >> 12) & (TLB_SIZE - 1));

    if (g_tlb.entries[slot].vaPage == vaPage && g_tlb.entries[slot].physPage != 0) {
        return g_tlb.entries[slot].physPage + (va & 0xFFFULL);
    }

    if (!g_tlb.pml4Valid) {
        if (ReadPhysicalCached(cr3 & PHYS_MASK_4K, g_tlb.pml4, sizeof(g_tlb.pml4))) {
            g_tlb.pml4Valid = TRUE;
        }
    }

    ULONG pml4Idx = (ULONG)((va >> 39) & 0x1FF);
    ULONG64 pml4e = g_tlb.pml4Valid
        ? g_tlb.pml4[pml4Idx]
        : ReadPhysicalQword((cr3 & PHYS_MASK_4K) + 8ULL * pml4Idx);
    if (!(pml4e & 1)) return 0;

    ULONG64 pdpte = ReadPhysicalQword((pml4e & PHYS_MASK_4K) + 8ULL * ((va >> 30) & 0x1FF));
    if (!(pdpte & 1)) return 0;
    if (pdpte & 0x80) {
        ULONG64 physBase = (pdpte & PHYS_MASK_1G) + (va & 0x3FFFF000ULL);
        g_tlb.entries[slot].vaPage   = vaPage;
        g_tlb.entries[slot].physPage = physBase;
        return physBase + (va & 0xFFFULL);
    }

    ULONG64 pde = ReadPhysicalQword((pdpte & PHYS_MASK_4K) + 8ULL * ((va >> 21) & 0x1FF));
    if (!(pde & 1)) return 0;
    if (pde & 0x80) {
        ULONG64 physBase = (pde & PHYS_MASK_2M) + (va & 0x1FF000ULL);
        g_tlb.entries[slot].vaPage   = vaPage;
        g_tlb.entries[slot].physPage = physBase;
        return physBase + (va & 0xFFFULL);
    }

    ULONG64 pte = ReadPhysicalQword((pde & PHYS_MASK_4K) + 8ULL * ((va >> 12) & 0x1FF));
    if (!(pte & 1)) return 0;

    ULONG64 physPage = pte & PHYS_MASK_4K;
    g_tlb.entries[slot].vaPage   = vaPage;
    g_tlb.entries[slot].physPage = physPage;
    return physPage + (va & 0xFFFULL);
}

static BOOLEAN ReadCs2Memory(ULONG64 va, PVOID buf, SIZE_T size) {
    if (!g_cs2Cr3 || !va || !buf || !size) return FALSE;
    PUCHAR out = (PUCHAR)buf;
    SIZE_T done = 0;

    while (done < size) {
        ULONG64 curVa = va + done;
        ULONG64 startPhys = VirtualToPhysical(g_cs2Cr3, curVa);
        if (!startPhys) {
            InterlockedIncrement(&g_statReadFails);
            return FALSE;
        }

        SIZE_T chunk = 0x1000 - (curVa & 0xFFF);
        if (chunk > size - done) chunk = size - done;

        while (done + chunk < size) {
            ULONG64 nextVa = curVa + chunk;
            ULONG64 nextPhys = VirtualToPhysical(g_cs2Cr3, nextVa);
            if (nextPhys != startPhys + chunk) break;

            SIZE_T add = 0x1000;
            if (chunk + add > size - done) add = (size - done) - chunk;
            chunk += add;
        }

        if (!ReadPhysicalCached(startPhys, out + done, chunk)) {
            InterlockedIncrement(&g_statReadFails);
            return FALSE;
        }
        InterlockedIncrement(&g_statReads);
        done += chunk;
    }
    return TRUE;
}

static __forceinline BOOLEAN RdQ(ULONG64 va, PULONG64 out) { return ReadCs2Memory(va, out, sizeof(ULONG64)); }
static __forceinline BOOLEAN RdD(ULONG64 va, PULONG   out) { return ReadCs2Memory(va, out, sizeof(ULONG)); }

static __forceinline ULONG64 GetProcessCr3(PEPROCESS p) {
    ULONG64 cr3 = *(ULONG64*)((PUCHAR)p + 0x28);
    return cr3 & PHYS_MASK_4K;
}

static __forceinline BOOLEAN IsCr3Sane(ULONG64 cr3) {
    if (!cr3) return FALSE;
    if (cr3 & 0xFFF) return FALSE;
    if (cr3 & ~PHYS_MASK_4K) return FALSE;
    return TRUE;
}

static void ClearShared() {
    if (!g_sharedVa) return;
    InterlockedIncrement(&g_sharedVa->hdr.seq);
    KeMemoryBarrier();
    g_sharedVa->hdr.entityCount = 0;
    KeMemoryBarrier();
    InterlockedIncrement(&g_sharedVa->hdr.seq);
}

static void ResetProcessState(const char* reason) {
    Log("reset: %s", reason);
    g_cs2Cr3      = 0;
    g_cs2Pid      = nullptr;
    g_clientBase  = 0;
    g_localPawn   = 0;
    g_entitySystem = 0;
    ResetTlb();
    ClearShared();
}

static BOOLEAN VerifyCr3EachTick() {
    if (!g_cs2Pid) return FALSE;

    PEPROCESS p = nullptr;
    if (!NT_SUCCESS(PsLookupProcessByProcessId(g_cs2Pid, &p)) || !p) {
        ResetProcessState("proc gone");
        return FALSE;
    }

    ULONG64 cr3now = GetProcessCr3(p);
    ObDereferenceObject(p);

    if (!IsCr3Sane(cr3now)) {
        ResetProcessState("cr3 insane");
        return FALSE;
    }
    if (cr3now != g_cs2Cr3) {
        g_cs2Cr3 = cr3now;
        g_clientBase = 0;
        ResetTlb();
        Log("cr3 changed");
    }
    return TRUE;
}

constexpr ULONG CACHED_CHUNKS = 4;
static UCHAR*         g_chunkCache   = nullptr;
static UCHAR*         g_bonesScratch = nullptr;
static UCHAR*         g_pawnScratch  = nullptr;
static SHARED_ENTITY* g_localEnts    = nullptr;
static SHARED_ENTITY* g_prevEnts     = nullptr;

static __forceinline ULONG64 FastEntityByIndex(const ULONG64* chunkPtrs,
                                                const BOOLEAN* chunkValid,
                                                ULONG idx) {
    if (idx > 0x7FFE) return 0;
    ULONG c = idx >> 9;
    ULONG subIdx = idx & 0x1FF;
    if (c >= CACHED_CHUNKS) return 0;
    if (!chunkValid[c] || !g_chunkCache) return 0;
    const UCHAR* entryPtr = g_chunkCache + (SIZE_T)c * 512 * ENTITY_STRIDE
                          + (SIZE_T)subIdx * ENTITY_STRIDE;
    ULONG ident = *(const ULONG*)(entryPtr + 0x10);
    if ((ident & 0x7FFF) != idx) return 0;
    return *(const ULONG64*)entryPtr;
}

static void TryLoadConfig() {
    if (g_offsetsReady || !g_sharedVa) return;
    const SHARED_CONFIG* c = &g_sharedVa->cfg;
    if (c->magic != 0x45535043 || c->ready != 1) return;

    g_offEntityList      = c->offEntityList;
    g_offLocalPlayerPawn = c->offLocalPlayerPawn;
    g_offViewMatrix      = c->offViewMatrix;
    g_offGameSceneNode   = c->offGameSceneNode;
    g_offHealth          = c->offHealth;
    g_offLifeState       = c->offLifeState;
    g_offTeamNum         = c->offTeamNum;
    g_offModelState      = c->offModelState;
    g_offPlayerPawn      = c->offPlayerPawn;

    g_pawnBatchBase      = g_offGameSceneNode;
    g_pawnBatchSize      = (g_offTeamNum + sizeof(UCHAR)) - g_pawnBatchBase;

    g_cfgPid             = c->pid;
    g_cfgClientBase      = c->clientBase;

    if (g_pawnBatchSize == 0 || g_pawnBatchSize > 0x400) {
        g_pawnBatchSize = 0xB8;
    }
    g_offsetsReady = true;
    Log("cfg loaded");
}

static void Tick() {
    ResetTlb();

    static ULONG s_tickCount = 0;
    static bool  s_processLogged = false;
    static ULONG s_emptyGraceTicks = 0;
    static ULONG s_critFailTicks = 0;
    ULONG tick = ++s_tickCount;
    bool logNow = ((tick % 500) == 0);

    if (!g_offsetsReady) {
        TryLoadConfig();
        if (!g_offsetsReady) {
            ClearShared();
            return;
        }
    }

    if (g_cs2Pid) {
        if (!VerifyCr3EachTick()) return;
    }

    if (!g_cs2Cr3) {
        if (!g_cfgPid) { ClearShared(); return; }
        PEPROCESS p = nullptr;
        if (!NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)g_cfgPid, &p)) || !p) {
            ClearShared();
            if (logNow) Log("proc not found");
            return;
        }
        ULONG64 cr3 = GetProcessCr3(p);
        if (!IsCr3Sane(cr3)) {
            ObDereferenceObject(p);
            ClearShared();
            if (logNow) Log("cr3 insane");
            return;
        }
        g_cs2Cr3 = cr3;
        g_cs2Pid = (HANDLE)(ULONG_PTR)g_cfgPid;
        ObDereferenceObject(p);
        ResetTlb();

        if (!s_processLogged) {
            Log("proc ok");
            s_processLogged = true;
        }
    }

    if (!g_clientBase) {
        if (!g_cfgClientBase) { ClearShared(); return; }
        g_clientBase = g_cfgClientBase;
    }

    ULONG64 lp = 0, es = 0;
    BOOLEAN gotLp = RdQ(g_clientBase + g_offLocalPlayerPawn, &lp);
    BOOLEAN gotEs = RdQ(g_clientBase + g_offEntityList, &es);

    if (!gotLp || !gotEs) {
        if (++s_critFailTicks >= 4) {
            s_critFailTicks = 0;
            ResetProcessState("crit reads failed");
        }
        return;
    }
    s_critFailTicks = 0;

    g_localPawn = lp;
    g_entitySystem = es;

    ULONG vmRaw[16] = {};
    BOOLEAN vmOk = ReadCs2Memory(g_clientBase + g_offViewMatrix, vmRaw, sizeof(vmRaw));

    if (!lp || !es) {
        if (++s_emptyGraceTicks >= 8) {
            ClearShared();
        }
        return;
    }

    UCHAR myTeam = 0;
    ReadCs2Memory(lp + g_offTeamNum, &myTeam, sizeof(myTeam));

    ULONG64 chunkPtrs[CACHED_CHUNKS]   = {};
    BOOLEAN chunkValid[CACHED_CHUNKS]  = {};
    if (g_chunkCache) {
        for (ULONG c = 0; c < CACHED_CHUNKS; ++c) {
            if (RdQ(es + 0x10 + 8ULL * c, &chunkPtrs[c]) && chunkPtrs[c]) {
                chunkValid[c] = ReadCs2Memory(
                    chunkPtrs[c],
                    g_chunkCache + (SIZE_T)c * 512 * ENTITY_STRIDE,
                    512 * ENTITY_STRIDE);
            }
        }
    }

    ULONG out = 0, ctrlOk = 0, pawnOk = 0, aliveOk = 0, boneOk = 0;

    for (ULONG i = 1; i <= 64 && out < MAX_ENTITIES; ++i) {
        ULONG slotIdx = i - 1;
        ULONG64 ctrl = FastEntityByIndex(chunkPtrs, chunkValid, i);
        if (!ctrl) continue;
        ++ctrlOk;

        ULONG hPawn = 0;
        if (!RdD(ctrl + g_offPlayerPawn, &hPawn) || !hPawn || hPawn == 0xFFFFFFFF) continue;

        ULONG64 pawn = FastEntityByIndex(chunkPtrs, chunkValid, hPawn & 0x7FFF);
        if (!pawn || pawn == lp) continue;
        ++pawnOk;

        if (!g_pawnScratch) continue;
        if (!ReadCs2Memory(pawn + g_pawnBatchBase, g_pawnScratch, g_pawnBatchSize)) continue;

        ULONG64 scene = *(ULONG64*)(g_pawnScratch + (g_offGameSceneNode - g_pawnBatchBase));
        int     hp    = *(int*)    (g_pawnScratch + (g_offHealth        - g_pawnBatchBase));
        UCHAR   ls    = *(UCHAR*)  (g_pawnScratch + (g_offLifeState      - g_pawnBatchBase));
        UCHAR   team  = *(UCHAR*)  (g_pawnScratch + (g_offTeamNum        - g_pawnBatchBase));

        if (hp <= 0 || hp > 200 || ls != 0 || team == myTeam || !scene) {
            if (g_prevEnts) g_prevEnts[slotIdx].valid = 0;
            continue;
        }

        ++aliveOk;

        ULONG64 bMat = 0;
        bool bonesReadSuccess = false;
        if (RdQ(scene + g_offModelState + 0x80, &bMat) && bMat && g_bonesScratch) {
            if (ReadCs2Memory(bMat, g_bonesScratch, MAX_BONES * 0x20)) {
                const ULONG* headBone = (const ULONG*)(g_bonesScratch + (SIZE_T)6 * 0x20);
                if (headBone[0] != 0 || headBone[1] != 0 || headBone[2] != 0) {
                    bonesReadSuccess = true;
                }
            }
        }

        if (bonesReadSuccess && g_localEnts) {
            ++boneOk;
            SHARED_ENTITY& dst = g_localEnts[out];
            dst.valid = 1;
            dst.hp    = hp;
            dst.team  = (int)team;
            dst.pad0  = 0;
            for (ULONG b = 0; b < MAX_BONES; ++b) {
                const ULONG* srcBone = (const ULONG*)(g_bonesScratch + (SIZE_T)b * 0x20);
                dst.bones[b].xBits = srcBone[0];
                dst.bones[b].yBits = srcBone[1];
                dst.bones[b].zBits = srcBone[2];
            }
            if (g_prevEnts) {
                RtlCopyMemory(&g_prevEnts[slotIdx], &dst, sizeof(SHARED_ENTITY));
            }
            ++out;
        } else if (g_prevEnts && g_prevEnts[slotIdx].valid && g_localEnts) {
            SHARED_ENTITY& dst = g_localEnts[out];
            RtlCopyMemory(&dst, &g_prevEnts[slotIdx], sizeof(SHARED_ENTITY));
            dst.hp   = hp;
            dst.team = (int)team;
            ++out;
        }
    }

    if (out == 0) {
        if (++s_emptyGraceTicks < 8) {
            if (vmOk && g_viewVa && g_sharedVa) {
                InterlockedIncrement(&g_sharedVa->hdr.seq);
                KeMemoryBarrier();
                RtlCopyMemory(g_viewVa, vmRaw, sizeof(vmRaw));
                KeMemoryBarrier();
                InterlockedIncrement(&g_sharedVa->hdr.seq);
            }
            return;
        }
    } else {
        s_emptyGraceTicks = 0;
    }

    if (g_sharedVa && g_localEnts) {
        InterlockedIncrement(&g_sharedVa->hdr.seq);
        KeMemoryBarrier();

        if (vmOk && g_viewVa) {
            RtlCopyMemory(g_viewVa, vmRaw, sizeof(vmRaw));
        }
        if (out > 0) {
            RtlCopyMemory(g_sharedVa->ents, g_localEnts, (SIZE_T)out * sizeof(SHARED_ENTITY));
        }
        g_sharedVa->hdr.entityCount = out;
        g_sharedVa->hdr.version = 1;

        KeMemoryBarrier();
        InterlockedIncrement(&g_sharedVa->hdr.seq);
    }

    if (logNow) {
        Log("tick: %u/%u/%u/%u/%u", ctrlOk, pawnOk, aliveOk, boneOk, out);
    }
}

static VOID WorkerThread(PVOID) {
    KeSetPriorityThread(KeGetCurrentThread(), 8);

    LARGE_INTEGER interval;
    interval.QuadPart = -20000LL;  // 2 мс = 500 Гц ровно

    for (;;) {
        if (InterlockedCompareExchange(&g_stop, 0, 0)) break;
        Tick();
        KeWaitForSingleObject(&g_workerEvent, Executive, KernelMode, FALSE, &interval);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS BuildSafeSd(PSECURITY_DESCRIPTOR sd) {
    NTSTATUS st = RtlCreateSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(st)) return st;
    return RtlSetDaclSecurityDescriptor(sd, TRUE, NULL, FALSE);
}

static NTSTATUS OpenOrCreateSection(PCWSTR nameStr, HANDLE* outHandle, LARGE_INTEGER size) {
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    SECURITY_DESCRIPTOR sd;

    NTSTATUS st = BuildSafeSd(&sd);
    if (!NT_SUCCESS(st)) return st;

    RtlInitUnicodeString(&name, nameStr);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, &sd);

    st = ZwCreateSection(outHandle, SECTION_ALL_ACCESS, &oa, &size, PAGE_READWRITE, SEC_COMMIT, nullptr);
    if (st == STATUS_OBJECT_NAME_COLLISION) {
        InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
        st = ZwOpenSection(outHandle, SECTION_ALL_ACCESS, &oa);
    }
    return st;
}

static NTSTATUS MapSectionSystem(HANDLE h, SIZE_T size, PVOID* outVa, PVOID* outObj) {
    PVOID obj = nullptr;
    NTSTATUS st = ObReferenceObjectByHandle(h, SECTION_MAP_READ | SECTION_MAP_WRITE,
                                            nullptr, KernelMode, &obj, nullptr);
    if (!NT_SUCCESS(st)) return st;

    PVOID base = nullptr;
    SIZE_T vs = size;
    st = MmMapViewInSystemSpace(obj, &base, &vs);
    if (!NT_SUCCESS(st)) {
        ObDereferenceObject(obj);
        return st;
    }

    *outVa = base;
    *outObj = obj;
    return STATUS_SUCCESS;
}

static void UnmapSectionSystem(PVOID* va, PVOID* obj) {
    if (*va)  { MmUnmapViewInSystemSpace(*va); *va = nullptr; }
    if (*obj) { ObDereferenceObject(*obj);     *obj = nullptr; }
}

static NTSTATUS CreateShared() {
    NTSTATUS st;
    LARGE_INTEGER sz;
    PVOID base = nullptr;
    SECTION_BASIC_INFORMATION sbi = {};

    if (!g_sharedVa) {
        sz.QuadPart = sizeof(SHARED_BLOCK);
        st = OpenOrCreateSection(g_sharedName, &g_hSharedSection, sz);
        if (!NT_SUCCESS(st)) return st;

        RtlZeroMemory(&sbi, sizeof(sbi));
        st = ZwQuerySection(g_hSharedSection, SectionBasicInformation, &sbi, sizeof(sbi), nullptr);
        if (NT_SUCCESS(st) && sbi.Size.QuadPart < (LONGLONG)sizeof(SHARED_BLOCK)) {
            ZwClose(g_hSharedSection);
            g_hSharedSection = nullptr;
            return STATUS_INVALID_PARAMETER;
        }

        st = MapSectionSystem(g_hSharedSection, sizeof(SHARED_BLOCK), &base, &g_sharedSecObj);
        if (!NT_SUCCESS(st)) return st;

        g_sharedVa = (SHARED_BLOCK*)base;
        g_sharedVa->hdr.version = 1;
        g_sharedVa->hdr.maxEntities = MAX_ENTITIES;
    }

    if (!g_viewVa) {
        sz.QuadPart = 0x1000;
        st = OpenOrCreateSection(g_viewName, &g_hViewSection, sz);
        if (!NT_SUCCESS(st)) return st;

        st = MapSectionSystem(g_hViewSection, 0x1000, &base, &g_viewSecObj);
        if (!NT_SUCCESS(st)) return st;

        g_viewVa = (ULONG*)base;
    }

    return STATUS_SUCCESS;
}

static void DestroyShared() {
    UnmapSectionSystem((PVOID*)&g_viewVa, &g_viewSecObj);
    if (g_hViewSection)   { ZwClose(g_hViewSection);   g_hViewSection = nullptr; }
    UnmapSectionSystem((PVOID*)&g_sharedVa, &g_sharedSecObj);
    if (g_hSharedSection) { ZwClose(g_hSharedSection); g_hSharedSection = nullptr; }
    if (g_chunkCache)     { ExFreePoolWithTag(g_chunkCache, g_poolTag); g_chunkCache = nullptr; }
    if (g_bonesScratch)   { ExFreePoolWithTag(g_bonesScratch, g_poolTag); g_bonesScratch = nullptr; }
    if (g_pawnScratch)    { ExFreePoolWithTag(g_pawnScratch, g_poolTag); g_pawnScratch = nullptr; }
    if (g_localEnts)      { ExFreePoolWithTag(g_localEnts, g_poolTag); g_localEnts = nullptr; }
    if (g_prevEnts)       { ExFreePoolWithTag(g_prevEnts, g_poolTag); g_prevEnts = nullptr; }
}

static VOID DriverUnloadFn(PDRIVER_OBJECT) {
    InterlockedExchange(&g_stop, 1);
    KeSetEvent(&g_workerEvent, 0, FALSE);
    if (g_workerThread) {
        KeWaitForSingleObject(g_workerThread, Executive, KernelMode, FALSE, nullptr);
        ObDereferenceObject(g_workerThread);
        g_workerThread = nullptr;
    }
    DestroyShared();

    ExSetTimerResolution(0, FALSE);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING) {
    g_hSharedSection = nullptr; g_sharedSecObj = nullptr; g_sharedVa = nullptr;
    g_hViewSection   = nullptr; g_viewSecObj   = nullptr; g_viewVa   = nullptr;
    g_cs2Cr3 = 0; g_clientBase = 0; g_cs2Pid = nullptr;
    g_localPawn = 0; g_entitySystem = 0;
    g_stop = 0; g_workerThread = nullptr;
    g_logTick = 0; g_statReads = 0; g_statReadFails = 0;
    g_chunkCache = nullptr;
    g_bonesScratch = nullptr;
    g_pawnScratch = nullptr;
    g_localEnts = nullptr;
    g_prevEnts = nullptr;
    g_offsetsReady = false;
    g_cfgPid = 0;
    g_cfgClientBase = 0;
    ResetTlb();

    GenPoolTag();

    g_chunkCache = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                           CACHED_CHUNKS * 512 * ENTITY_STRIDE, g_poolTag);
    if (!g_chunkCache) return STATUS_INSUFFICIENT_RESOURCES;

    g_bonesScratch = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, MAX_BONES * 0x20, g_poolTag);
    if (!g_bonesScratch) { DestroyShared(); return STATUS_INSUFFICIENT_RESOURCES; }

    g_pawnScratch = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, 0xB8, g_poolTag);
    if (!g_pawnScratch) { DestroyShared(); return STATUS_INSUFFICIENT_RESOURCES; }

    g_localEnts = (SHARED_ENTITY*)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                  sizeof(SHARED_ENTITY) * MAX_ENTITIES, g_poolTag);
    if (!g_localEnts) { DestroyShared(); return STATUS_INSUFFICIENT_RESOURCES; }

    g_prevEnts = (SHARED_ENTITY*)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                 sizeof(SHARED_ENTITY) * MAX_ENTITIES, g_poolTag);
    if (!g_prevEnts) { DestroyShared(); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(g_prevEnts, sizeof(SHARED_ENTITY) * MAX_ENTITIES);

    NTSTATUS st = CreateShared();
    if (!NT_SUCCESS(st)) {
        DestroyShared();
        return st;
    }

    ULONG curRes = ExSetTimerResolution(10000, TRUE);

    KeInitializeEvent(&g_workerEvent, NotificationEvent, FALSE);

    HANDLE h = nullptr;
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, nullptr, nullptr, nullptr, WorkerThread, nullptr);
    if (!NT_SUCCESS(st)) {
        DestroyShared();
        return st;
    }

    st = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                   (PVOID*)&g_workerThread, nullptr);
    ZwClose(h);
    if (!NT_SUCCESS(st)) {
        InterlockedExchange(&g_stop, 1);
        KeSetEvent(&g_workerEvent, 0, FALSE);
        return st;
    }

#ifdef DEBUG_LOG
    Log("init ok res=%u", curRes);
#else
    (void)curRes;
#endif

    if (drv) drv->DriverUnload = DriverUnloadFn;
    return STATUS_SUCCESS;
}
