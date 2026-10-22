// min_reader.cpp — минимальный драйвер: cs2.exe -> client.dll -> dwLocalPlayerPawn
// Логирует в C:\Users\Public\Desktop\min_reader.log (через ZwWriteFile)
// build: MSVC + WDK, /std:c++20 /kernel /O2 /GS- /GR- /EHs-c- /EHc- /Zl

#include <ntifs.h>
#include <ntddk.h>
#include <stdarg.h>

extern "C" NTSTATUS ZwQuerySystemInformation(ULONG, PVOID, ULONG, PULONG);
extern "C" NTSTATUS ZwCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                 PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
extern "C" NTSTATUS ZwWriteFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK,
                                PVOID, ULONG, PLARGE_INTEGER, PULONG);
extern "C" NTSTATUS ZwClose(HANDLE);

constexpr ULONG   POOL_TAG = 'rdmN';
constexpr ULONG64 OFF_dwLocalPlayerPawn = 0x2565718ULL;
constexpr ULONG64 PHYS_MASK_4K = 0x000FFFFFFFFFF000ULL;
constexpr ULONG64 PHYS_MASK_2M = 0x000FFFFFFFE00000ULL;
constexpr ULONG64 PHYS_MASK_1G = 0x000FFFFFC0000000ULL;
constexpr ULONG64 USER_VA_MAX  = 0x800000000000ULL;

static ULONG64 g_cr3        = 0;
static ULONG64 g_clientBase = 0;
static HANDLE  g_pid        = nullptr;
static volatile LONG g_stop = 0;
static KEVENT  g_evt;
static PKTHREAD g_thread    = nullptr;

// ============================================================================
// Файловый лог (kernel-mode, ZwCreateFile/ZwWriteFile)
// ============================================================================
static HANDLE  g_logFile = nullptr;
static KSPIN_LOCK g_logLock;
static ULONG   g_logOff = 0;

static void LogOpen() {
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    // Public Desktop — доступен всем, без прав пользователя
    RtlInitUnicodeString(&path, L"\\??\\C:\\Users\\Public\\Desktop\\min_reader.log");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);

    LARGE_INTEGER alloc;
    alloc.QuadPart = 1 << 20; // 1 MB
    NTSTATUS st = ZwCreateFile(
        &g_logFile,
        GENERIC_WRITE | SYNCHRONIZE,
        &oa, &iosb, &alloc, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    if (!NT_SUCCESS(st)) {
        DbgPrint("[min] LogOpen fail 0x%08lX\n", (unsigned long)st);
        g_logFile = nullptr;
        return;
    }
    g_logOff = 0;
}

static void LogClose() {
    if (g_logFile) { ZwClose(g_logFile); g_logFile = nullptr; }
}

static char* AppendStr(char* p, char* end, const char* s) {
    if (!s) s = "(null)";
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char* AppendHex(char* p, char* end, unsigned long long v) {
    char tmp[32]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) { int d = (int)(v & 0xF); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    while (n > 0 && p < end) *p++ = tmp[--n];
    return p;
}

static char* AppendInt(char* p, char* end, long long v) {
    if (v < 0) { if (p < end) *p++ = '-'; v = -v; }
    char tmp[32]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && p < end) *p++ = tmp[--n];
    return p;
}

