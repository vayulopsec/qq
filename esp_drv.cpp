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

// Пользовательский VA не может быть >= 0x800000000000 (canonical верх)
constexpr ULONG64 USER_VA_MAX           = 0x800000000000ULL;

#pragma pack(push, 1)
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
static PEPROCESS     g_cs2Proc        = nullptr;  // держим reference для KeStackAttachProcess
static ULONG64       g_localPawn      = 0;
static ULONG64       g_entitySystem   = 0;
static volatile LONG g_stop           = 0;
static KEVENT        g_workerEvent;
static PKTHREAD      g_workerThread   = nullptr;

static volatile LONG g_logTick        = 0;
static volatile LONG g_statReads      = 0;
static volatile LONG g_statReadFails  = 0;
static volatile LONG g_statAttachReads = 0;

// ---- Состояние для отслеживания смены процесса/PEB ----
static ULONG64       g_lastPeb          = 0;
static bool          g_fmbVerboseLogged = false;

// ---- Локальный кэш трансляции адресов (Direct-mapped TLB 256 + PML4) ----
constexpr ULONG TLB_SIZE     = 256;
constexpr ULONG PML4_MAX_AGE = 64;   // ~128 мс при 500 Гц

struct TlbEntry {
    ULONG64 vaPage;
    ULONG64 physPage;
};

struct TranslationCache {
    TlbEntry entries[TLB_SIZE];
    ULONG64  pml4[512];
    BOOLEAN  pml4Valid;
    ULONG    pml4Age;
};

static TranslationCache g_tlb;

static __forceinline void ResetTlbPages() {
    for (ULONG i = 0; i < TLB_SIZE; ++i) {
        g_tlb.entries[i].vaPage   = ~0ULL;
        g_tlb.entries[i].physPage = 0;
    }
}

static __forceinline void ResetTlbFull() {
    ResetTlbPages();
    g_tlb.pml4Valid = FALSE;
    g_tlb.pml4Age   = 0;
}

// ---------------------------------------------------------------------------
// Мини-форматтер для kernel-mode
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
#ifdef DBG
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[esp_drv] %s\n", text);
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
            g_tlb.pml4Age   = 0;
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

// Ручной walk через CR3
static BOOLEAN ReadViaPhysicalWalk(ULONG64 va, PVOID buf, SIZE_T size) {
    if (!g_cs2Cr3 || !va || !buf || !size) return FALSE;
    if (va >= USER_VA_MAX) return FALSE;
    if (size > USER_VA_MAX || va + size < va || va + size > USER_VA_MAX) return FALSE;

    PUCHAR out = (PUCHAR)buf;
    SIZE_T done = 0;

    while (done < size) {
        ULONG64 curVa = va + done;
        ULONG64 startPhys = VirtualToPhysical(g_cs2Cr3, curVa);
        if (!startPhys) {
            ResetTlbPages();        // только страницы — PML4 не трогаем
            return FALSE;
        }

        SIZE_T chunk = 0x1000 - (curVa & 0xFFF);
        if (chunk > size - done) chunk = size - done;

        // Склеиваем смежные страницы, если они физически непрерывны
        while (done + chunk < size) {
            ULONG64 nextVa = curVa + chunk;
            ULONG64 nextPhys = VirtualToPhysical(g_cs2Cr3, nextVa);
            if (nextPhys != startPhys + chunk) break;

            SIZE_T add = 0x1000;
            if (chunk + add > size - done) add = (size - done) - chunk;
            chunk += add;
        }

        if (!ReadPhysicalCached(startPhys, out + done, chunk)) {
            ResetTlbPages();
            return FALSE;
        }
        done += chunk;
    }
    return TRUE;
}

