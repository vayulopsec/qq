// esp_drv.cpp — CS2 ESP kernel driver (C++20, manual-map ready, no SEH, no STL/RTTI/exceptions)
// build: MSVC + WDK, /std:c++20 /kernel /O2 /Ob3 /Oi /Ot /GS- /GR- /EHs-c- /EHc- /Zl

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

constexpr ULONG   POOL_TAG       = 'spSE';
constexpr ULONG   MAX_ENTITIES   = 64;
constexpr ULONG   MAX_BONES      = 24;
constexpr ULONG   MAX_LOGS       = 256;
constexpr ULONG   LOG_LINE_MAX   = 128;
constexpr ULONG   ENTITY_STRIDE  = 112; // 0x70

// ---- CS2 offsets (проверены по offsets_2.hpp и client_dll_2.hpp) ----
constexpr ULONG64 OFF_dwEntityList      = 0x271A908ULL;
constexpr ULONG64 OFF_dwLocalPlayerPawn = 0x2565718ULL;
constexpr ULONG64 OFF_dwViewMatrix      = 0x256B020ULL;
constexpr ULONG64 OFF_m_pGameSceneNode  = 0x330ULL;
constexpr ULONG64 OFF_m_iHealth         = 0x34CULL;
constexpr ULONG64 OFF_m_lifeState       = 0x354ULL;
constexpr ULONG64 OFF_m_iTeamNum        = 0x3E7ULL;
constexpr ULONG64 OFF_m_modelState      = 0x140ULL;
constexpr ULONG64 OFF_m_hPlayerPawn     = 0x92CULL;

// Диапазон пакетного чтения павна: от 0x330 (m_pGameSceneNode) до 0x3E7 + 1 (m_iTeamNum)
constexpr ULONG64 PAWN_BATCH_BASE       = OFF_m_pGameSceneNode;
constexpr SIZE_T  PAWN_BATCH_SIZE       = (OFF_m_iTeamNum + sizeof(UCHAR)) - PAWN_BATCH_BASE; // 0xB8 байт

constexpr ULONG64 PHYS_MASK_4K          = 0x000FFFFFFFFFF000ULL;
constexpr ULONG64 PHYS_MASK_2M          = 0x000FFFFFFFE00000ULL;
constexpr ULONG64 PHYS_MASK_1G          = 0x000FFFFFC0000000ULL;

#pragma pack(push, 1)
struct SHARED_BONE {
    ULONG xBits, yBits, zBits; // Храним как сырые 32-битные поля (float на стороне usermode)
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
    SHARED_HEADER hdr;
    SHARED_LOG    log;
    SHARED_ENTITY ents[MAX_ENTITIES];
};
#pragma pack(pop)

static HANDLE        g_hSharedSection = nullptr;
static PVOID         g_sharedSecObj   = nullptr;
static SHARED_BLOCK* g_sharedVa       = nullptr;
static HANDLE        g_hViewSection   = nullptr;
static PVOID         g_viewSecObj     = nullptr;
static ULONG*        g_viewVa         = nullptr;

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

// ---- Локальный кэш трансляции адресов (Direct-mapped TLB 256 + PML4) ----
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

// Вызывается каждый тик: сбрасывает только страницы, НЕ трогая PML4
static __forceinline void ResetTlb() {
    for (ULONG i = 0; i < TLB_SIZE; ++i) {
        g_tlb.entries[i].vaPage   = ~0ULL;
        g_tlb.entries[i].physPage = 0;
    }
}

