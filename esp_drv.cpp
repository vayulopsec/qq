// esp_drv.cpp — CS2 ESP kernel driver (manual map, no SEH, no STL/RTTI/exceptions)
// build: MSVC + WDK, /kernel /GS- /GR- /EHs-c- /Zl
// Entry: DriverEntry (called by manual mapper). No IoCreateDevice — comms via shared memory only.

#include <ntifs.h>
#include <ntddk.h>
#include <intrin.h>

#define POOL_TAG      'spSE'
#define MAX_ENTITIES  256
#define MAX_BONES     24

// ---- CS2 offsets (from a2x cs2-dumper, offsets.hpp + client_dll.hpp) ----
#define OFF_dwEntityList        0x271A908ULL
#define OFF_dwLocalPlayerPawn   0x2565718ULL
#define OFF_dwViewMatrix        0x256B020ULL
#define OFF_m_iTeamNum          0x3E7ULL
#define OFF_m_lifeState         0x354ULL
#define OFF_m_iHealth           0x34CULL
#define OFF_m_pGameSceneNode    0x330ULL
#define OFF_m_modelState        0x140ULL
#define OFF_m_hPlayerPawn       0x92CULL

// CSkeletonInstance sits at CBodyComponentSkeletonInstance + 0x80,
// and m_modelState is at CSkeletonInstance + 0x140.
// Pawn->m_pGameSceneNode actually points to CGameSceneNode*; for skeletons the
// real skeleton instance is reachable via BodyComponent (0x30) + 0x80. We use
// the same shortcut as user-mode code: scene + 0x80 + m_modelState.
// However main.cpp used scene + m_modelState + 0x80 — the game accepts both
// because m_pGameSceneNode for pawns IS the CSkeletonInstance. We keep:
//   boneMatrix = *(scene + 0x80 + OFF_m_modelState + 0x80)
// where 0x80 = CSkeletonInstance inside CBodyComponentSkeletonInstance,
// and scene itself is CGameSceneNode*, but for player pawns scene is actually
// CSkeletonInstance* (already at the right level). So we use scene directly.
// The user code: bMatrix = *(scene + m_modelState + 0x80) — 0x80 there is
// CModelState.m_hModel offset? No. Actually user code is:
//   scene = *(pawn + m_pGameSceneNode);
//   bMatrix = *(scene + m_modelState + 0x80);
// where 0x80 = ModelState.m_hModel? Let's just replicate user logic exactly.

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
typedef struct _SHARED_BLOCK {
    SHARED_HEADER hdr;
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

// ---- CR3 page walker (physical read via MmMapIoSpace) ----
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
    ULONG64 pml4e = ReadPhysicalQword((cr3 & ~0xFFFULL) + 8 * ((va >> 39) & 0x1FF));
    if (!(pml4e & 1)) return 0;
    ULONG64 pdpte = ReadPhysicalQword((pml4e & ~0xFFFULL) + 8 * ((va >> 30) & 0x1FF));
    if (!(pdpte & 1)) return 0;
    if (pdpte & 0x80) return (pdpte & ~0x3FFFFFFFULL) + (va & 0x3FFFFFFFULL);
    ULONG64 pde = ReadPhysicalQword((pdpte & ~0xFFFULL) + 8 * ((va >> 21) & 0x1FF));
    if (!(pde & 1)) return 0;
    if (pde & 0x80) return (pde & ~0x1FFFFFULL) + (va & 0x1FFFFFULL);
    ULONG64 pte = ReadPhysicalQword((pde & ~0xFFFULL) + 8 * ((va >> 12) & 0x1FF));
    if (!(pte & 1)) return 0;
    return (pte & ~0xFFFULL) + (va & 0xFFFULL);
}

