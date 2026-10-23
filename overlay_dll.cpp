// overlay_dll.cpp — C++20 multithreaded async proxy version.dll + DX11 VSync ImGui overlay
// build: g++ -std=c++20 -O3 -march=native -flto -shared -municode -DUNICODE -D_UNICODE \
//        -DWIN32_LEAN_AND_MEAN -DNOMINMAX -I. overlay_dll.cpp imgui.cpp \
//        imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp \
//        imgui_impl_win32.cpp imgui_impl_dx11.cpp -o version.dll \
//        -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lgdi32 -lole32 -limm32 \
//        -static-libgcc -static-libstdc++ -static -Wl,--kill-at

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// ===========================================================================
// Контракт Shared Memory (1-в-1 с esp_drv.cpp)
// ===========================================================================
constexpr int MAX_ENTITIES = 256;
constexpr int MAX_BONES    = 24;
constexpr int MAX_LOGS     = 256;
constexpr int LOG_LINE_MAX = 128;

#pragma pack(push, 1)
struct SharedBone { float x, y, z; };
struct SharedEntity { int valid; int hp; int team; int pad0; SharedBone bones[MAX_BONES]; };
struct SharedHeader { volatile LONG seq; ULONG maxEntities; ULONG entityCount; ULONG version; };
struct SharedLogEntry { ULONG tick; char text[LOG_LINE_MAX]; };
struct SharedLog { volatile LONG head; volatile LONG tail; SharedLogEntry entries[MAX_LOGS]; };

struct SharedConfig {
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

// ВАЖНО: cfg должен быть первым полем — драйвер читает его из начала блока
struct SharedBlock { SharedConfig cfg; SharedHeader hdr; SharedLog log; SharedEntity ents[MAX_ENTITIES]; };
#pragma pack(pop)

constexpr ULONG CFG_MAGIC = 0x45535043; // 'ESPC'

static std::atomic<SharedBlock*> g_shm{nullptr};
static std::atomic<float*>       g_view{nullptr};
static HANDLE                    g_map     = nullptr;
static HANDLE                    g_viewMap = nullptr;
static std::atomic<bool>         g_run{true};

static std::atomic<int>          g_W{1920};
static std::atomic<int>          g_H{1080};

// Рандомные имена, читаются драйвером/нами из конфиг-файла
static wchar_t g_sharedMapName[128] = {};
static wchar_t g_viewMapName[128]   = {};
static wchar_t g_logPath[MAX_PATH]  = {};

// ===========================================================================
// Тройной буфер
// ===========================================================================
constexpr size_t BONE_CONNS_COUNT = 17;
static constexpr std::array<std::pair<uint8_t, uint8_t>, BONE_CONNS_COUNT> CONN = {{
    {1,3}, {3,4}, {4,23}, {23,6}, {6,7}, {6,9}, {9,10}, {10,11},
    {6,13}, {13,14}, {14,15}, {1,17}, {17,18}, {18,19}, {1,20}, {20,21}, {21,22}
}};

struct RenderReadyEntity {
    int   hp;
    bool  hasHead;
    float headX, headY, headRadius;
    uint32_t lineCount;
    struct LineSeg { float x1, y1, x2, y2; } lines[BONE_CONNS_COUNT];
};

struct RenderSnapshot {
    uint32_t          count = 0;
    LONG              seq   = -1;
    ULONG             ver   = 0;
    RenderReadyEntity ents[64];
};

static RenderSnapshot        g_snapshots[3];
static std::atomic<uint32_t> g_sharedSnapState{1};
static uint32_t              g_readIdx = 2;

// ===========================================================================
// Логирование (SRWLOCK)
// ===========================================================================
static FILE*   g_logFile       = nullptr;
static SRWLOCK g_logLock       = SRWLOCK_INIT;
static LONG    g_logFlushCount = 0;

// ---- Рандомные имена: окно, класс, файл лога, конфиг ----
static wchar_t g_winClass[64] = {};
static wchar_t g_winTitle[64] = {};

static __forceinline unsigned long long Xs64(unsigned long long& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
}

static void GenRandomNames() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    unsigned long long seed = (unsigned long long)t.QuadPart
                            ^ GetTickCount64()
                            ^ ((unsigned long long)GetCurrentProcessId() << 32);
    if (!seed) seed = 0x9E3779B97F4A7C15ULL;

    unsigned long long a = Xs64(seed);
    unsigned long long b = Xs64(seed);