// Вызывается при смене CR3, ошибке чтения или старте драйвера
static __forceinline void ResetTlbFull() {
    ResetTlb();
    g_tlb.pml4Valid = FALSE;
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
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[esp_drv] %s\n", text);
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
            ResetTlbFull();                     // форсим перечитывание PML4 и очистку TLB
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
            ResetTlbFull();
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

// ---- Поиск процесса и базы client.dll ----
typedef struct _SYSTEM_PROCESS_INFO {
    ULONG NextEntryOffset; ULONG NumberOfThreads; LARGE_INTEGER WorkingSetPrivateSize; ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark; ULONGLONG CycleTime; LARGE_INTEGER CreateTime; LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime; UNICODE_STRING ImageName; KPRIORITY BasePriority; HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId; ULONG HandleCount; ULONG SessionId; ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize; SIZE_T VirtualSize; ULONG PageFaultCount; SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize; SIZE_T QuotaPeakPagedPoolUsage; SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage; SIZE_T QuotaNonPagedPoolUsage; SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage; SIZE_T PrivatePageCount; LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount; LARGE_INTEGER OtherOperationCount; LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount; LARGE_INTEGER OtherTransferCount;
} SYSTEM_PROCESS_INFO, *PSYSTEM_PROCESS_INFO;

static PEPROCESS FindCs2Process(HANDLE* outPid = nullptr) {
    ULONG size = 0x20000;
    PVOID buf = nullptr;
    NTSTATUS st = STATUS_INFO_LENGTH_MISMATCH;
    for (int i = 0; i < 4; ++i) {
        buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, POOL_TAG);
        if (!buf) return nullptr;
        st = ZwQuerySystemInformation(5, buf, size, &size);
        if (NT_SUCCESS(st)) break;
        ExFreePoolWithTag(buf, POOL_TAG);
        buf = nullptr;
        if (st != STATUS_INFO_LENGTH_MISMATCH) return nullptr;
        size *= 2;
    }
    if (!buf) return nullptr;

    UNICODE_STRING target;
    RtlInitUnicodeString(&target, L"cs2.exe");
    PSYSTEM_PROCESS_INFO spi = (PSYSTEM_PROCESS_INFO)buf;
    PEPROCESS found = nullptr;
    HANDLE foundPid = nullptr;

    for (;;) {
        if (spi->ImageName.Buffer && spi->ImageName.Length) {
            if (RtlEqualUnicodeString(&spi->ImageName, &target, TRUE)) {
                if (NT_SUCCESS(PsLookupProcessByProcessId(spi->UniqueProcessId, &found))) {
                    foundPid = spi->UniqueProcessId;
                    break;
                }
            }
        }
        if (!spi->NextEntryOffset) break;
        spi = (PSYSTEM_PROCESS_INFO)((PUCHAR)spi + spi->NextEntryOffset);
    }
    ExFreePoolWithTag(buf, POOL_TAG);
    if (outPid) *outPid = foundPid;
    return found;
}

static __forceinline ULONG64 GetProcessCr3(PEPROCESS p) {
    ULONG64 cr3 = *(ULONG64*)((PUCHAR)p + 0x28);
    return cr3 & PHYS_MASK_4K;   // срезаем PCID/флаги
}

typedef struct _LDR_ENTRY_OFF {
    LIST_ENTRY InLoadOrderLinks; PVOID ExceptionTable; ULONG ExceptionTableSize; PVOID GpValue;
    PVOID NonPagedDebugInfo; PVOID DllBase; PVOID EntryPoint; ULONG SizeOfImage;
    UNICODE_STRING FullDllName; UNICODE_STRING BaseDllName;
} LDR_ENTRY_OFF;