static void Log(const char* fmt, ...) {
    char buf[256];
    char* p = buf;
    char* end = buf + sizeof(buf) - 1;

    va_list ap;
    va_start(ap, fmt);
    while (*fmt && p < end) {
        if (*fmt != '%') { *p++ = *fmt++; continue; }
        ++fmt;
        int l = 0;
        while (*fmt == 'l') { ++l; ++fmt; }
        bool ll = (l >= 2);
        if (*fmt == 'x' || *fmt == 'X') {
            ++fmt;
            unsigned long long v = ll ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
            p = AppendHex(p, end, v);
        } else if (*fmt == 'u') {
            ++fmt;
            unsigned long long v = ll ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
            p = AppendHex(p, end, 0); // заглушка
            (void)v;
        } else if (*fmt == 'd' || *fmt == 'i') {
            ++fmt;
            long long v = ll ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            p = AppendInt(p, end, v);
        } else if (*fmt == 'p') {
            ++fmt;
            p = AppendStr(p, end, "0x");
            p = AppendHex(p, end, (unsigned long long)(ULONG_PTR)va_arg(ap, void*));
        } else if (*fmt == 's') {
            ++fmt;
            p = AppendStr(p, end, va_arg(ap, const char*));
        } else {
            if (p < end) *p++ = '%';
            if (*fmt) { if (p < end) *p++ = *fmt++; }
        }
    }
    va_end(ap);
    if (p < end) *p++ = '\r';
    if (p < end) *p++ = '\n';
    *p = 0;

    DbgPrint("[min] %s", buf);

    if (!g_logFile) return;
    KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_logLock);
    if (g_logFile) {
        IO_STATUS_BLOCK iosb;
        LARGE_INTEGER off;
        off.QuadPart = g_logOff;
        ULONG written = 0;
        NTSTATUS st = ZwWriteFile(g_logFile, nullptr, nullptr, nullptr, &iosb,
                                  buf, (ULONG)(p - buf), &off, nullptr);
        if (NT_SUCCESS(st)) g_logOff += (ULONG)(p - buf);
    }
    KeReleaseSpinLock(&g_logLock, irql);
}

// ============================================================================
// Физическое чтение
// ============================================================================
static BOOLEAN ReadPhys(ULONG64 phys, PVOID buf, SIZE_T size) {
    if (!phys || !buf || !size) return FALSE;
    MM_COPY_ADDRESS src = {};
    PHYSICAL_ADDRESS pa = {};
    SIZE_T copied = 0;
    pa.QuadPart = (LONGLONG)phys;
    src.PhysicalAddress = pa;
    return NT_SUCCESS(MmCopyMemory(buf, src, size, MM_COPY_MEMORY_PHYSICAL, &copied)) && copied == size;
}

static ULONG64 ReadPhysQ(ULONG64 phys) {
    ULONG64 v = 0;
    ReadPhys(phys, &v, sizeof(v));
    return v;
}

// ============================================================================
// VA -> PA (без кэша)
// ============================================================================
static ULONG64 VirtToPhys(ULONG64 cr3, ULONG64 va) {
    ULONG64 pml4e = ReadPhysQ((cr3 & PHYS_MASK_4K) + 8ULL * ((va >> 39) & 0x1FF));
    if (!(pml4e & 1)) return 0;
    ULONG64 pdpte = ReadPhysQ((pml4e & PHYS_MASK_4K) + 8ULL * ((va >> 30) & 0x1FF));
    if (!(pdpte & 1)) return 0;
    if (pdpte & 0x80) return (pdpte & PHYS_MASK_1G) + (va & 0x3FFFF000ULL) + (va & 0xFFFULL);
    ULONG64 pde = ReadPhysQ((pdpte & PHYS_MASK_4K) + 8ULL * ((va >> 21) & 0x1FF));
    if (!(pde & 1)) return 0;
    if (pde & 0x80) return (pde & PHYS_MASK_2M) + (va & 0x1FF000ULL) + (va & 0xFFFULL);
    ULONG64 pte = ReadPhysQ((pde & PHYS_MASK_4K) + 8ULL * ((va >> 12) & 0x1FF));
    if (!(pte & 1)) return 0;
    return (pte & PHYS_MASK_4K) + (va & 0xFFFULL);
}

static BOOLEAN ReadCs2(ULONG64 va, PVOID buf, SIZE_T size) {
    if (!g_cr3 || !va || !buf || !size) return FALSE;
    if (va >= USER_VA_MAX || va + size < va || va + size > USER_VA_MAX) return FALSE;
    PUCHAR out = (PUCHAR)buf;
    SIZE_T done = 0;
    while (done < size) {
        ULONG64 curVa = va + done;
        ULONG64 phys = VirtToPhys(g_cr3, curVa);
        if (!phys) return FALSE;
        SIZE_T chunk = 0x1000 - (curVa & 0xFFF);
        if (chunk > size - done) chunk = size - done;
        if (!ReadPhys(phys, out + done, chunk)) return FALSE;
        done += chunk;
    }
    return TRUE;
}