    _snwprintf_s(g_winClass, 64, _TRUNCATE, L"Cls_%08lx%08lx",
                 (unsigned long)(a >> 32), (unsigned long)a);
    _snwprintf_s(g_winTitle, 64, _TRUNCATE, L"Wnd_%08lx%08lx",
                 (unsigned long)(b >> 16), (unsigned long)(b >> 48));

    wchar_t tmp[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp)) {
        _snwprintf_s(g_logPath, MAX_PATH, _TRUNCATE, L"%slog_%08lx%08lx.tmp",
                     tmp, (unsigned long)(seed >> 32), (unsigned long)seed);
    }
}

// Файл конфига: %TEMP%\~cfg.dat — содержит g_sharedMapName\0g_viewMapName\0
static void WriteConfigFile() {
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    wchar_t cfg[MAX_PATH];
    _snwprintf_s(cfg, MAX_PATH, _TRUNCATE, L"%s~cfg.dat", tmp);

    FILE* f = nullptr;
    _wfopen_s(&f, cfg, L"wb");
    if (!f) return;
    fwrite(g_sharedMapName, sizeof(wchar_t), wcslen(g_sharedMapName) + 1, f);
    fwrite(g_viewMapName,   sizeof(wchar_t), wcslen(g_viewMapName) + 1, f);
    fclose(f);
}

// Создаём секции сами (usermode), чтобы задать рандомные имена.
// Драйвер увидит STATUS_OBJECT_NAME_COLLISION и откроет их.
static void CreateSharedNamesAndSections() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    unsigned long long seed = (unsigned long long)t.QuadPart
                            ^ GetTickCount64()
                            ^ ((unsigned long long)GetCurrentProcessId() << 16);
    if (!seed) seed = 0xD1B54A32D192ED03ULL;

    unsigned long long a = Xs64(seed);
    unsigned long long b = Xs64(seed);

    _snwprintf_s(g_sharedMapName, 128, _TRUNCATE, L"\\BaseNamedObjects\\%08lx%08lx_s",
                 (unsigned long)(a >> 32), (unsigned long)a);
    _snwprintf_s(g_viewMapName, 128, _TRUNCATE, L"\\BaseNamedObjects\\%08lx%08lx_v",
                 (unsigned long)(b >> 32), (unsigned long)b);

    // Создаём сами секции с рандомными именами. Драйвер откроет их по collision.
    HANDLE hShared = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(SharedBlock), g_sharedMapName);
    if (hShared) {
        void* p = MapViewOfFile(hShared, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock));
        if (p) {
            auto* blk = (SharedBlock*)p;
            // Заполняем конфиг (offsets + magic). PID и clientBase — позже.
            blk->cfg.offEntityList      = 0x271A908ULL;
            blk->cfg.offLocalPlayerPawn = 0x2565718ULL;
            blk->cfg.offViewMatrix      = 0x256B020ULL;
            blk->cfg.offGameSceneNode   = 0x330ULL;
            blk->cfg.offHealth          = 0x34CULL;
            blk->cfg.offLifeState       = 0x354ULL;
            blk->cfg.offTeamNum         = 0x3E7ULL;
            blk->cfg.offModelState      = 0x140ULL;
            blk->cfg.offPlayerPawn      = 0x92CULL;
            blk->cfg.magic              = CFG_MAGIC;
            blk->cfg.ready              = 0;   // PID/base заполним ниже
            blk->hdr.version            = 1;
            blk->hdr.maxEntities        = MAX_ENTITIES;
        }
        // hShared не закрываем — пусть висит (или можно закрыть, драйвер откроет по имени)
    }

    HANDLE hView = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      0, 0x1000, g_viewMapName);
    (void)hView;

    WriteConfigFile();
}