// Fallback через KeStackAttachProcess + MmCopyMemory(VIRTUAL).
// Работает при VBS/HVCI и когда ручной walk CR3 сломан.
static BOOLEAN ReadViaAttach(ULONG64 va, PVOID buf, SIZE_T size) {
    if (!g_cs2Proc || !va || !buf || !size) return FALSE;
    if (va >= USER_VA_MAX) return FALSE;

    KAPC_STATE apc;
    KeStackAttachProcess(g_cs2Proc, &apc);

    BOOLEAN ok = FALSE;
    SIZE_T copied = 0;
    MM_COPY_ADDRESS src;
    src.VirtualAddress = (PVOID)va;
    NTSTATUS st = MmCopyMemory(buf, src, size, MM_COPY_MEMORY_VIRTUAL, &copied);
    ok = NT_SUCCESS(st) && copied == size;

    KeUnstackDetachProcess(&apc);
    return ok;
}

// Основная функция чтения: сначала ручной walk, при провале — attach+VIRTUAL.
static BOOLEAN ReadCs2Memory(ULONG64 va, PVOID buf, SIZE_T size) {
    if (!va || !buf || !size) return FALSE;
    if (va >= USER_VA_MAX) return FALSE;
    if (size > USER_VA_MAX || va + size < va || va + size > USER_VA_MAX) return FALSE;

    if (g_cs2Cr3 && ReadViaPhysicalWalk(va, buf, size)) {
        InterlockedIncrement(&g_statReads);
        return TRUE;
    }

    if (ReadViaAttach(va, buf, size)) {
        InterlockedIncrement(&g_statAttachReads);
        return TRUE;
    }

    InterlockedIncrement(&g_statReadFails);
    return FALSE;
}

static __forceinline BOOLEAN RdQ(ULONG64 va, PULONG64 out) { return ReadCs2Memory(va, out, sizeof(ULONG64)); }
static __forceinline BOOLEAN RdD(ULONG64 va, PULONG   out) { return ReadCs2Memory(va, out, sizeof(ULONG)); }

// ---- Поиск процесса ----
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
    return cr3 & PHYS_MASK_4K;
}

static __forceinline ULONG64 GetProcessPeb(PEPROCESS p) {
    UNICODE_STRING fnName;
    RtlInitUnicodeString(&fnName, L"PsGetProcessPeb");
    typedef PVOID(*fnPeb)(PEPROCESS);
    fnPeb fn = (fnPeb)MmGetSystemRoutineAddress(&fnName);
    if (!fn) return 0;
    return (ULONG64)fn(p);
}

// ---- Правильная структура _LDR_DATA_TABLE_ENTRY ----
typedef struct _LDR_ENTRY_OFF {
    LIST_ENTRY     InLoadOrderLinks;              // +0x000
    LIST_ENTRY     InMemoryOrderLinks;            // +0x010
    LIST_ENTRY     InInitializationOrderLinks;    // +0x020
    PVOID          DllBase;                       // +0x030
    PVOID          EntryPoint;                    // +0x038
    ULONG          SizeOfImage;                   // +0x040
    UNICODE_STRING FullDllName;                   // +0x048
    UNICODE_STRING BaseDllName;                   // +0x058
} LDR_ENTRY_OFF;

struct LdrListDesc {
    ULONG headOff;       // offset в _PEB_LDR_DATA
    ULONG linkOff;       // offset links в _LDR_DATA_TABLE_ENTRY
    const char* name;
};

// Все три списка с корректными смещениями
static const LdrListDesc LDR_LISTS[3] = {
    { 0x10, 0x00, "InLoadOrder" },
    { 0x20, 0x10, "InMemoryOrder" },
    { 0x30, 0x20, "InInitOrder" },
};

