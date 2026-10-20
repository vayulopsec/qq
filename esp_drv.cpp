// esp_drv.cpp — CS2 ESP kernel driver (manual map, no SEH, no STL/RTTI/exceptions)
// build: MSVC + WDK, /kernel /GS- /GR- /EHs-c- /EHc- /Zl
//
// Guarantees:
//   * No C++ exceptions       — /EHs-c- /EHc-, no try/throw/catch.
//   * No RTTI                 — /GR-, no dynamic_cast, no typeid.
//   * No SEH                  — no __try/__except/__finally.
//   * No STL / C++ runtime    — only <ntifs.h>, <ntddk.h>, <intrin.h>.
//   * No implicit new/delete  — all memory via ExAllocatePool2 / ExFreePoolWithTag.

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

// ---- ZwQuerySection: не объявлен в WDK headers — добавляем вручную ----
typedef enum _SECTION_INFORMATION_CLASS {
    SectionBasicInformation = 0,
    SectionImageInformation = 1
} SECTION_INFORMATION_CLASS;

typedef struct _SECTION_BASIC_INFORMATION {
    PVOID       BaseAddress;
    ULONG       Attributes;
    LARGE_INTEGER Size;
} SECTION_BASIC_INFORMATION, *PSECTION_BASIC_INFORMATION;

extern "C" NTSTATUS ZwQuerySection(
    HANDLE SectionHandle,
    SECTION_INFORMATION_CLASS SectionInformationClass,
    PVOID SectionInformation,
    SIZE_T SectionInformationLength,
    PSIZE_T ReturnLength
);

#define POOL_TAG      'spSE'
#define MAX_ENTITIES  256
#define MAX_BONES     24
#define MAX_LOGS      256
#define LOG_LINE_MAX  128

// ---- CS2 offsets ----
#define OFF_dwEntityList        0x271A908ULL
#define OFF_dwLocalPlayerPawn   0x2565718ULL
#define OFF_dwViewMatrix        0x256B020ULL
#define OFF_m_iTeamNum          0x3E7ULL
#define OFF_m_lifeState         0x354ULL
#define OFF_m_iHealth           0x34CULL
#define OFF_m_pGameSceneNode    0x330ULL
#define OFF_m_modelState        0x140ULL
#define OFF_m_hPlayerPawn       0x92CULL

#define PHYS_MASK_4K  0x000FFFFFFFFFF000ULL
#define PHYS_MASK_2M  0x000FFFFFFFE00000ULL
#define PHYS_MASK_1G  0x000FFFFFC0000000ULL

#pragma pack(push, 1)
typedef struct _SHARED_BONE { float x, y, z; } SHARED_BONE;
typedef struct _SHARED_ENTITY {
    int   valid;
    int   hp;
    int   team;
    int   pad0;
    SHARED_BONE bones[MAX_BONES];
} SHARED_ENTITY;
typedef struct _SHARED_HEADER {
    volatile LONG seq;
    ULONG maxEntities;
    ULONG entityCount;
    ULONG version;
} SHARED_HEADER;
typedef struct _SHARED_LOG_ENTRY {
    ULONG tick;
    char  text[LOG_LINE_MAX];
} SHARED_LOG_ENTRY;
typedef struct _SHARED_LOG {
    volatile LONG head;
    volatile LONG tail;
    SHARED_LOG_ENTRY entries[MAX_LOGS];
} SHARED_LOG;
typedef struct _SHARED_BLOCK {
    SHARED_HEADER hdr;
    SHARED_LOG    log;
    SHARED_ENTITY ents[MAX_ENTITIES];
} SHARED_BLOCK;
#pragma pack(pop)

static HANDLE          g_hSharedSection = nullptr;
static SHARED_BLOCK*   g_sharedVa = nullptr;
static HANDLE          g_hViewSection = nullptr;
static float*          g_viewVa = nullptr;

static ULONG64         g_cs2Cr3 = 0;
static ULONG64         g_clientBase = 0;
static ULONG64         g_localPawn = 0;
static ULONG64         g_entitySystem = 0;
static volatile LONG   g_stop = 0;
static KEVENT          g_workerEvent;
static PKTHREAD        g_workerThread = nullptr;

// ---- logging: DbgPrintEx + ring in shared memory ----
static volatile LONG   g_logTick = 0;
static volatile LONG   g_statReads = 0;
static volatile LONG   g_statReadFails = 0;