// Найти PID cs2.exe (usermode)
static DWORD FindCs2Pid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"cs2.exe") == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Найти базу client.dll в процессе (usermode)
static ULONG64 FindClientBase(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    ULONG64 base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"client.dll") == 0) {
                base = (ULONG64)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

// Заполнить cfg.pid / cfg.clientBase, когда они станут известны
static void PublishPidAndBase() {
    SharedBlock* shm = g_shm.load(std::memory_order_acquire);
    if (!shm) return;
    if (shm->cfg.magic != CFG_MAGIC) return;
    if (shm->cfg.ready == 1 && shm->cfg.pid != 0 && shm->cfg.clientBase != 0) return;

    DWORD pid = FindCs2Pid();
    if (!pid) return;
    ULONG64 base = FindClientBase(pid);
    if (!base) return;

    shm->cfg.pid        = (ULONG)pid;
    shm->cfg.clientBase = base;
    shm->cfg.magic      = CFG_MAGIC;
    shm->cfg.ready      = 1;
}

static void LogOpen() {
    if (!g_logPath[0]) GenRandomNames();
    if (!g_logPath[0]) return;
    char pathA[MAX_PATH * 2];
    if (!WideCharToMultiByte(CP_UTF8, 0, g_logPath, -1, pathA, sizeof(pathA), nullptr, nullptr)) return;
    fopen_s(&g_logFile, pathA, "a");
}

static void LogSelf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[640];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[%02u:%02u:%02u.%03u][self] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);

    OutputDebugStringA(line);

    AcquireSRWLockExclusive(&g_logLock);
    if (g_logFile) {
        fputs(line, g_logFile);
        if (++g_logFlushCount >= 64) {
            g_logFlushCount = 0;
            fflush(g_logFile);
        }
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

static void LogKernelLine(const SharedLogEntry& le) {
    if (!le.text[0]) return;
    char line[640];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[k #%u] %s\n", le.tick, le.text);
    OutputDebugStringA(line);

    AcquireSRWLockExclusive(&g_logLock);
    if (g_logFile) {
        fputs(line, g_logFile);
        if (++g_logFlushCount >= 64) {
            g_logFlushCount = 0;
            fflush(g_logFile);
        }
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

static LONG g_lastKernelTail = 0;
static void DrainKernelLogs() {
    SharedBlock* shm = g_shm.load(std::memory_order_acquire);
    if (!shm) return;
    LONG tail = shm->log.tail;
    if (tail == g_lastKernelTail) return;
    if (tail < g_lastKernelTail) g_lastKernelTail = 0;
    LONG start = g_lastKernelTail;
    LONG count = tail - start;
    if (count > MAX_LOGS) { start = tail - MAX_LOGS; count = MAX_LOGS; }
    for (LONG i = 0; i < count; ++i) {
        LONG idx = (start + i) % MAX_LOGS;
        if (idx < 0) idx += MAX_LOGS;
        LogKernelLine(shm->log.entries[idx]);
    }
    g_lastKernelTail = tail;
}

// ===========================================================================
// Proxy version.dll
// ===========================================================================
typedef WINBOOL (WINAPI* fnGVIA)(LPCSTR,DWORD,DWORD,LPVOID);
typedef WINBOOL (WINAPI* fnGVIW)(LPCWSTR,DWORD,DWORD,LPVOID);
typedef DWORD   (WINAPI* fnGVISA)(LPCSTR,LPDWORD);
typedef DWORD   (WINAPI* fnGVISW)(LPCWSTR,LPDWORD);
typedef WINBOOL (WINAPI* fnVQVA)(LPCVOID,LPCSTR,LPVOID*,PUINT);
typedef WINBOOL (WINAPI* fnVQVW)(LPCVOID,LPCWSTR,LPVOID*,PUINT);

static fnGVIA  pGVIA  = nullptr;
static fnGVIW  pGVIW  = nullptr;
static fnGVISA pGVISA = nullptr;
static fnGVISW pGVISW = nullptr;
static fnVQVA  pVQVA  = nullptr;
static fnVQVW  pVQVW  = nullptr;

extern "C" __declspec(dllexport) WINBOOL WINAPI GetFileVersionInfoA(LPCSTR a,DWORD b,DWORD c,LPVOID d){ return pGVIA ? pGVIA(a,b,c,d) : FALSE; }
extern "C" __declspec(dllexport) WINBOOL WINAPI GetFileVersionInfoW(LPCWSTR a,DWORD b,DWORD c,LPVOID d){ return pGVIW ? pGVIW(a,b,c,d) : FALSE; }
extern "C" __declspec(dllexport) DWORD   WINAPI GetFileVersionInfoSizeA(LPCSTR a,LPDWORD b){ return pGVISA ? pGVISA(a,b) : 0; }
extern "C" __declspec(dllexport) DWORD   WINAPI GetFileVersionInfoSizeW(LPCWSTR a,LPDWORD b){ return pGVISW ? pGVISW(a,b) : 0; }
extern "C" __declspec(dllexport) WINBOOL WINAPI VerQueryValueA(LPCVOID a,LPCSTR b,LPVOID* c,PUINT d){ return pVQVA ? pVQVA(a,b,c,d) : FALSE; }
extern "C" __declspec(dllexport) WINBOOL WINAPI VerQueryValueW(LPCVOID a,LPCWSTR b,LPVOID* c,PUINT d){ return pVQVW ? pVQVW(a,b,c,d) : FALSE; }

static void LoadReal() {
    wchar_t p[MAX_PATH];
    GetSystemDirectoryW(p, MAX_PATH);
    wcscat_s(p, L"\\version.dll");
    HMODULE h = LoadLibraryW(p);
    if (!h) return;
    pGVIA  = (fnGVIA) GetProcAddress(h, "GetFileVersionInfoA");
    pGVIW  = (fnGVIW) GetProcAddress(h, "GetFileVersionInfoW");
    pGVISA = (fnGVISA)GetProcAddress(h, "GetFileVersionInfoSizeA");
    pGVISW = (fnGVISW)GetProcAddress(h, "GetFileVersionInfoSizeW");
    pVQVA  = (fnVQVA) GetProcAddress(h, "VerQueryValueA");
    pVQVW  = (fnVQVW) GetProcAddress(h, "VerQueryValueW");
}

// ===========================================================================
// Математика и Async Worker
// ===========================================================================
static __forceinline bool FastW2S(const float* __restrict m, float x, float y, float z,
                                  float cx, float cy, float& sx, float& sy) noexcept {
    const float sw = m[12] * x + m[13] * y + m[14] * z + m[15];
    if (sw <= 0.001f) return false;
    const float invW = 1.0f / sw;
    sx = cx + (cx * (m[0] * x + m[1] * y + m[2] * z + m[3]) * invW);
    sy = cy - (cy * (m[4] * x + m[5] * y + m[6] * z + m[7]) * invW);
    return true;
}

static DWORD WINAPI AsyncDataWorkerThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    SharedEntity localEnts[64];
    float        vm[16];
    uint32_t     writeIdx = 0;
    LONG         lastProcessedSeq = -1;
    int          spin = 0;

    while (g_run.load(std::memory_order_relaxed)) {
        SharedBlock* shm  = g_shm.load(std::memory_order_acquire);
        float*       view = g_view.load(std::memory_order_acquire);

        if (!shm || !view) {
            Sleep(5);
            continue;
        }

        LONG s1 = shm->hdr.seq;
        if (s1 & 1) {
            YieldProcessor();
            continue;
        }

        if (s1 == lastProcessedSeq) {
            if (++spin < 100) {
                YieldProcessor();
            } else {
                spin = 0;
                SwitchToThread();
            }
            continue;
        }
        spin = 0;

        std::atomic_thread_fence(std::memory_order_acquire);

        ULONG n = shm->hdr.entityCount;
        if (n > 64) n = 64;
        if (n > 0) {
            std::memcpy(localEnts, (const void*)shm->ents, n * sizeof(SharedEntity));
        }
        ULONG ver = shm->hdr.version;
        std::memcpy(vm, (const void*)view, sizeof(vm));

        std::atomic_thread_fence(std::memory_order_acquire);
        LONG s2 = shm->hdr.seq;
        if (s1 != s2) {
            YieldProcessor();
            continue;
        }

        lastProcessedSeq = s2;

        const float cx = g_W.load(std::memory_order_relaxed) * 0.5f;
        const float cy = g_H.load(std::memory_order_relaxed) * 0.5f;

        RenderSnapshot& snap = g_snapshots[writeIdx];
        snap.seq = s2;
        snap.ver = ver;
        uint32_t outCount = 0;

        for (ULONG e = 0; e < n; ++e) {
            const SharedEntity& en = localEnts[e];
            if (!en.valid || en.hp <= 0) continue;

            float sx[MAX_BONES];
            float sy[MAX_BONES];
            bool  ok[MAX_BONES]{};

            for (int b = 0; b < MAX_BONES; ++b) {
                const auto& bone = en.bones[b];
                if (bone.x == 0.f && bone.y == 0.f && bone.z == 0.f) continue;
                ok[b] = FastW2S(vm, bone.x, bone.y, bone.z, cx, cy, sx[b], sy[b]);
            }

            RenderReadyEntity& re = snap.ents[outCount];
            re.hp = en.hp;
            re.lineCount = 0;

            for (const auto& [b1, b2] : CONN) {
                if (ok[b1] && ok[b2]) {
                    auto& seg = re.lines[re.lineCount++];
                    seg.x1 = sx[b1]; seg.y1 = sy[b1];
                    seg.x2 = sx[b2]; seg.y2 = sy[b2];
                }
            }

            re.hasHead = ok[7];
            if (ok[7]) {
                re.headX = sx[7];
                re.headY = sy[7];
                float r = 4.0f;
                if (ok[6]) {
                    const float dx = sx[7] - sx[6];
                    const float dy = sy[7] - sy[6];
                    const float dd = std::sqrt(dx * dx + dy * dy);
                    r = dd < 2.f ? 2.f : (dd > 15.f ? 15.f : dd);
                }
                re.headRadius = r;
            }
            ++outCount;
        }

        snap.count = outCount;

        uint32_t prev = g_sharedSnapState.exchange(writeIdx | 0x80u, std::memory_order_acq_rel);
        writeIdx = prev & 0x03u;
    }
    return 0;
}

// ===========================================================================
// D3D11
// ===========================================================================
static HWND                    g_hWnd            = nullptr;
static ID3D11Device*           g_dev             = nullptr;
static ID3D11DeviceContext*    g_ctx             = nullptr;
static IDXGISwapChain*         g_sc              = nullptr;
static ID3D11RenderTargetView* g_rtv             = nullptr;
static HANDLE                  g_frameWaitable   = nullptr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        g_run.store(false, std::memory_order_release);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool CreateDev(HWND h) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width  = g_W.load();
    sd.BufferDesc.Height = g_H.load();
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count  = 1;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount       = 2;
    sd.OutputWindow      = h;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags             = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
        fls, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) {
        LogSelf("d3d fail hr=0x%08lX", (unsigned long)hr);
        return false;
    }

    IDXGIDevice1* dxgiDev = nullptr;
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev)) && dxgiDev) {
        dxgiDev->SetMaximumFrameLatency(1);
        dxgiDev->Release();
    }

    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) {
        g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }
    return true;
}