static ULONG64 FindModuleBase(PEPROCESS proc, PCWSTR moduleName, bool verbose) {
    UNICODE_STRING fnName;
    RtlInitUnicodeString(&fnName, L"PsGetProcessPeb");
    typedef PVOID(*fnPeb)(PEPROCESS);
    fnPeb fn = (fnPeb)MmGetSystemRoutineAddress(&fnName);
    if (!fn) { if (verbose) Log("FMB: PsGetProcessPeb not resolved"); return 0; }

    ULONG64 peb = (ULONG64)fn(proc);
    if (verbose) Log("FMB: peb=%llx", peb);
    if (!peb) return 0;

    UNICODE_STRING want;
    RtlInitUnicodeString(&want, moduleName);

    ULONG64 ldr = 0;
    if (!RdQ(peb + 0x18, &ldr) || !ldr) {
        if (verbose) Log("FMB: ldr read fail/null (client.dll not loaded yet?)");
        return 0;
    }
    if (verbose) Log("FMB: ldr=%llx", ldr);

    ULONG64 head = ldr + 0x10;
    ULONG64 cur = 0;
    if (!RdQ(head, &cur)) {
        if (verbose) Log("FMB: head read fail");
        return 0;
    }
    if (verbose) Log("FMB: head=%llx firstCur=%llx", head, cur);

    int guard = 0;
    for (; guard < 2048 && cur && cur != head; ++guard) {
        LDR_ENTRY_OFF e = {};
        if (!ReadCs2Memory(cur, &e, sizeof(e))) {
            if (verbose) Log("FMB: entry read fail cur=%llx guard=%d", cur, guard);
            return 0;
        }
        if (e.BaseDllName.Buffer && e.BaseDllName.Length) {
            WCHAR n[128] = {};
            USHORT len = e.BaseDllName.Length;
            if (len > sizeof(n) - sizeof(WCHAR)) len = sizeof(n) - sizeof(WCHAR);
            if (ReadCs2Memory((ULONG64)e.BaseDllName.Buffer, n, len)) {
                n[len / sizeof(WCHAR)] = 0;
                UNICODE_STRING un;
                RtlInitUnicodeString(&un, n);
                if (RtlEqualUnicodeString(&un, &want, TRUE)) {
                    // ВАЛИДАЦИЯ: по DllBase реально должен читаться "MZ"
                    ULONG64 base = (ULONG64)e.DllBase;
                    USHORT mz = 0;
                    if (base && ReadCs2Memory(base, &mz, sizeof(mz)) && mz == 0x5A4D) {
                        if (verbose) Log("FMB: found at %llx (guard=%d) MZ ok", base, guard);
                        return base;
                    }
                    if (verbose) Log("FMB: candidate %llx guard=%d MZ fail, skip", base, guard);
                    // не возвращаем — идём дальше по списку
                }
            }
        }
        cur = (ULONG64)e.InLoadOrderLinks.Flink;
        // ВАЛИДАЦИЯ Flink: должен быть в user-space и выровнен на 8
        if (!cur || cur >= 0x7FFFFFFFFFFFULL || (cur & 7)) {
            if (verbose) Log("FMB: bad Flink=%llx guard=%d, break", cur, guard);
            break;
        }
    }
    if (verbose) Log("FMB: not found guard=%d cur=%llx", guard, cur);
    return 0;
}

static void ClearShared() {
    if (!g_sharedVa) return;
    InterlockedIncrement(&g_sharedVa->hdr.seq);
    KeMemoryBarrier();
    g_sharedVa->hdr.entityCount = 0;
    KeMemoryBarrier();
    InterlockedIncrement(&g_sharedVa->hdr.seq);
}

constexpr ULONG CACHED_CHUNKS = 3; // индексы 0..1535 (для 5v5 хватает 2 -> 0..1023)
static UCHAR*         g_chunkCache   = nullptr; // CACHED_CHUNKS * 512 * 112
static UCHAR*         g_bonesScratch = nullptr; // MAX_BONES * 0x20 = 768 байт
static UCHAR*         g_pawnScratch  = nullptr; // PAWN_BATCH_SIZE = 0xB8 байт
static SHARED_ENTITY* g_localEnts    = nullptr; // Сборка текущего тика (вне Seqlock)
static SHARED_ENTITY* g_prevEnts     = nullptr; // Кэш костей по слоту игрока (1..64) на случай промаха чтения

struct SlotPtrCache {
    ULONG64 ctrl;
    ULONG   hPawn;
    ULONG64 pawn;
    ULONG64 scene;
    ULONG64 bMat;
    ULONG   lastUpdateTick;
};