static ULONG64 TryLdrList(ULONG64 ldr, const LdrListDesc& list, PCWSTR moduleName,
                          bool verbose) {
    UNICODE_STRING want;
    RtlInitUnicodeString(&want, moduleName);

    ULONG64 head = ldr + list.headOff;
    ULONG64 cur = 0;
    if (!RdQ(head, &cur) || !cur) return 0;

    // Валидация: user VA, выровнено на 8
    if (cur >= USER_VA_MAX || (cur & 7)) return 0;

    int guard = 0;
    // Реально в CS2 модулей ~10-15. Ставим 16 — античит не сможет зациклить нас надолго.
    for (; guard < 16 && cur && cur != head; ++guard) {
        // cur указывает на &entry->{...}Links
        ULONG64 entryVa = cur - list.linkOff;

        LDR_ENTRY_OFF e = {};
        if (!ReadCs2Memory(entryVa, &e, sizeof(e))) break;

        ULONG64 base = (ULONG64)e.DllBase;
        USHORT mz = 0;
        bool mzOk = base && base < USER_VA_MAX
                    && ReadCs2Memory(base, &mz, sizeof(mz)) && mz == 0x5A4D;

        if (mzOk && e.BaseDllName.Buffer && e.BaseDllName.Length) {
            WCHAR n[128] = {};
            USHORT len = e.BaseDllName.Length;
            if (len > sizeof(n) - sizeof(WCHAR)) len = sizeof(n) - sizeof(WCHAR);
            if (ReadCs2Memory((ULONG64)e.BaseDllName.Buffer, n, len)) {
                n[len / sizeof(WCHAR)] = 0;
                UNICODE_STRING un;
                RtlInitUnicodeString(&un, n);
                if (RtlEqualUnicodeString(&un, &want, TRUE)) {
                    if (verbose) Log("FMB[%s]: found at %llx (guard=%d)", list.name, base, guard);
                    return base;
                }
            }
        }

        // Следующий элемент — Flink по тому же offset внутри entry
        ULONG64 nextLink = 0;
        if (!RdQ(entryVa + list.linkOff, &nextLink)) break;
        cur = nextLink;
        if (!cur || cur >= USER_VA_MAX || (cur & 7)) break;
    }
    if (verbose) Log("FMB[%s]: not found guard=%d", list.name, guard);
    return 0;
}

static ULONG64 FindModuleViaLdr(PEPROCESS proc, PCWSTR moduleName, bool verbose) {
    ULONG64 peb = GetProcessPeb(proc);
    if (verbose) Log("FMB: peb=%llx", peb);
    if (!peb) return 0;

    // Sanity: PEB->BeingDebugged (+0x02) должен читаться
    UCHAR beingDebugged = 0;
    if (!ReadCs2Memory(peb + 0x02, &beingDebugged, 1)) {
        if (verbose) Log("FMB: PEB itself unreadable at %llx", peb);
        return 0;
    }

    ULONG64 ldr = 0;
    if (!RdQ(peb + 0x18, &ldr)) {
        if (verbose) Log("FMB: ldr READ FAIL peb=%llx", peb);
        return 0;
    }
    if (!ldr) {
        if (verbose) Log("FMB: ldr == NULL (not initialized) peb=%llx", peb);
        return 0;
    }
    if (verbose) Log("FMB: ldr=%llx", ldr);

    // Пробуем все три списка по очереди
    for (const auto& list : LDR_LISTS) {
        ULONG64 base = TryLdrList(ldr, list, moduleName, verbose);
        if (base) return base;
    }
    return 0;
}

// ---- VAD-обход (primary метод) ----
// Динамически находим offset VadRoot через RtlGetVersion + таблицу,
// т.к. в разных сборках он разный. Но т.к. таблицу обновлять сложно,
// используем универсальный способ: перебираем известные offset'ы и проверяем,
// что первый узел дерева выглядит как валидный _MMVAD.
//
// Известные значения VadRoot для _EPROCESS:
//   Win10 19041-19045: 0x628
//   Win11 22000-22621: 0x7D8
//   Win11 22631+:      0x7D8 (проверить!)
// Пользовательский дамп (не x64 Win10/11) даёт 0x558 — не используем как primary.
//
// Более надёжный способ: RtlGetVersion → build number → таблица.

typedef struct _RTL_BALANCED_NODE_LOCAL {
    struct _RTL_BALANCED_NODE_LOCAL* Children[2];
    union {
        struct _RTL_BALANCED_NODE_LOCAL* Left;
        struct _RTL_BALANCED_NODE_LOCAL* Right;
    };
    ULONG_PTR ParentValue;
} RTL_BALANCED_NODE_LOCAL;

typedef struct _RTL_AVL_TREE_LOCAL {
    RTL_BALANCED_NODE_LOCAL* Root;
} RTL_AVL_TREE_LOCAL;