static void CleanDev() {
    if (g_frameWaitable) { CloseHandle(g_frameWaitable); g_frameWaitable = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_sc)  { g_sc->Release();  g_sc = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static void DrawKernelLogs(ImDrawList* d) {
    SharedBlock* shm = g_shm.load(std::memory_order_acquire);
    if (!shm) return;
    LONG tail = shm->log.tail;
    float y = 28.f;
    for (int i = 1; i <= 8; ++i) {
        LONG idx = (tail - i) % MAX_LOGS;
        if (idx < 0) idx += MAX_LOGS;
        const SharedLogEntry& le = shm->log.entries[idx];
        if (!le.text[0]) continue;
        char buf[LOG_LINE_MAX + 32];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "#%u %s", le.tick, le.text);
        d->AddText({10.f, y}, IM_COL32(0, 200, 0, 220), buf);
        y += 15.f;
    }
}

static void Render(ImDrawList* d) {
    uint32_t state = g_sharedSnapState.load(std::memory_order_acquire);
    if (state & 0x80u) {
        uint32_t prev = g_sharedSnapState.exchange(g_readIdx, std::memory_order_acq_rel);
        g_readIdx = prev & 0x03u;
    }
    const RenderSnapshot& snap = g_snapshots[g_readIdx];

    char statusLine[256];
    _snprintf_s(statusLine, sizeof(statusLine), _TRUNCATE,
        "shm=%s view=%s | seq=%ld entities=%u ver=%lu | W=%d H=%d",
        g_shm.load(std::memory_order_relaxed) ? "OK" : "NULL",
        g_view.load(std::memory_order_relaxed) ? "OK" : "NULL",
        snap.seq, snap.count, snap.ver,
        g_W.load(std::memory_order_relaxed), g_H.load(std::memory_order_relaxed));
    d->AddText({10.f, 8.f}, IM_COL32(0, 255, 0, 255), statusLine);

    DrawKernelLogs(d);

    constexpr ImU32 red   = IM_COL32(255, 0, 0, 255);
    constexpr ImU32 white = IM_COL32(255, 255, 255, 255);

    for (uint32_t i = 0; i < snap.count; ++i) {
        const RenderReadyEntity& en = snap.ents[i];
        for (uint32_t l = 0; l < en.lineCount; ++l) {
            const auto& seg = en.lines[l];
            d->AddLine({seg.x1, seg.y1}, {seg.x2, seg.y2}, red, 2.0f);
        }
        if (en.hasHead) {
            d->AddCircle({en.headX, en.headY}, en.headRadius, white, 16, 1.5f);
            char hpBuf[32];
            _snprintf_s(hpBuf, sizeof(hpBuf), _TRUNCATE, "HP: %d", en.hp);
            d->AddText({en.headX - 15.f, en.headY - en.headRadius - 15.f}, red, hpBuf);
        }
    }
}

static DWORD WINAPI DiagThread(LPVOID) {
    unsigned diagCounter = 0;
    while (g_run.load(std::memory_order_relaxed)) {
        if (!g_shm.load(std::memory_order_acquire)) {
            HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g_sharedMapName);
            if (m) {
                auto* ptr = (SharedBlock*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock));
                if (ptr) {
                    g_map = m;
                    g_shm.store(ptr, std::memory_order_release);
                    LogSelf("diag: shm ok %p", ptr);
                    PublishPidAndBase();
                } else {
                    CloseHandle(m);
                }
            }
        }
        if (!g_view.load(std::memory_order_acquire)) {
            HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g_viewMapName);
            if (m) {
                auto* ptr = (float*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0x1000);
                if (ptr) {
                    g_viewMap = m;
                    g_view.store(ptr, std::memory_order_release);
                    LogSelf("diag: view ok %p", ptr);
                } else {
                    CloseHandle(m);
                }
            }
        }

        PublishPidAndBase();
        DrainKernelLogs();

        if (++diagCounter >= 5) {
            diagCounter = 0;
            SharedBlock* shm = g_shm.load(std::memory_order_relaxed);
            if (shm) {
                LogSelf("diag: seq=%ld entities=%lu ver=%lu logTail=%ld",
                        shm->hdr.seq, shm->hdr.entityCount, shm->hdr.version, shm->log.tail);
            }
        }
        Sleep(5000);
    }
    return 0;
}