// ---------------------------------------------------------------------------
// Мини-форматтер для kernel-mode: без UCRT, без ntstrsafe-vprintf.
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
    if (v < 0) { p = KAppendChar(p, end, '-'); return KAppendUInt(p, end, (unsigned long long)(-v), 10, false); }
    return KAppendUInt(p, end, (unsigned long long)v, 10, false);
}

static char* KAppendFloat(char* p, char* end, double v, int prec) {
    if (v != v) return KAppendStr(p, end, "nan");
    if (v < 0) { p = KAppendChar(p, end, '-'); v = -v; }
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    p = KAppendUInt(p, end, ip, 10, false);
    if (prec > 0) {
        p = KAppendChar(p, end, '.');
        for (int i = 0; i < prec; ++i) {
            frac *= 10.0;
            int d = (int)frac;
            p = KAppendChar(p, end, (char)('0' + d));
            frac -= d;
        }
    }
    return p;
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
            unsigned long long v = isLongLong ? va_arg(ap, unsigned long long)
                                              : (unsigned long long)va_arg(ap, unsigned int);
            p = KAppendUInt(p, end, v, 16, true); continue; }
        if (*fmt == 'x') { ++fmt;
            unsigned long long v = isLongLong ? va_arg(ap, unsigned long long)
                                              : (unsigned long long)va_arg(ap, unsigned int);
            p = KAppendUInt(p, end, v, 16, false); continue; }
        if (*fmt == 'u') { ++fmt;
            unsigned long long v = isLongLong ? va_arg(ap, unsigned long long)
                                              : (unsigned long long)va_arg(ap, unsigned int);
            p = KAppendUInt(p, end, v, 10, false); continue; }
        if (*fmt == 'd' || *fmt == 'i') { ++fmt;
            long long v = isLongLong ? va_arg(ap, long long)
                                     : (long long)va_arg(ap, int);
            p = KAppendInt(p, end, v); continue; }
        if (*fmt == 'p') { ++fmt;
            p = KAppendStr(p, end, "0x");
            p = KAppendUInt(p, end, (unsigned long long)(ULONG_PTR)va_arg(ap, void*), 16, false);
            continue; }
        if (*fmt == 's') { ++fmt; p = KAppendStr(p, end, va_arg(ap, const char*)); continue; }
        if (*fmt == 'c') { ++fmt; p = KAppendChar(p, end, (char)va_arg(ap, int)); continue; }
        if (*fmt == 'f') { ++fmt; p = KAppendFloat(p, end, va_arg(ap, double), 3); continue; }
        p = KAppendChar(p, end, '%');
        if (*fmt) p = KAppendChar(p, end, *fmt++);
    }
    *p = 0;
}