static ULONG GetVadRootOffset() {
    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    // RtlGetVersion всегда возвращает реальную версию (в отличие от GetVersionEx)
    typedef NTSTATUS(NTAPI* fnRtlGetVersion)(PRTL_OSVERSIONINFOW);
    UNICODE_STRING fnName;
    RtlInitUnicodeString(&fnName, L"RtlGetVersion");
    fnRtlGetVersion fn = (fnRtlGetVersion)MmGetSystemRoutineAddress(&fnName);
    if (!fn) return 0x7D8; // default Win11
    if (!NT_SUCCESS(fn(&vi))) return 0x7D8;

    ULONG build = vi.dwBuildNumber;

    // Win10 19041..19045
    if (build >= 19041 && build <= 19045) return 0x628;
    // Win11 22000..22621
    if (build >= 22000 && build < 22631) return 0x7D8;
    // Win11 22631+
    if (build >= 22631) return 0x7D8;
    // fallback
    return 0x7D8;
}

// Обход AVL-дерева VAD и поиск модуля по имени файла.
// Возвращает базовый VA региона или 0.
static ULONG64 FindModuleViaVad(PEPROCESS proc, PCWSTR moduleName, bool verbose) {
    ULONG vadRootOff = GetVadRootOffset();
    if (verbose) Log("VAD: rootOff=0x%x", vadRootOff);

    RTL_AVL_TREE_LOCAL tree = {};
    // Читаем VadRoot через attach — надёжнее, т.к. EPROCESS в system space
    // (но EPROCESS всегда доступен напрямую по kernel VA, attach не нужен)
    RtlCopyMemory(&tree, (PUCHAR)proc + vadRootOff, sizeof(tree));
    if (!tree.Root) {
        if (verbose) Log("VAD: root is NULL");
        return 0;
    }

    // Итеративный обход с ограничением
    constexpr int MAX_VADS = 4096;
    RTL_BALANCED_NODE_LOCAL* stack[MAX_VADS];
    int sp = 0;
    stack[sp++] = tree.Root;

    // Оффсеты _MMVAD (Win10 19041+ / Win11):
    //   _MMVAD.Core (т.е. _MMVAD_SHORT) на +0x00
    //   _MMVAD.Subsection на +0x48 (Win10) / +0x50 (некоторые сборки)
    //   _SUBSECTION.ControlArea на +0x00
    //   _CONTROL_AREA.FilePointer на +0x20
    //   _FILE_OBJECT.FileName на +0x30
    //
    // Если Subsection = 0 — это не image VAD, пропускаем.
    // Проверяем три варианта offset Subsection: 0x48, 0x50, 0x40.
    constexpr ULONG SUBSECTION_OFF_CANDIDATES[] = { 0x48, 0x50, 0x40 };

    while (sp > 0) {
        RTL_BALANCED_NODE_LOCAL* node = stack[--sp];
        if (!node) continue;

        // Проверяем, что узел читается
        RTL_BALANCED_NODE_LOCAL local = {};
        RtlCopyMemory(&local, node, sizeof(local));

        // _MMVAD_SHORT начинается с VadNode (RTL_BALANCED_NODE).
        // StartingVpn на +0x18, EndingVpn на +0x20.
        // Но т.к. VadNode — это вложенная структура, начало _MMVAD_SHORT = node.
        ULONG64 mmvad = (ULONG64)node;
        ULONG64 startingVpn = 0, endingVpn = 0;
        RtlCopyMemory(&startingVpn, (PVOID)(mmvad + 0x18), sizeof(startingVpn));
        RtlCopyMemory(&endingVpn,   (PVOID)(mmvad + 0x20), sizeof(endingVpn));

        if (endingVpn < startingVpn || startingVpn == 0) {
            // не image VAD или битые данные — идём дальше
        } else {
            ULONG64 regionBase = startingVpn << 12;
            ULONG64 regionSize = (endingVpn - startingVpn + 1) << 12;

            // Проверяем MZ по базе региона
            USHORT mz = 0;
            if (regionBase < USER_VA_MAX &&
                regionSize >= 0x1000 &&
                ReadCs2Memory(regionBase, &mz, sizeof(mz)) && mz == 0x5A4D) {

                // Пробуем найти имя модуля через Subsection->ControlArea->FilePointer
                for (ULONG subOff : SUBSECTION_OFF_CANDIDATES) {
                    ULONG64 subsection = 0;
                    RtlCopyMemory(&subsection, (PVOID)(mmvad + subOff), sizeof(subsection));
                    if (!subsection || subsection >= USER_VA_MAX) continue;

                    // _SUBSECTION.ControlArea на +0x00
                    ULONG64 controlArea = 0;
                    if (!ReadCs2Memory(subsection, &controlArea, sizeof(controlArea))) continue;
                    if (!controlArea || controlArea >= USER_VA_MAX) continue;

                    // _CONTROL_AREA.FilePointer на +0x20
                    ULONG64 fileObj = 0;
                    if (!ReadCs2Memory(controlArea + 0x20, &fileObj, sizeof(fileObj))) continue;
                    if (!fileObj || fileObj >= USER_VA_MAX) continue;

                    // _FILE_OBJECT.FileName на +0x30 (UNICODE_STRING)
                    UNICODE_STRING fileName = {};
                    if (!ReadCs2Memory(fileObj + 0x30, &fileName, sizeof(fileName))) continue;
                    if (!fileName.Buffer || !fileName.Length || fileName.Length > 512) continue;

                    WCHAR fullPath[260] = {};
                    USHORT len = fileName.Length;
                    if (len > sizeof(fullPath) - sizeof(WCHAR)) len = sizeof(fullPath) - sizeof(WCHAR);
                    if (!ReadCs2Memory((ULONG64)fileName.Buffer, fullPath, len)) continue;

                    // Сравниваем по суффиксу (имя файла без пути)
                    // moduleName = "client.dll", fullPath = "\Device\HarddiskVolumeX\...\client.dll"
                    USHORT chars = len / sizeof(WCHAR);
                    UNICODE_STRING want;
                    RtlInitUnicodeString(&want, moduleName);
                    USHORT wantChars = want.Length / sizeof(WCHAR);

                    if (chars >= wantChars) {
                        bool match = true;
                        for (USHORT k = 0; k < wantChars; ++k) {
                            WCHAR a = fullPath[chars - wantChars + k];
                            WCHAR b = want.Buffer[k];
                            // case-insensitive
                            if (a >= L'A' && a <= L'Z') a = (WCHAR)(a + 32);
                            if (b >= L'A' && b <= L'Z') b = (WCHAR)(b + 32);
                            if (a != b) { match = false; break; }
                        }
                        if (match) {
                            if (verbose) Log("VAD: found %ws at %llx", moduleName, regionBase);
                            return regionBase;
                        }
                    }
                }
            }
        }

        // Обход детей
        if (local.Children[0] && sp < MAX_VADS) stack[sp++] = local.Children[0];
        if (local.Children[1] && sp < MAX_VADS) stack[sp++] = local.Children[1];
    }

    if (verbose) Log("VAD: not found");
    return 0;
}