// ============================================================================
// Поиск процесса
// ============================================================================
typedef struct _SPI {
    ULONG NextEntryOffset; ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize; ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark; ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    UNICODE_STRING ImageName; KPRIORITY BasePriority;
    HANDLE UniqueProcessId; HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount; ULONG SessionId; ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize, VirtualSize; ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER ReadOperationCount, WriteOperationCount, OtherOperationCount;
    LARGE_INTEGER ReadTransferCount, WriteTransferCount, OtherTransferCount;
} SPI, *PSPI;

static PEPROCESS FindCs2(HANDLE* outPid) {
    ULONG size = 0x20000;
    PVOID buf = nullptr;
    for (int i = 0; i < 4; ++i) {
        buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, POOL_TAG);
        if (!buf) return nullptr;
        NTSTATUS st = ZwQuerySystemInformation(5, buf, size, &size);
        if (NT_SUCCESS(st)) break;
        ExFreePoolWithTag(buf, POOL_TAG);
        buf = nullptr;
        if (st != STATUS_INFO_LENGTH_MISMATCH) return nullptr;
        size *= 2;
    }
    if (!buf) return nullptr;

    UNICODE_STRING target;
    RtlInitUnicodeString(&target, L"cs2.exe");
    PSPI spi = (PSPI)buf;
    PEPROCESS found = nullptr;
    for (;;) {
        if (spi->ImageName.Buffer && spi->ImageName.Length &&
            RtlEqualUnicodeString(&spi->ImageName, &target, TRUE)) {
            if (NT_SUCCESS(PsLookupProcessByProcessId(spi->UniqueProcessId, &found))) {
                *outPid = spi->UniqueProcessId;
                break;
            }
        }
        if (!spi->NextEntryOffset) break;
        spi = (PSPI)((PUCHAR)spi + spi->NextEntryOffset);
    }
    ExFreePoolWithTag(buf, POOL_TAG);
    return found;
}

static ULONG64 GetPeb(PEPROCESS p) {
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, L"PsGetProcessPeb");
    typedef PVOID(*fnT)(PEPROCESS);
    fnT fn = (fnT)MmGetSystemRoutineAddress(&n);
    return fn ? (ULONG64)fn(p) : 0;
}

static ULONG64 GetCr3(PEPROCESS p) {
    return *(ULONG64*)((PUCHAR)p + 0x28) & PHYS_MASK_4K;
}