static BOOLEAN ReadCs2Memory(ULONG64 va, PVOID buf, SIZE_T size) {
    if (!g_cs2Cr3 || !va || !buf || !size) return FALSE;
    PUCHAR out = (PUCHAR)buf;
    SIZE_T done = 0;
    while (done < size) {
        ULONG64 cur = va + done;
        ULONG64 phys = VirtualToPhysical(g_cs2Cr3, cur);
        if (!phys) return FALSE;
        SIZE_T chunk = 0x1000 - (cur & 0xFFF);
        if (chunk > size - done) chunk = size - done;
        if (!ReadPhysical(phys, out + done, chunk)) return FALSE;
        done += chunk;
    }
    return TRUE;
}

static BOOLEAN RdQ(ULONG64 va, PULONG64 out) { return ReadCs2Memory(va, out, 8); }
static BOOLEAN RdD(ULONG64 va, PULONG   out) { return ReadCs2Memory(va, out, 4); }
static BOOLEAN RdB(ULONG64 va, PUCHAR   out) { return ReadCs2Memory(va, out, 1); }

// ---- process / module discovery ----
typedef struct _SYSTEM_PROCESS_INFO {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
} SYSTEM_PROCESS_INFO, *PSYSTEM_PROCESS_INFO;

static PEPROCESS FindCs2Process() {
    ULONG size = 0x10000;
    PVOID buf = nullptr;
    NTSTATUS st;
    for (int i = 0; i < 4; ++i) {
        buf = ExAllocatePoolWithTag(NonPagedPool, size, POOL_TAG);
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

static ULONG64 GetProcessCr3(PEPROCESS p) {
    // KPROCESS.DirectoryTableBase at +0x28 on x64
    return *(ULONG64*)((PUCHAR)p + 0x28);
}

// Read PEB->Ldr->InLoadOrderModuleList to find module base (via CR3 walker)
typedef struct _LDR_ENTRY_OFF {
    LIST_ENTRY InLoadOrderLinks; // 0x00
    PVOID ExceptionTable;        // 0x10
    ULONG ExceptionTableSize;    // 0x18
    PVOID GpValue;               // 0x20
    PVOID NonPagedDebugInfo;     // 0x28
    PVOID DllBase;               // 0x30
    PVOID EntryPoint;            // 0x38
    ULONG SizeOfImage;           // 0x40
    UNICODE_STRING FullDllName;  // 0x48
    UNICODE_STRING BaseDllName;  // 0x58
} LDR_ENTRY_OFF;

static ULONG64 FindModuleBase(PEPROCESS proc, PCWSTR moduleName) {
    ULONG64 peb = 0;
    UNICODE_STRING fnName; RtlInitUnicodeString(&fnName, L"PsGetProcessPeb");
    typedef PVOID(*fnPeb)(PEPROCESS);
    fnPeb fn = (fnPeb)MmGetSystemRoutineAddress(&fnName);
    if (fn) peb = (ULONG64)fn(proc);
    if (!peb) return 0;

    ULONG64 ldr = 0;
    if (!RdQ(peb + 0x18, &ldr) || !ldr) return 0;
    ULONG64 head = ldr + 0x10;
    ULONG64 cur = 0;
    if (!RdQ(head, &cur)) return 0;
    for (int guard = 0; guard < 512 && cur && cur != head; ++guard) {
        LDR_ENTRY_OFF e = {};
        if (!ReadCs2Memory(cur, &e, sizeof(e))) return 0;
        if (e.BaseDllName.Buffer && e.BaseDllName.Length) {
            WCHAR n[128] = {};
            USHORT len = e.BaseDllName.Length;
            if (len > sizeof(n) - sizeof(WCHAR)) len = sizeof(n) - sizeof(WCHAR);
            if (ReadCs2Memory((ULONG64)e.BaseDllName.Buffer, n, len)) {
                n[len / sizeof(WCHAR)] = 0;
                if (_wcsicmp(n, moduleName) == 0) return (ULONG64)e.DllBase;
            }
        }
        ULONG64 next = 0;
        if (!RdQ(cur, &next)) return 0;
        cur = next;
    }
    return 0;
}

// ---- GetEntityByIndex (matches user code: entry = *(chunk + 112*slot)) ----
static ULONG64 EntityByIndex(ULONG64 es, ULONG idx) {
    if (!es || idx > 0x7FFE) return 0;
    ULONG c = idx >> 9;
    if (c > 0x3F) return 0;
    ULONG64 chunk = 0;
    if (!RdQ(es + 0x10 + 8ULL * c, &chunk) || !chunk) return 0;
    ULONG64 entry = chunk + 112ULL * (idx & 0x1FF);
    ULONG ident = 0;
    if (!RdD(entry + 0x10, &ident)) return 0;
    if ((ident & 0x7FFF) != idx) return 0;
    ULONG64 ent = 0;
    if (!RdQ(entry, &ent)) return 0;
    return ent;
}

// ---- worker ----
static void Tick() {
    PEPROCESS p = FindCs2Process();
    if (!p) return;
    g_cs2Cr3 = GetProcessCr3(p);
    g_clientBase = FindModuleBase(p, L"client.dll");
    ObDereferenceObject(p);
    if (!g_cs2Cr3 || !g_clientBase) return;

    RdQ(g_clientBase + OFF_dwLocalPlayerPawn, &g_localPawn);
    RdQ(g_clientBase + OFF_dwEntityList, &g_entitySystem);

    float vm[16] = {};
    ReadCs2Memory(g_clientBase + OFF_dwViewMatrix, vm, sizeof(vm));
    if (g_viewVa) RtlCopyMemory(g_viewVa, vm, sizeof(vm));

    if (!g_localPawn || !g_entitySystem) {
        if (g_sharedVa) {
            InterlockedIncrement(&g_sharedVa->hdr.seq);
            g_sharedVa->hdr.entityCount = 0;
            InterlockedIncrement(&g_sharedVa->hdr.seq);
        }
        return;
    }

    UCHAR myTeam = 0;
    RdB(g_localPawn + OFF_m_iTeamNum, &myTeam);

    if (g_sharedVa) InterlockedIncrement(&g_sharedVa->hdr.seq);
    ULONG out = 0;

    for (ULONG i = 1; i <= 64 && out < MAX_ENTITIES; ++i) {
        ULONG64 ctrl = EntityByIndex(g_entitySystem, i);
        if (!ctrl) continue;
        ULONG hPawn = 0;
        if (!RdD(ctrl + OFF_m_hPlayerPawn, &hPawn)) continue;
        if (!hPawn || hPawn == 0xFFFFFFFF) continue;
        ULONG64 pawn = EntityByIndex(g_entitySystem, hPawn & 0x7FFF);
        if (!pawn || pawn == g_localPawn) continue;

        int hp = 0;
        UCHAR ls = 0, team = 0;
        RdD(pawn + OFF_m_iHealth, (PULONG)&hp);
        RdB(pawn + OFF_m_lifeState, &ls);
        RdB(pawn + OFF_m_iTeamNum, &team);
        if (hp <= 0 || hp > 200 || ls != 256 || team == myTeam) continue;

        // scene = *(pawn + m_pGameSceneNode); then bone matrix = *(scene + 0x80 + m_modelState + 0x80)
        // user code used: scene + m_modelState + 0x80, where 0x80 is ModelState.m_hModel offset — but
        // in fact CSkeletonInstance sits at CBodyComponentSkeletonInstance + 0x80, and m_pGameSceneNode
        // for a player pawn points to the CSkeletonInstance (not raw CGameSceneNode). So user code
        // scene + 0x140 + 0x80 = +0x1C0 is the bone matrix pointer location inside CSkeletonInstance.
        ULONG64 scene = 0, bMat = 0;
        if (!RdQ(pawn + OFF_m_pGameSceneNode, &scene) || !scene) continue;
        if (!RdQ(scene + OFF_m_modelState + 0x80, &bMat) || !bMat) continue;

        SHARED_ENTITY e = {};
        e.valid = 1; e.hp = hp; e.team = team;
        for (int b = 0; b < MAX_BONES; ++b) {
            float tmp[3] = {};
            if (ReadCs2Memory(bMat + (ULONG64)b * 0x20, tmp, sizeof(tmp))) {
                e.bones[b].x = tmp[0];
                e.bones[b].y = tmp[1];
                e.bones[b].z = tmp[2];
            }
        }
        if (g_sharedVa) g_sharedVa->ents[out] = e;
        ++out;
    }
    if (g_sharedVa) {
        g_sharedVa->hdr.entityCount = out;
        g_sharedVa->hdr.version = 1;
        InterlockedIncrement(&g_sharedVa->hdr.seq);
    }
}

static VOID WorkerThread(PVOID) {
    LARGE_INTEGER interval;
    interval.QuadPart = -10 * 1000 * 10; // 10ms
    for (;;) {
        if (InterlockedCompareExchange(&g_stop, 0, 0)) break;
        Tick();
        KeWaitForSingleObject(&g_workerEvent, Executive, KernelMode, FALSE, &interval);
    }
}

// ---- shared memory ----
static NTSTATUS CreateShared() {
    OBJECT_ATTRIBUTES oa; UNICODE_STRING name; LARGE_INTEGER sz; NTSTATUS st;

    RtlInitUnicodeString(&name, L"\\BaseNamedObjects\\Cs2EspShared_v1");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    sz.QuadPart = sizeof(SHARED_BLOCK);
    st = ZwCreateSection(&g_hSharedSection, SECTION_ALL_ACCESS, &oa, &sz, PAGE_READWRITE, SEC_COMMIT, nullptr);
    if (!NT_SUCCESS(st)) return st;
    PVOID base = nullptr; SIZE_T vs = 0;
    st = ZwMapViewOfSection(g_hSharedSection, ZwCurrentProcess(), &base, 0, 0, nullptr, &vs, ViewUnmap, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(st)) return st;
    g_sharedVa = (SHARED_BLOCK*)base;
    g_sharedVa->hdr.version = 1;
    g_sharedVa->hdr.maxEntities = MAX_ENTITIES;

    RtlInitUnicodeString(&name, L"\\BaseNamedObjects\\Cs2EspView_v1");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    sz.QuadPart = 64;
    st = ZwCreateSection(&g_hViewSection, SECTION_ALL_ACCESS, &oa, &sz, PAGE_READWRITE, SEC_COMMIT, nullptr);
    if (!NT_SUCCESS(st)) return st;
    base = nullptr;
    st = ZwMapViewOfSection(g_hViewSection, ZwCurrentProcess(), &base, 0, 0, nullptr, &vs, ViewUnmap, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(st)) return st;
    g_viewVa = (float*)base;
    return STATUS_SUCCESS;
}

static void DestroyShared() {
    if (g_viewVa && g_hViewSection)   { ZwUnmapViewOfSection(ZwCurrentProcess(), g_viewVa); g_viewVa = nullptr; }
    if (g_hViewSection)               { ZwClose(g_hViewSection); g_hViewSection = nullptr; }
    if (g_sharedVa && g_hSharedSection){ ZwUnmapViewOfSection(ZwCurrentProcess(), g_sharedVa); g_sharedVa = nullptr; }
    if (g_hSharedSection)             { ZwClose(g_hSharedSection); g_hSharedSection = nullptr; }
}

// ---- DriverEntry (manual map safe) ----
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING) {
    NTSTATUS st = CreateShared();
    if (!NT_SUCCESS(st)) return st;

    KeInitializeEvent(&g_workerEvent, NotificationEvent, FALSE);

    HANDLE h = nullptr;
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, nullptr, nullptr, nullptr, WorkerThread, nullptr);
    if (!NT_SUCCESS(st)) { DestroyShared(); return st; }
    ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID*)&g_workerThread, nullptr);
    ZwClose(h);

    // For manual mapping we can't rely on DriverUnload being called. We also
    // don't create a device object — nothing to unload from IRP path.
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