// Fallback: PEB->ImageBaseAddress (это base EXE, не DLL).
// Используется только как последняя надежда — если ничего другого не сработало.
static ULONG64 FindModuleViaImageBase(PEPROCESS proc, PCWSTR moduleName, bool verbose) {
    ULONG64 peb = GetProcessPeb(proc);
    if (!peb) return 0;

    ULONG64 imageBase = 0;
    if (!RdQ(peb + 0x10, &imageBase) || !imageBase) return 0;

    // Если ищем не exe, этот путь не подходит — возвращаем 0
    // Но по крайней мере проверим MZ
    USHORT mz = 0;
    if (!ReadCs2Memory(imageBase, &mz, sizeof(mz)) || mz != 0x5A4D) return 0;

    // Ищем "client.dll" в строках PE-заголовка (экспорт/импорт) — слишком сложно,
    // поэтому просто возвращаем 0, если имя не совпадает с cs2.exe
    UNICODE_STRING want;
    RtlInitUnicodeString(&want, moduleName);
    UNICODE_STRING exeName;
    RtlInitUnicodeString(&exeName, L"cs2.exe");
    if (RtlEqualUnicodeString(&want, &exeName, TRUE)) {
        if (verbose) Log("ImageBase: found cs2.exe at %llx", imageBase);
        return imageBase;
    }
    return 0;
}