static void LogRaw(const char* text) {
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[esp_drv] %s\n", text);
    if (!g_sharedVa) return;
    LONG tail = InterlockedIncrement(&g_sharedVa->log.tail) - 1;
    LONG slot = tail % MAX_LOGS;
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

// ---- CR3 page walker ----
static BOOLEAN ReadPhysical(ULONG64 phys, PVOID buf, SIZE_T size) {
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = phys;
    PVOID mapped = MmMapIoSpace(pa, size, MmNonCached);
    if (!mapped) return FALSE;
    RtlCopyMemory(buf, mapped, size);
    MmUnmapIoSpace(mapped, size);
    return TRUE;
}

static ULONG64 ReadPhysicalQword(ULONG64 phys) {
    ULONG64 v = 0;
    ReadPhysical(phys, &v, sizeof(v));
    return v;
}

static ULONG64 VirtualToPhysical(ULONG64 cr3, ULONG64 va) {
    ULONG64 pml4e = ReadPhysicalQword((cr3 & PHYS_MASK_4K) + 8 * ((va >> 39) & 0x1FF));
    if (!(pml4e & 1)) return 0;
    ULONG64 pdpte = ReadPhysicalQword((pml4e & PHYS_MASK_4K) + 8 * ((va >> 30) & 0x1FF));
    if (!(pdpte & 1)) return 0;
    if (pdpte & 0x80) return (pdpte & PHYS_MASK_1G) + (va & 0x3FFFFFFFULL);
    ULONG64 pde = ReadPhysicalQword((pdpte & PHYS_MASK_4K) + 8 * ((va >> 21) & 0x1FF));
    if (!(pde & 1)) return 0;
    if (pde & 0x80) return (pde & PHYS_MASK_2M) + (va & 0x1FFFFFULL);
    ULONG64 pte = ReadPhysicalQword((pde & PHYS_MASK_4K) + 8 * ((va >> 12) & 0x1FF));
    if (!(pte & 1)) return 0;
    return (pte & PHYS_MASK_4K) + (va & 0xFFFULL);
}

static BOOLEAN ReadCs2Memory(ULONG64 va, PVOID buf, SIZE_T size) {
    if (!g_cs2Cr3 || !va || !buf || !size) return FALSE;
    PUCHAR out = (PUCHAR)buf;
    SIZE_T done = 0;
    while (done < size) {
        ULONG64 cur = va + done;
        ULONG64 phys = VirtualToPhysical(g_cs2Cr3, cur);
        if (!phys) { InterlockedIncrement(&g_statReadFails); return FALSE; }
        SIZE_T chunk = 0x1000 - (cur & 0xFFF);
        if (chunk > size - done) chunk = size - done;
        if (!ReadPhysical(phys, out + done, chunk)) { InterlockedIncrement(&g_statReadFails); return FALSE; }
        InterlockedIncrement(&g_statReads);
        done += chunk;
    }
    return TRUE;
}

static BOOLEAN RdQ(ULONG64 va, PULONG64 out) { return ReadCs2Memory(va, out, 8); }
static BOOLEAN RdD(ULONG64 va, PULONG   out) { return ReadCs2Memory(va, out, 4); }

// ---- process / module discovery ----
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

static PEPROCESS FindCs2Process() {
    ULONG size = 0x10000; PVOID buf = nullptr; NTSTATUS st = STATUS_INFO_LENGTH_MISMATCH;
    for (int i = 0; i < 4; ++i) {
        buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, POOL_TAG);
        if (!buf) return nullptr;
        st = ZwQuerySystemInformation(5, buf, size, &size);
        if (NT_SUCCESS(st)) break;
        ExFreePoolWithTag(buf, POOL_TAG); buf = nullptr;
        if (st != STATUS_INFO_LENGTH_MISMATCH) return nullptr;
        size *= 2;
    }
    if (!buf) return nullptr;
    ANSI_STRING target; RtlInitAnsiString(&target, "cs2.exe");
    PSYSTEM_PROCESS_INFO spi = (PSYSTEM_PROCESS_INFO)buf;
    PEPROCESS found = nullptr;
    for (;;) {
        if (spi->ImageName.Buffer && spi->ImageName.Length) {
            ANSI_STRING a;
            if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&a, &spi->ImageName, TRUE))) {
                if (RtlEqualString(&a, &target, TRUE)) {
                    if (NT_SUCCESS(PsLookupProcessByProcessId(spi->UniqueProcessId, &found))) {
                        RtlFreeAnsiString(&a); break;
                    }
                }
                RtlFreeAnsiString(&a);
            }
        }
        if (!spi->NextEntryOffset) break;
        spi = (PSYSTEM_PROCESS_INFO)((PUCHAR)spi + spi->NextEntryOffset);
    }
    ExFreePoolWithTag(buf, POOL_TAG);
    return found;
}

static ULONG64 GetProcessCr3(PEPROCESS p) { return *(ULONG64*)((PUCHAR)p + 0x28); }

typedef struct _LDR_ENTRY_OFF {
    LIST_ENTRY InLoadOrderLinks; PVOID ExceptionTable; ULONG ExceptionTableSize; PVOID GpValue;
    PVOID NonPagedDebugInfo; PVOID DllBase; PVOID EntryPoint; ULONG SizeOfImage;
    UNICODE_STRING FullDllName; UNICODE_STRING BaseDllName;
} LDR_ENTRY_OFF;