static DWORD WINAPI OverlayThread(LPVOID) {
    Sleep(3000);

    const int w = GetSystemMetrics(SM_CXSCREEN);
    const int h = GetSystemMetrics(SM_CYSCREEN);
    g_W.store(w, std::memory_order_relaxed);
    g_H.store(h, std::memory_order_relaxed);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr),
                       LoadIcon(nullptr, IDI_APPLICATION), LoadCursor(nullptr, IDC_ARROW),
                       nullptr, nullptr, g_winClass, nullptr };
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_APPWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, g_winTitle, WS_POPUP, 0, 0, w, h,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hWnd) return 0;

    MARGINS m = { -1 };
    DwmExtendFrameIntoClientArea(g_hWnd, &m);
    SetLayeredWindowAttributes(g_hWnd, 0, 255, LWA_ALPHA);

    if (!CreateDev(g_hWnd)) {
        CleanDev();
        DestroyWindow(g_hWnd);
        return 0;
    }
    ShowWindow(g_hWnd, SW_SHOW);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    MSG msg = {};
    while (g_run.load(std::memory_order_relaxed)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_run.store(false, std::memory_order_release);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_run.load(std::memory_order_relaxed)) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowSize({(float)w, (float)h});
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::Begin("ov", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings);

        Render(ImGui::GetWindowDrawList());

        ImGui::End();
        ImGui::Render();

        constexpr float clr[4] = {0.f, 0.f, 0.f, 0.f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_sc->Present(1, 0);
        if (hr == DXGI_STATUS_OCCLUDED) {
            DwmFlush();
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanDev();
    DestroyWindow(g_hWnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

static DWORD WINAPI InitThread(LPVOID) {
    GenRandomNames();
    CreateSharedNamesAndSections();
    LogOpen();
    LoadReal();

    HANDLE mShared = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g_sharedMapName);
    if (mShared) {
        auto* ptr = (SharedBlock*)MapViewOfFile(mShared, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock));
        if (ptr) {
            g_map = mShared;
            g_shm.store(ptr, std::memory_order_release);
            PublishPidAndBase();
        } else {
            CloseHandle(mShared);
        }
    }

    HANDLE mView = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g_viewMapName);
    if (mView) {
        auto* ptr = (float*)MapViewOfFile(mView, FILE_MAP_ALL_ACCESS, 0, 0, 0x1000);
        if (ptr) {
            g_viewMap = mView;
            g_view.store(ptr, std::memory_order_release);
        } else {
            CloseHandle(mView);
        }
    }

    CreateThread(nullptr, 0, DiagThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, AsyncDataWorkerThread, nullptr, 0, nullptr);

    OverlayThread(nullptr);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    } else if (r == DLL_PROCESS_DETACH) {
        g_run.store(false, std::memory_order_release);
    }
    return TRUE;
}