// Основная функция поиска модуля: VAD → Ldr → ImageBase
static ULONG64 FindModuleBase(PEPROCESS proc, PCWSTR moduleName, bool verbose) {
    // 1) VAD-обход (primary — античит не трогает)
    ULONG64 base = FindModuleViaVad(proc, moduleName, verbose);
    if (base) return base;

    // 2) Ldr-обход (все три списка)
    base = FindModuleViaLdr(proc, moduleName, verbose);
    if (base) return base;

    // 3) ImageBase (только для exe)
    base = FindModuleViaImageBase(proc, moduleName, verbose);
    if (base) return base;

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

constexpr ULONG CACHED_CHUNKS = 3;
static UCHAR*         g_chunkCache   = nullptr;
static UCHAR*         g_bonesScratch = nullptr;
static UCHAR*         g_pawnScratch  = nullptr;
static SHARED_ENTITY* g_localEnts    = nullptr;
static SHARED_ENTITY* g_prevEnts     = nullptr;

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
    if (g_tlb.pml4Valid) {
        if (++g_tlb.pml4Age >= PML4_MAX_AGE) {
            g_tlb.pml4Valid = FALSE;
            g_tlb.pml4Age   = 0;
        }
    }
    ResetTlbPages();

    static ULONG s_tickCount = 0;
    static ULONG s_lastModuleAttempt = 0;
    static bool  s_processLogged = false;
    static ULONG s_emptyGraceTicks = 0;
    static ULONG s_mzFailCount = 0;
    ULONG tick = ++s_tickCount;
    bool logNow = ((tick % 2000) == 0);

    // ---- Проверка живости процесса и смены PEB/CR3 ----
    if (g_cs2Pid) {
        PEPROCESS p = nullptr;
        if (!NT_SUCCESS(PsLookupProcessByProcessId(g_cs2Pid, &p))) {
            Log("cs2.exe exited, full reset (pid=%p)", g_cs2Pid);
            if (g_cs2Proc) { ObDereferenceObject(g_cs2Proc); g_cs2Proc = nullptr; }
            g_cs2Cr3 = 0; g_clientBase = 0; g_cs2Pid = nullptr;
            g_lastPeb = 0; g_fmbVerboseLogged = false;
            s_processLogged = false;
            ResetTlbFull();
            ClearShared();
            return;
        }

        if ((tick & 0xF) == 0) {
            ULONG64 curPeb = GetProcessPeb(p);
            if (curPeb && g_lastPeb && curPeb != g_lastPeb) {
                Log("PEB changed %llx -> %llx, reset", g_lastPeb, curPeb);
                g_clientBase = 0;
                g_fmbVerboseLogged = false;
                ResetTlbFull();
                g_cs2Cr3 = GetProcessCr3(p);
            }
            if (curPeb) g_lastPeb = curPeb;
        }
        ObDereferenceObject(p);
    }

    // 1) Поиск процесса
    if (!g_cs2Cr3) {
        HANDLE pid = nullptr;
        PEPROCESS p = FindCs2Process(&pid);
        if (!p) {
            ClearShared();
            if (logNow && (tick % 5000) == 0) Log("cs2.exe not found");
            return;
        }
        g_cs2Cr3 = GetProcessCr3(p);
        g_cs2Pid = pid;
        g_lastPeb = GetProcessPeb(p);

        // Сохраняем reference на EPROCESS для KeStackAttachProcess
        if (g_cs2Proc) ObDereferenceObject(g_cs2Proc);
        g_cs2Proc = p;   // НЕ освобождаем — используем для attach
        ResetTlbFull();

        if (!s_processLogged) {
            Log("process found: pid=%p cr3=%llx peb=%llx",
                g_cs2Pid, g_cs2Cr3, g_lastPeb);
            s_processLogged = true;
        }
        if (!g_cs2Cr3) {
            ClearShared();
            return;
        }
    }

    // 2) Поиск/перепроверка client.dll
    bool needModuleCheck = (!g_clientBase) || ((tick - s_lastModuleAttempt) >= 64);

    if (g_clientBase && (tick & 0x3F) == 0) {
        USHORT mz = 0;
        if (!ReadCs2Memory(g_clientBase, &mz, sizeof(mz)) || mz != 0x5A4D) {
            if (++s_mzFailCount >= 3) {
                s_mzFailCount = 0;
                Log("client.dll MZ check failed 3x, resetting clientBase");
                g_clientBase = 0;
                g_fmbVerboseLogged = false;
                // ВАЖНО: только страницы, НЕ PML4 — иначе теряем весь кэш
                ResetTlbPages();
                needModuleCheck = true;
            }
        } else {
            s_mzFailCount = 0;
        }
    }

    if (needModuleCheck && !g_clientBase) {
        s_lastModuleAttempt = tick;
        PEPROCESS p = nullptr;
        if (g_cs2Proc) {
            // Используем сохранённый EPROCESS
            bool verbose = !g_fmbVerboseLogged;
            g_clientBase = FindModuleBase(g_cs2Proc, L"client.dll", verbose);
        } else if (NT_SUCCESS(PsLookupProcessByProcessId(g_cs2Pid, &p))) {
            bool verbose = !g_fmbVerboseLogged;
            g_clientBase = FindModuleBase(p, L"client.dll", verbose);
            ObDereferenceObject(p);
        }
        if (g_clientBase) {
            g_fmbVerboseLogged = true;
            Log("client.dll found: base=%llx", g_clientBase);
        } else {
            g_fmbVerboseLogged = true;
            g_tlb.pml4Valid = FALSE;
            g_tlb.pml4Age   = 0;
            if ((tick % 5000) == 0) {
                Log("module rediscover: clientBase=0 (client.dll not loaded yet)");
            }
        }
    }

    if (!g_clientBase) {
        ClearShared();
        return;
    }

    // 3) Чтение данных игры (вне Seqlock)
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
            sc.lastUpdateTick = tick - (i & 7);
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
                const ULONG* headBone = (const ULONG*)(g_bonesScratch + (SIZE_T)6 * 0x20);
                if (headBone[0] != 0 || headBone[1] != 0 || headBone[2] != 0) {
                    bonesReadSuccess = true;
                }
            }
            if (!bonesReadSuccess) {
                sc.bMat = 0;
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

    // 4) Публикация под Seqlock
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

    if (logNow && (out > 0 || g_clientBase)) {
        Log("tick: ctrl=%u pawn=%u alive=%u bone=%u out=%u maxPawn=%u reads=%u attach=%u fails=%u",
            ctrlOk, pawnOk, aliveOk, boneOk, out, maxPawn,
            (ULONG)g_statReads, (ULONG)g_statAttachReads, (ULONG)g_statReadFails);
    }
}