static ULONG64 FindModuleBase(PEPROCESS proc, PCWSTR moduleName) {
    ULONG64 peb = 0; UNICODE_STRING fnName; RtlInitUnicodeString(&fnName, L"PsGetProcessPeb");
    typedef PVOID(*fnPeb)(PEPROCESS); fnPeb fn = (fnPeb)MmGetSystemRoutineAddress(&fnName);
    if (fn) peb = (ULONG64)fn(proc); if (!peb) return 0;
    ULONG64 ldr = 0; if (!RdQ(peb + 0x18, &ldr) || !ldr) return 0;
    ULONG64 head = ldr + 0x10; ULONG64 cur = 0; if (!RdQ(head, &cur)) return 0;
    for (int guard = 0; guard < 512 && cur && cur != head; ++guard) {
        LDR_ENTRY_OFF e = {}; if (!ReadCs2Memory(cur, &e, sizeof(e))) return 0;
        if (e.BaseDllName.Buffer && e.BaseDllName.Length) {
            WCHAR n[128] = {}; USHORT len = e.BaseDllName.Length;
            if (len > sizeof(n) - sizeof(WCHAR)) len = sizeof(n) - sizeof(WCHAR);
            if (ReadCs2Memory((ULONG64)e.BaseDllName.Buffer, n, len)) {
                n[len / sizeof(WCHAR)] = 0;
                if (_wcsicmp(n, moduleName) == 0) return (ULONG64)e.DllBase;
            }
        }
        ULONG64 next = 0; if (!RdQ(cur, &next)) return 0; cur = next;
    }
    return 0;
}

// ---- GetEntityByIndex ----
static ULONG64 EntityByIndex(ULONG64 es, ULONG idx) {
    if (!es || idx > 0x7FFE) return 0;
    ULONG c = idx >> 9; if (c > 0x3F) return 0;
    ULONG64 chunk = 0; if (!RdQ(es + 0x10 + 8ULL * c, &chunk) || !chunk) return 0;
    ULONG64 entry = chunk + 112ULL * (idx & 0x1FF);
    ULONG ident = 0; if (!RdD(entry + 0x10, &ident)) return 0;
    if ((ident & 0x7FFF) != idx) return 0;
    ULONG64 ent = 0; if (!RdQ(entry, &ent)) return 0; return ent;
}

static void ClearShared() {
    if (!g_sharedVa) return;
    InterlockedIncrement(&g_sharedVa->hdr.seq);
    g_sharedVa->hdr.entityCount = 0;
    InterlockedIncrement(&g_sharedVa->hdr.seq);
}