// ============================================================================
// Поиск client.dll
// ============================================================================
static ULONG64 FindClientDll() {
    PEPROCESS p = nullptr;
    if (!NT_SUCCESS(PsLookupProcessByProcessId(g_pid, &p))) {
        Log("FMB: PsLookupProcessByProcessId fail");
        return 0;
    }
    ULONG64 peb = GetPeb(p);
    ObDereferenceObject(p);
    Log("FMB: peb=%llx", peb);
    if (!peb) return 0;

    UCHAR dbg = 0;
    if (!ReadCs2(peb + 0x02, &dbg, 1)) {
        Log("FMB: PEB itself unreadable at %llx (CR3 broken)", peb);
        return 0;
    }

    ULONG64 ldr = 0;
    if (!ReadCs2(peb + 0x18, &ldr, 8)) { Log("FMB: ldr READ FAIL"); return 0; }
    if (!ldr) { Log("FMB: ldr == NULL (not loaded yet)"); return 0; }
    Log("FMB: ldr=%llx", ldr);

    ULONG64 head = ldr + 0x10;
    ULONG64 cur = 0;
    if (!ReadCs2(head, &cur, 8)) { Log("FMB: head READ FAIL"); return 0; }
    Log("FMB: head=%llx firstCur=%llx", head, cur);

    if (!cur || cur >= USER_VA_MAX || (cur & 7)) {
        Log("FMB: firstCur INVALID=%llx (CR3 stale?)", cur);
        return 0;
    }

    UNICODE_STRING want;
    RtlInitUnicodeString(&want, L"client.dll");

    for (int i = 0; i < 512 && cur && cur != head; ++i) {
        UCHAR ent[0x80];
        if (!ReadCs2(cur, ent, sizeof(ent))) {
            Log("FMB: ent read fail cur=%llx guard=%d", cur, i);
            return 0;
        }

        ULONG64 dllBase = *(ULONG64*)(ent + 0x30);
        USHORT nameLen  = *(USHORT*)(ent + 0x58);
        ULONG64 nameBuf = *(ULONG64*)(ent + 0x60);

        USHORT mz = 0;
        bool mzOk = dllBase && dllBase < USER_VA_MAX &&
                    ReadCs2(dllBase, &mz, 2) && mz == 0x5A4D;

        if (mzOk && nameBuf && nameLen && nameLen <= 0x100) {
            WCHAR nm[128] = {};
            USHORT len = nameLen;
            if (len > 0xF0) len = 0xF0;
            if (ReadCs2(nameBuf, nm, len)) {
                nm[len / 2] = 0;
                UNICODE_STRING un;
                RtlInitUnicodeString(&un, nm);
                if (RtlEqualUnicodeString(&un, &want, TRUE)) {
                    Log("FMB: client.dll found base=%llx guard=%d", dllBase, i);
                    return dllBase;
                }
            }
        }
        cur = *(ULONG64*)(ent + 0x00);
        if (!cur || cur >= USER_VA_MAX || (cur & 7)) {
            Log("FMB: bad Flink=%llx guard=%d", cur, i);
            break;
        }
    }
    Log("FMB: client.dll NOT found (guard end)");
    return 0;
}

// ============================================================================
// Рабочий поток
// ============================================================================
static VOID Worker(PVOID) {
    LARGE_INTEGER iv;
    iv.QuadPart = -50000000LL; // 5 сек

    for (;;) {
        if (InterlockedCompareExchange(&g_stop, 0, 0)) break;

        if (!g_cr3) {
            HANDLE pid = nullptr;
            PEPROCESS p = FindCs2(&pid);
            if (p) {
                g_cr3 = GetCr3(p);
                g_pid = pid;
                ObDereferenceObject(p);
                Log("cs2 found: pid=%p cr3=%llx peb=%llx", g_pid, g_cr3, GetPeb(nullptr));
            } else {
                Log("cs2.exe not found");
            }
        }

        if (g_cr3 && !g_clientBase) {
            g_clientBase = FindClientDll();
        }

        if (g_clientBase) {
            ULONG64 lp = 0;
            if (ReadCs2(g_clientBase + OFF_dwLocalPlayerPawn, &lp, 8)) {
                Log("READ OK: client.dll=%llx dwLocalPlayerPawn=%llx", g_clientBase, lp);
            } else {
                Log("READ FAIL dwLocalPlayerPawn (translation?)");
                g_clientBase = 0;
            }
        }

        KeWaitForSingleObject(&g_evt, Executive, KernelMode, FALSE, &iv);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING) {
    g_cr3 = 0; g_clientBase = 0; g_pid = nullptr; g_stop = 0; g_thread = nullptr;
    KeInitializeSpinLock(&g_logLock);
    KeInitializeEvent(&g_evt, NotificationEvent, FALSE);

    LogOpen();
    Log("DriverEntry ok");

    HANDLE h = nullptr;
    NTSTATUS st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, nullptr, nullptr, nullptr, Worker, nullptr);
    if (!NT_SUCCESS(st)) { Log("PsCreateSystemThread fail 0x%08lX", (unsigned long)st); LogClose(); return st; }

    st = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                   (PVOID*)&g_thread, nullptr);
    ZwClose(h);
    if (!NT_SUCCESS(st)) {
        InterlockedExchange(&g_stop, 1);
        KeSetEvent(&g_evt, 0, FALSE);
        Log("ObReferenceObjectByHandle fail");
        LogClose();
        return st;
    }
    return STATUS_SUCCESS;
}