static VOID WorkerThread(PVOID) {
    KeSetPriorityThread(KeGetCurrentThread(), 8);

    LARGE_INTEGER interval;
    interval.QuadPart = -20000LL;  // 2 мс

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
        g_sharedVa->hdr.maxEntities = MAX_ENTITIES;
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
    if (g_cs2Proc)        { ObDereferenceObject(g_cs2Proc); g_cs2Proc = nullptr; }
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
    g_cs2Proc = nullptr;
    g_localPawn = 0; g_entitySystem = 0;
    g_stop = 0; g_workerThread = nullptr;
    g_logTick = 0; g_statReads = 0; g_statReadFails = 0;
    g_statAttachReads = 0;
    g_lastPeb = 0;
    g_fmbVerboseLogged = false;
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
    if (!NT_SUCCESS(st)) {
        InterlockedExchange(&g_stop, 1);
        KeSetEvent(&g_workerEvent, 0, FALSE);
        ZwWaitForSingleObject(h, FALSE, nullptr);
        ZwClose(h);
        DestroyShared();
        return st;
    }
    ZwClose(h);

    Log("DriverEntry ok (VAD+Ldr, KeStackAttach fallback, res=%u): shared=%p view=%p",
        curRes, g_sharedVa, g_viewVa);

    if (drv) drv->DriverUnload = DriverUnloadFn;
    return STATUS_SUCCESS;
}