static void Tick() {
    static ULONG s_lastLog = 0;
    ULONG now = (ULONG)InterlockedIncrement(&g_logTick) / 100;
    bool logNow = (now != s_lastLog); if (logNow) s_lastLog = now;

    USHORT mz = 0;
    bool baseAlive = g_cs2Cr3 && g_clientBase && ReadCs2Memory(g_clientBase, &mz, sizeof(mz)) && mz == 0x5A4D;

    if (!baseAlive) {
        g_cs2Cr3 = 0; g_clientBase = 0;
        PEPROCESS p = FindCs2Process();
        if (!p) { ClearShared(); if (logNow) Log("cs2.exe not found"); return; }
        g_cs2Cr3 = GetProcessCr3(p);
        g_clientBase = FindModuleBase(p, L"client.dll");
        ObDereferenceObject(p);
        if (logNow) Log("re-discovered: cr3=%llx clientBase=%llx", g_cs2Cr3, g_clientBase);
        if (!g_cs2Cr3 || !g_clientBase) { ClearShared(); return; }
    }

    ULONG64 lp = 0, es = 0;
    RdQ(g_clientBase + OFF_dwLocalPlayerPawn, &lp);
    RdQ(g_clientBase + OFF_dwEntityList, &es);
    g_localPawn = lp; g_entitySystem = es;

    float vm[16] = {};
    ReadCs2Memory(g_clientBase + OFF_dwViewMatrix, vm, sizeof(vm));
    if (g_viewVa) RtlCopyMemory(g_viewVa, vm, sizeof(vm));

    if (logNow) Log("base=%llx cr3=%llx lp=%llx es=%llx vm[0]=%.3f vm[12]=%.3f", g_clientBase, g_cs2Cr3, lp, es, vm[0], vm[12]);

    if (!lp || !es) { ClearShared(); return; }
    ULONG myTeam = 0; RdD(lp + OFF_m_iTeamNum, &myTeam);

    if (g_sharedVa) InterlockedIncrement(&g_sharedVa->hdr.seq);
    ULONG out = 0; ULONG scanned = 0, ctrlOk = 0, pawnOk = 0, aliveOk = 0, boneOk = 0;

    for (ULONG i = 1; i <= 64 && out < MAX_ENTITIES; ++i) {
        ++scanned;
        ULONG64 ctrl = EntityByIndex(es, i);
        if (!ctrl) continue; ++ctrlOk;
        ULONG hPawn = 0; if (!RdD(ctrl + OFF_m_hPlayerPawn, &hPawn)) continue;
        if (!hPawn || hPawn == 0xFFFFFFFF) continue;
        ULONG64 pawn = EntityByIndex(es, hPawn & 0x7FFF);
        if (!pawn || pawn == lp) continue; ++pawnOk;

        ULONG hp = 0, ls = 0, team = 0;
        RdD(pawn + OFF_m_iHealth, &hp); RdD(pawn + OFF_m_lifeState, &ls); RdD(pawn + OFF_m_iTeamNum, &team);
        if ((int)hp <= 0 || (int)hp > 200 || ls != 256 || team == myTeam) continue; ++aliveOk;

        ULONG64 scene = 0, bMat = 0;
        if (!RdQ(pawn + OFF_m_pGameSceneNode, &scene) || !scene) continue;
        if (!RdQ(scene + OFF_m_modelState + 0x80, &bMat) || !bMat) continue;

        UCHAR raw[MAX_BONES * 0x20];
        if (!ReadCs2Memory(bMat, raw, sizeof(raw))) continue; ++boneOk;

        SHARED_ENTITY e = {}; e.valid = 1; e.hp = (int)hp; e.team = (int)team;
        for (int b = 0; b < MAX_BONES; ++b) RtlCopyMemory(&e.bones[b], raw + (size_t)b * 0x20, sizeof(SHARED_BONE));

        if (g_sharedVa) g_sharedVa->ents[out] = e; ++out;
    }
    if (g_sharedVa) {
        g_sharedVa->hdr.entityCount = out; g_sharedVa->hdr.version = 1;
        InterlockedIncrement(&g_sharedVa->hdr.seq);
    }
    if (logNow) Log("scan: scanned=%u ctrlOk=%u pawnOk=%u aliveOk=%u boneOk=%u out=%u reads=%ld fails=%ld", scanned, ctrlOk, pawnOk, aliveOk, boneOk, out, (long)g_statReads, (long)g_statReadFails);
}

static VOID WorkerThread(PVOID) {
    LARGE_INTEGER interval;
    interval.QuadPart = -10 * 1000 * 10;
    for (;;) {
        if (InterlockedCompareExchange(&g_stop, 0, 0)) break;
        Tick();
        KeWaitForSingleObject(&g_workerEvent, Executive, KernelMode, FALSE, &interval);
    }
}

// ---------------------------------------------------------------------------
// Безопасное создание Security Descriptor для Windows 11 24H2+
// Используем стандартный API, чтобы избежать BSOD в SeAssignSecurity.
// ---------------------------------------------------------------------------
static NTSTATUS BuildSafeSd(PSECURITY_DESCRIPTOR sd) {
    NTSTATUS st = RtlCreateSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(st)) return st;
    // Устанавливаем Null DACL (доступ всем), но через официальную функцию
    return RtlSetDaclSecurityDescriptor(sd, TRUE, NULL, FALSE);
}

static NTSTATUS OpenOrCreateSection(PCWSTR nameStr, HANDLE* outHandle, LARGE_INTEGER size) {
    NTSTATUS st;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;

    SECURITY_DESCRIPTOR sd;
    st = BuildSafeSd(&sd);
    if (!NT_SUCCESS(st)) return st;

    RtlInitUnicodeString(&name, nameStr);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, &sd);

    st = ZwCreateSection(outHandle, SECTION_ALL_ACCESS, &oa, &size, PAGE_READWRITE, SEC_COMMIT, nullptr);

    if (st == STATUS_OBJECT_NAME_COLLISION) {
        Log("section exists, opening instead");
        InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
        st = ZwOpenSection(outHandle, SECTION_ALL_ACCESS, &oa);
    }
    return st;
}