static SlotPtrCache g_slotCache[MAX_ENTITIES] = {};

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

static void Tick() {
    ResetTlb();

    static ULONG s_tickCount = 0;
    static ULONG s_lastModuleAttempt = 0;
    static bool  s_processLogged = false;
    static ULONG s_emptyGraceTicks = 0;
    ULONG tick = ++s_tickCount;
    bool logNow = ((tick % 500) == 0);

    // 1) Поиск процесса
    if (!g_cs2Cr3) {
        HANDLE pid = nullptr;
        PEPROCESS p = FindCs2Process(&pid);
        if (!p) {
            ClearShared();
            if (logNow) Log("cs2.exe not found");
            return;
        }
        g_cs2Cr3 = GetProcessCr3(p);
        g_cs2Pid = pid;
        ObDereferenceObject(p);
        ResetTlbFull();

        if (!s_processLogged) {
            Log("process found: pid=%p cr3=%llx", g_cs2Pid, g_cs2Cr3);
            s_processLogged = true;
        }
        if (!g_cs2Cr3) {
            ClearShared();
            return;
        }
    }

    // 2) Поиск/перепроверка модуля client.dll
    bool needModuleCheck = (!g_clientBase) || ((tick - s_lastModuleAttempt) >= 64);

    static ULONG s_mzFailCount = 0;
    if (g_clientBase && (tick & 0x3F) == 0) {
        USHORT mz = 0;
        if (!ReadCs2Memory(g_clientBase, &mz, sizeof(mz)) || mz != 0x5A4D) {
            if (++s_mzFailCount >= 3) {
                s_mzFailCount = 0;
                Log("client.dll MZ check failed 3x, resetting clientBase");
                g_clientBase = 0;          // НЕ сбрасываем g_cs2Cr3
                ResetTlbFull();
                needModuleCheck = true;
            }
        } else {
            s_mzFailCount = 0;
        }
    }

    if (needModuleCheck && !g_clientBase) {
        s_lastModuleAttempt = tick;
        PEPROCESS p = nullptr;
        if (NT_SUCCESS(PsLookupProcessByProcessId(g_cs2Pid, &p))) {
            bool verbose = (g_clientBase == 0);
            g_clientBase = FindModuleBase(p, L"client.dll", verbose);
            ObDereferenceObject(p);
        }
        if (g_clientBase) {
            Log("client.dll found: base=%llx", g_clientBase);
        } else if (logNow || (tick - s_lastModuleAttempt) < 2) {
            Log("module rediscover: clientBase=0 (client.dll not loaded yet)");
        }
    }

    if (!g_clientBase) {
        ClearShared();
        return;
    }

    // 3) Чтение данных игры (полностью ВНЕ Seqlock)
    ULONG64 lp = 0, es = 0;
    RdQ(g_clientBase + OFF_dwLocalPlayerPawn, &lp);
    RdQ(g_clientBase + OFF_dwEntityList, &es);
    g_localPawn = lp;
    g_entitySystem = es;

    ULONG vmRaw[16] = {};
    BOOLEAN vmOk = ReadCs2Memory(g_clientBase + OFF_dwViewMatrix, vmRaw, sizeof(vmRaw));

    if (!lp || !es) {
        if (++s_emptyGraceTicks >= 8) {
            ClearShared();
        }
        return;
    }

    UCHAR myTeam = 0;
    ReadCs2Memory(lp + OFF_m_iTeamNum, &myTeam, sizeof(myTeam));

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

    ULONG out = 0, ctrlOk = 0, pawnOk = 0, aliveOk = 0, boneOk = 0, maxPawn = 0;

    for (ULONG i = 1; i <= 64 && out < MAX_ENTITIES; ++i) {
        ULONG slotIdx = i - 1;
        SlotPtrCache& sc = g_slotCache[slotIdx];

        ULONG64 ctrl = FastEntityByIndex(chunkPtrs, chunkValid, i);
        if (!ctrl) {
            sc.ctrl = 0;
            if (g_prevEnts) g_prevEnts[slotIdx].valid = 0;
            continue;
        }
        ++ctrlOk;

        // Обновляем связку ctrl -> hPawn раз в 20 тиков или при смене контроллера/отсутствии павна
        bool refreshSlot = (ctrl != sc.ctrl) || !sc.pawn || ((tick - sc.lastUpdateTick) >= 20);
        if (refreshSlot) {
            ULONG hPawn = 0;
            if (!RdD(ctrl + OFF_m_hPlayerPawn, &hPawn) || !hPawn || hPawn == 0xFFFFFFFF) {
                sc.ctrl = ctrl;
                sc.pawn = 0;
                if (g_prevEnts) g_prevEnts[slotIdx].valid = 0;
                continue;
            }
            sc.ctrl           = ctrl;
            sc.hPawn          = hPawn;
            sc.lastUpdateTick = tick - (i & 7); // размазываем обновления по тикам
        }

        ULONG pawnIdx = sc.hPawn & 0x7FFF;
        if (pawnIdx > maxPawn) maxPawn = pawnIdx;

        ULONG64 pawn = FastEntityByIndex(chunkPtrs, chunkValid, pawnIdx);
        if (!pawn || pawn == lp) {
            sc.pawn = 0;
            if (g_prevEnts) g_prevEnts[slotIdx].valid = 0;
            continue;
        }
        if (pawn != sc.pawn) {
            sc.pawn  = pawn;
            sc.scene = 0;
            sc.bMat  = 0;
        }
        ++pawnOk;

        if (!g_pawnScratch) continue;
        if (!ReadCs2Memory(pawn + PAWN_BATCH_BASE, g_pawnScratch, PAWN_BATCH_SIZE)) {
            sc.pawn = 0;
            continue;
        }

        ULONG64 scene = *(ULONG64*)(g_pawnScratch + (OFF_m_pGameSceneNode - PAWN_BATCH_BASE));
        int     hp    = *(int*)    (g_pawnScratch + (OFF_m_iHealth        - PAWN_BATCH_BASE));
        UCHAR   ls    = *(UCHAR*)  (g_pawnScratch + (OFF_m_lifeState      - PAWN_BATCH_BASE));
        UCHAR   team  = *(UCHAR*)  (g_pawnScratch + (OFF_m_iTeamNum       - PAWN_BATCH_BASE));

        if (hp <= 0 || hp > 200 || ls != 0 || team == myTeam || !scene) {
            sc.bMat = 0;
            if (g_prevEnts) g_prevEnts[slotIdx].valid = 0;
            continue;
        }

        ++aliveOk;

        // Обновляем bMat только при смене scene, раз в 20 тиков или если bMat ещё не закэширован
        if (scene != sc.scene || !sc.bMat || refreshSlot) {
            ULONG64 newBMat = 0;
            if (RdQ(scene + OFF_m_modelState + 0x80, &newBMat) && newBMat) {
                sc.scene = scene;
                sc.bMat  = newBMat;
            } else {
                sc.bMat = 0;
            }
        }

        ULONG64 bMat = sc.bMat;
        bool bonesReadSuccess = false;
        if (bMat && g_bonesScratch) {
            if (ReadCs2Memory(bMat, g_bonesScratch, MAX_BONES * 0x20)) {
                // Проверяем, что кость головы (индекс 6 или 7) не нулевая
                const ULONG* headBone = (const ULONG*)(g_bonesScratch + (SIZE_T)6 * 0x20);
                if (headBone[0] != 0 || headBone[1] != 0 || headBone[2] != 0) {
                    bonesReadSuccess = true;
                }
            }
            if (!bonesReadSuccess) {
                sc.bMat = 0; // если кости нулевые или адрес протух — сбросим кэш bMat
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
            // Если на этом тике чтение костей сорвалось, используем кэш прошлого тика для этого слота
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

    // 4) Мгновенное копирование готового буфера под Seqlock
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
        Log("tick: ctrl=%u pawn=%u alive=%u bone=%u out=%u maxPawn=%u reads=%u fails=%u",
            ctrlOk, pawnOk, aliveOk, boneOk, out, maxPawn,
            (ULONG)g_statReads, (ULONG)g_statReadFails);
    }
}

static VOID WorkerThread(PVOID) {
    KeSetPriorityThread(KeGetCurrentThread(), 8);   // 8 = NORMAL_PRIORITY в терминах ядра

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
        st = OpenOrCreateSection(L"\\BaseNamedObjects\\Cs2EspShared_v1", &g_hSharedSection, sz);
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
        g_sharedVa->hdr.maxEntities = MAX_ENTITIES;   // 64
    }

    if (!g_viewVa) {
        sz.QuadPart = 0x1000;
        st = OpenOrCreateSection(L"\\BaseNamedObjects\\Cs2EspView_v1", &g_hViewSection, sz);
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
    if (g_chunkCache)     { ExFreePoolWithTag(g_chunkCache, POOL_TAG); g_chunkCache = nullptr; }
    if (g_bonesScratch)   { ExFreePoolWithTag(g_bonesScratch, POOL_TAG); g_bonesScratch = nullptr; }
    if (g_pawnScratch)    { ExFreePoolWithTag(g_pawnScratch, POOL_TAG); g_pawnScratch = nullptr; }
    if (g_localEnts)      { ExFreePoolWithTag(g_localEnts, POOL_TAG); g_localEnts = nullptr; }
    if (g_prevEnts)       { ExFreePoolWithTag(g_prevEnts, POOL_TAG); g_prevEnts = nullptr; }
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

    // ВАЖНО: восстанавливаем стандартную частоту системного таймера.
    // Если этого не сделать, после выгрузки драйвера таймер останется на 0.5 мс,
    // что повышает энергопотребление всей системы (ноутбуки/батарея).
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
    RtlZeroMemory(g_slotCache, sizeof(g_slotCache));
    ResetTlbFull();

    g_chunkCache = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                           CACHED_CHUNKS * 512 * ENTITY_STRIDE, POOL_TAG);
    if (!g_chunkCache) return STATUS_INSUFFICIENT_RESOURCES;

    g_bonesScratch = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, MAX_BONES * 0x20, POOL_TAG);
    if (!g_bonesScratch) { DestroyShared(); return STATUS_INSUFFICIENT_RESOURCES; }

    g_pawnScratch = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAWN_BATCH_SIZE, POOL_TAG);
    if (!g_pawnScratch) { DestroyShared(); return STATUS_INSUFFICIENT_RESOURCES; }

    g_localEnts = (SHARED_ENTITY*)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                  sizeof(SHARED_ENTITY) * MAX_ENTITIES, POOL_TAG);
    if (!g_localEnts) { DestroyShared(); return STATUS_INSUFFICIENT_RESOURCES; }

    g_prevEnts = (SHARED_ENTITY*)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                 sizeof(SHARED_ENTITY) * MAX_ENTITIES, POOL_TAG);
    if (!g_prevEnts) { DestroyShared(); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(g_prevEnts, sizeof(SHARED_ENTITY) * MAX_ENTITIES);

    NTSTATUS st = CreateShared();
    if (!NT_SUCCESS(st)) {
        DestroyShared();
        return st;
    }

    ULONG curRes = ExSetTimerResolution(10000, TRUE);  // 1 мс (не 0.5)

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

    Log("DriverEntry ok (MmCopyMemory PHYSICAL + Batch Read, res=%u): shared=%p view=%p",
        curRes, g_sharedVa, g_viewVa);

    if (drv) drv->DriverUnload = DriverUnloadFn;
    return STATUS_SUCCESS;
}