static NTSTATUS CreateShared() {
    NTSTATUS st;
    LARGE_INTEGER sz;
    PVOID base = nullptr;
    SIZE_T vs = 0;
    SECTION_BASIC_INFORMATION sbi = {};

    if (!g_sharedVa) {
        sz.QuadPart = sizeof(SHARED_BLOCK);
        st = OpenOrCreateSection(L"\\BaseNamedObjects\\Cs2EspShared_v1", &g_hSharedSection, sz);
        if (!NT_SUCCESS(st)) { Log("CreateSection shared failed %x", (unsigned)st); return st; }

        RtlZeroMemory(&sbi, sizeof(sbi));
        st = ZwQuerySection(g_hSharedSection, SectionBasicInformation, &sbi, sizeof(sbi), nullptr);
        if (NT_SUCCESS(st)) {
            if (sbi.Size.QuadPart < (ULONGLONG)sizeof(SHARED_BLOCK)) {
                Log("shared: too small, stale section. Reboot required.");
                ZwClose(g_hSharedSection); g_hSharedSection = nullptr;
                return STATUS_INVALID_PARAMETER;
            }
        }

        base = nullptr; vs = 0;
        st = ZwMapViewOfSection(g_hSharedSection, ZwCurrentProcess(), &base, 0, 0, nullptr, &vs, ViewUnmap, 0, PAGE_READWRITE);
        if (!NT_SUCCESS(st)) { Log("MapView shared failed %x", (unsigned)st); return st; }

        g_sharedVa = (SHARED_BLOCK*)base;
        g_sharedVa->hdr.version = 1;
        g_sharedVa->hdr.maxEntities = MAX_ENTITIES;
    }

    if (!g_viewVa) {
        sz.QuadPart = 0x1000;
        st = OpenOrCreateSection(L"\\BaseNamedObjects\\Cs2EspView_v1", &g_hViewSection, sz);
        if (!NT_SUCCESS(st)) { Log("CreateSection view failed %x", (unsigned)st); return st; }

        RtlZeroMemory(&sbi, sizeof(sbi));
        st = ZwQuerySection(g_hViewSection, SectionBasicInformation, &sbi, sizeof(sbi), nullptr);
        if (NT_SUCCESS(st)) {
            if (sbi.Size.QuadPart < 64ULL) {
                Log("view: too small, stale section. Reboot required.");
                ZwClose(g_hViewSection); g_hViewSection = nullptr;
                return STATUS_INVALID_PARAMETER;
            }
        }

        base = nullptr; vs = 0;
        st = ZwMapViewOfSection(g_hViewSection, ZwCurrentProcess(), &base, 0, 0, nullptr, &vs, ViewUnmap, 0, PAGE_READWRITE);
        if (!NT_SUCCESS(st)) { Log("MapView view failed %x", (unsigned)st); return st; }

        g_viewVa = (float*)base;
    }

    return STATUS_SUCCESS;
}

static void DestroyShared() {
    if (g_viewVa && g_hViewSection)   { ZwUnmapViewOfSection(ZwCurrentProcess(), g_viewVa); g_viewVa = nullptr; }
    if (g_hViewSection)               { ZwClose(g_hViewSection); g_hViewSection = nullptr; }
    if (g_sharedVa && g_hSharedSection){ ZwUnmapViewOfSection(ZwCurrentProcess(), g_sharedVa); g_sharedVa = nullptr; }
    if (g_hSharedSection)             { ZwClose(g_hSharedSection); g_hSharedSection = nullptr; }
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING) {
    NTSTATUS st = CreateShared();
    if (!NT_SUCCESS(st)) return st;

    KeInitializeEvent(&g_workerEvent, NotificationEvent, FALSE);

    HANDLE h = nullptr;
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, nullptr, nullptr, nullptr, WorkerThread, nullptr);
    if (!NT_SUCCESS(st)) { DestroyShared(); return st; }
    ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID*)&g_workerThread, nullptr);
    ZwClose(h);

    Log("DriverEntry ok: shared=%p view=%p", g_sharedVa, g_viewVa);

    drv->DriverUnload = [](PDRIVER_OBJECT) {
        InterlockedExchange(&g_stop, 1);
        KeSetEvent(&g_workerEvent, 0, FALSE);
        if (g_workerThread) {
            KeWaitForSingleObject(g_workerThread, Executive, KernelMode, FALSE, nullptr);
            ObDereferenceObject(g_workerThread);
            g_workerThread = nullptr;
        }
        DestroyShared();
    };
    return STATUS_SUCCESS;
}
