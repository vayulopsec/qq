// overlay_dll.cpp — proxy version.dll + ImGui overlay for Discord
// build: g++ -std=c++17 -O2 -shared -municode -DUNICODE -D_UNICODE \
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
#include <dwmapi.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// ===========================================================================
// shared memory contract — MUST match esp_drv.cpp exactly
// ===========================================================================
#pragma pack(push, 1)
struct SharedBone { float x, y, z; };
struct SharedEntity { int valid; int hp; int team; int pad0; SharedBone bones[24]; };
struct SharedHeader { volatile LONG seq; ULONG maxEntities; ULONG entityCount; ULONG version; };

static const int MAX_LOGS     = 256;
static const int LOG_LINE_MAX = 128;
struct SharedLogEntry { ULONG tick; char text[LOG_LINE_MAX]; };
struct SharedLog { volatile LONG head; volatile LONG tail; SharedLogEntry entries[MAX_LOGS]; };

struct SharedBlock { SharedHeader hdr; SharedLog log; SharedEntity ents[256]; };
#pragma pack(pop)

static HANDLE        g_map     = nullptr;
static SharedBlock*  g_shm     = nullptr;
static HANDLE        g_viewMap = nullptr;
static float*        g_view    = nullptr;

// ===========================================================================
// logging: OutputDebugStringA + %TEMP%\cs2esp_overlay.log
// ===========================================================================
static FILE*            g_logFile       = nullptr;
static CRITICAL_SECTION g_logCs;
static volatile LONG    g_selfLogCount  = 0;
static volatile LONG    g_logFlushCount = 0;

static void LogOpen() {
    // Инициализируем CS ПЕРВЫМ делом: даже если ниже что-то упадёт,
    // EnterCriticalSection в LogSelf не будет на неинициализированной CS.
    InitializeCriticalSection(&g_logCs);

    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n) return;

    wchar_t pathW[MAX_PATH];
    _snwprintf_s(pathW, MAX_PATH, _TRUNCATE, L"%scs2esp_overlay.log", tmp);

    // MinGW-w64 не поддерживает ccs=UTF-8 в _wfopen_s → битый FILE* → AV.
    // Открываем обычным fopen_s по ANSI-пути.
    char pathA[MAX_PATH * 2];
    if (!WideCharToMultiByte(CP_UTF8, 0, pathW, -1, pathA, sizeof(pathA), nullptr, nullptr)) return;
    fopen_s(&g_logFile, pathA, "a");
}

static void LogFlushLocked() {
    if (g_logFile) fflush(g_logFile);
}

static void LogSelf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st; GetLocalTime(&st);
    char line[640];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[%02u:%02u:%02u.%03u][self] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);

    OutputDebugStringA(line);

    EnterCriticalSection(&g_logCs);
    if (g_logFile) {
        fputs(line, g_logFile);
        // fflush реже: раз в 16 записей, чтобы не дёргать диск на каждой строке
        if (InterlockedIncrement(&g_logFlushCount) >= 16) {
            InterlockedExchange(&g_logFlushCount, 0);
            LogFlushLocked();
        }
    }
    LeaveCriticalSection(&g_logCs);

    InterlockedIncrement(&g_selfLogCount);
}

static void LogKernelLine(const SharedLogEntry& le) {
    if (!le.text[0]) return;
    char line[640];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[kernel #%u] %s\n", le.tick, le.text);
    OutputDebugStringA(line);
    EnterCriticalSection(&g_logCs);
    if (g_logFile) {
        fputs(line, g_logFile);
        if (InterlockedIncrement(&g_logFlushCount) >= 16) {
            InterlockedExchange(&g_logFlushCount, 0);
            LogFlushLocked();
        }
    }
    LeaveCriticalSection(&g_logCs);
}

// читаем новые строки из кольцевого буфера ядра (head-tail)
static LONG g_lastKernelTail = 0;
static void DrainKernelLogs() {
    if (!g_shm) return;
    LONG tail = g_shm->log.tail;
    if (tail == g_lastKernelTail) return;
    if (tail < g_lastKernelTail) g_lastKernelTail = 0;
    LONG start = g_lastKernelTail;
    LONG count = tail - start;
    if (count > MAX_LOGS) { start = tail - MAX_LOGS; count = MAX_LOGS; }
    for (LONG i = 0; i < count; ++i) {
        LONG idx = (start + i) % MAX_LOGS;
        if (idx < 0) idx += MAX_LOGS;
        LogKernelLine(g_shm->log.entries[idx]);
    }
    g_lastKernelTail = tail;
}

// ===========================================================================
// proxy version.dll exports (signatures MUST match winver.h)
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
    if (!h) {
        LogSelf("LoadReal: LoadLibraryW failed, err=%lu", GetLastError());
        return;
    }
    pGVIA  = (fnGVIA) GetProcAddress(h, "GetFileVersionInfoA");
    pGVIW  = (fnGVIW) GetProcAddress(h, "GetFileVersionInfoW");
    pGVISA = (fnGVISA)GetProcAddress(h, "GetFileVersionInfoSizeA");
    pGVISW = (fnGVISW)GetProcAddress(h, "GetFileVersionInfoSizeW");
    pVQVA  = (fnVQVA) GetProcAddress(h, "VerQueryValueA");
    pVQVW  = (fnVQVW) GetProcAddress(h, "VerQueryValueW");
    LogSelf("LoadReal: real version.dll @ %p (procs: %p %p %p %p %p %p)",
            h, pGVIA, pGVIW, pGVISA, pGVISW, pVQVA, pVQVW);
}

// ===========================================================================
// overlay
// ===========================================================================
static HWND g_hWnd = nullptr;
static int  g_W = 0, g_H = 0;
static ID3D11Device*           g_dev = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain*         g_sc  = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static volatile bool g_run = true;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    if (m == WM_DESTROY) { PostQuitMessage(0); g_run = false; return 0; }
    return DefWindowProcW(h, m, w, l);
}

static bool CreateDev(HWND h) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = h;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    D3D_FEATURE_LEVEL fl;
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fls, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) {
        LogSelf("D3D11CreateDeviceAndSwapChain failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
    LogSelf("CreateDev ok: dev=%p ctx=%p sc=%p rtv=%p", g_dev, g_ctx, g_sc, g_rtv);
    return true;
}

static void CleanDev() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_sc)  { g_sc->Release();  g_sc = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static void W2S(const float* m, float x, float y, float z, int w, int h, float& sx, float& sy) {
    float sw = m[12]*x + m[13]*y + m[14]*z + m[15];
    if (sw <= 0.001f) { sx = -9999.f; sy = -9999.f; return; }
    float cx = w * 0.5f, cy = h * 0.5f;
    sx = cx + std::floor(cx * (m[0]*x + m[1]*y + m[2]*z + m[3]) / sw);
    sy = cy - std::floor(cy * (m[4]*x + m[5]*y + m[6]*z + m[7]) / sw);
}

static const int CONN[][2] = {
    {1,3},{3,4},{4,23},{23,6},{6,7},{6,9},{9,10},{10,11},
    {6,13},{13,14},{14,15},{1,17},{17,18},{18,19},{1,20},{20,21},{21,22}
};

// ---- debug HUD: статус + логи ядра ----
static void DrawStatus(ImDrawList* d) {
    char line[256];
    LONG seq  = g_shm ? g_shm->hdr.seq : -1;
    ULONG n   = g_shm ? g_shm->hdr.entityCount : 0;
    ULONG ver = g_shm ? g_shm->hdr.version : 0;
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "shm=%s view=%s | seq=%ld entities=%lu ver=%lu | W=%d H=%d",
        g_shm ? "OK" : "NULL",
        g_view ? "OK" : "NULL",
        seq, n, ver, g_W, g_H);
    d->AddText({10.f, 8.f}, IM_COL32(0,255,0,255), line);
}

static void DrawKernelLogs(ImDrawList* d) {
    if (!g_shm) return;
    LONG tail = g_shm->log.tail;
    float y = 28.f;
    // рисуем 8 последних записей (сверху — самая свежая)
    for (int i = 1; i <= 8; ++i) {
        LONG idx = (tail - i) % MAX_LOGS;
        if (idx < 0) idx += MAX_LOGS;
        const SharedLogEntry& le = g_shm->log.entries[idx];
        if (!le.text[0]) continue;
        char buf[LOG_LINE_MAX + 32];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "#%u %s", le.tick, le.text);
        d->AddText({10.f, y}, IM_COL32(0,200,0,220), buf);
        y += 15.f;
    }
}

static void Render(ImDrawList* d) {
    DrawStatus(d);
    DrawKernelLogs(d);

    if (!g_shm || !g_view) return;

    LONG s1 = g_shm->hdr.seq;
    if (s1 & 1) return;
    ULONG n = g_shm->hdr.entityCount;
    if (n > 256) n = 256;
    static SharedEntity snap[256];
    for (ULONG k = 0; k < n; ++k) snap[k] = g_shm->ents[k];
    LONG s2 = g_shm->hdr.seq;
    if (s1 != s2) return;

    float vm[16]; for (int k = 0; k < 16; ++k) vm[k] = g_view[k];

    const ImU32 red = IM_COL32(255,0,0,255), white = IM_COL32(255,255,255,255);
    for (ULONG e = 0; e < n; ++e) {
        const SharedEntity& en = snap[e];
        if (!en.valid || en.hp <= 0) continue;
        float sx[24], sy[24]; bool ok[24];
        for (int b = 0; b < 24; ++b) {
            ok[b] = false;
            if (en.bones[b].x == 0 && en.bones[b].y == 0 && en.bones[b].z == 0) continue;
            W2S(vm, en.bones[b].x, en.bones[b].y, en.bones[b].z, g_W, g_H, sx[b], sy[b]);
            ok[b] = (sx[b] > -9000.f);
        }
        for (auto& c : CONN)
            if (ok[c[0]] && ok[c[1]])
                d->AddLine({sx[c[0]], sy[c[0]]}, {sx[c[1]], sy[c[1]]}, red, 2.0f);
        if (ok[7]) {
            float r = 4.0f;
            if (ok[6]) {
                float dx = sx[7]-sx[6], dy = sy[7]-sy[6];
                float dd = std::sqrt(dx*dx+dy*dy);
                r = dd < 2.f ? 2.f : (dd > 15.f ? 15.f : dd);
            }
            d->AddCircle({sx[7], sy[7]}, r, white, 16, 1.5f);
            char buf[32]; wsprintfA(buf, "HP: %d", en.hp);
            d->AddText({sx[7]-15.f, sy[7]-r-15.f}, red, buf);
        }
    }
}

static DWORD WINAPI DiagThread(LPVOID) {
    unsigned diagCounter = 0;
    for (;;) {
        // Если драйвер загрузился ПОЗЖЕ version.dll — переоткрываем секции
        if (!g_shm) {
            HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\Cs2EspShared_v1");
            if (m) {
                g_shm = (SharedBlock*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(SharedBlock));
                if (g_shm) { g_map = m; LogSelf("DiagThread: late OpenFileMapping(Shared) ok shm=%p", g_shm); }
                else CloseHandle(m);
            }
        }
        if (!g_view) {
            HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\Cs2EspView_v1");
            if (m) {
                g_view = (float*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0x1000);
                if (g_view) { g_viewMap = m; LogSelf("DiagThread: late OpenFileMapping(View) ok view=%p", g_view); }
                else CloseHandle(m);
            }
        }

        DrainKernelLogs();
        if (++diagCounter >= 5) {
            diagCounter = 0;
            if (g_shm) {
                LogSelf("diag: seq=%ld entities=%lu ver=%lu logTail=%ld",
                        g_shm->hdr.seq, g_shm->hdr.entityCount, g_shm->hdr.version,
                        g_shm->log.tail);
            } else {
                LogSelf("diag: g_shm=NULL (OpenFileMappingW failed?)");
            }
        }
        Sleep(1000);
    }
    return 0;
}

static DWORD WINAPI OverlayThread(LPVOID) {
    LogSelf("OverlayThread: started, sleeping 3s to let Discord init");
    Sleep(3000);

    g_W = GetSystemMetrics(SM_CXSCREEN);
    g_H = GetSystemMetrics(SM_CYSCREEN);
    LogSelf("OverlayThread: screen=%dx%d", g_W, g_H);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr),
                       LoadIcon(nullptr, IDI_APPLICATION), LoadCursor(nullptr, IDC_ARROW),
                       nullptr, nullptr, L"DiscordOvClass", nullptr };
    RegisterClassExW(&wc);
    g_hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_APPWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"Discord Overlay", WS_POPUP, 0, 0, g_W, g_H,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hWnd) {
        LogSelf("OverlayThread: CreateWindowExW failed err=%lu", GetLastError());
        return 0;
    }
    LogSelf("OverlayThread: hwnd=%p", g_hWnd);

    MARGINS m = { -1 };
    DwmExtendFrameIntoClientArea(g_hWnd, &m);
    SetLayeredWindowAttributes(g_hWnd, 0, 255, LWA_ALPHA);
    SetWindowDisplayAffinity(g_hWnd, 0x11);

    if (!CreateDev(g_hWnd)) {
        LogSelf("OverlayThread: CreateDev failed, aborting");
        CleanDev(); DestroyWindow(g_hWnd); return 0;
    }
    ShowWindow(g_hWnd, SW_SHOW);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    LogSelf("OverlayThread: ImGui initialized (win32 + dx11)");

    MSG msg = {};
    unsigned long long frame = 0;
    while (g_run && msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg); continue;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({(float)g_W, (float)g_H});
        ImGui::SetNextWindowPos({0, 0});
        ImGui::Begin("ov", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
        Render(ImGui::GetWindowDrawList());
        ImGui::End();
        ImGui::Render();
        const float clr[4] = {0,0,0,0};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        HRESULT hr = g_sc->Present(1, 0);
        if (frame < 3) {
            LogSelf("OverlayThread: frame=%llu Present hr=0x%08lX", frame, (unsigned long)hr);
        }
        if (frame == 60) {
            LogSelf("OverlayThread: 60 frames rendered, shm=%p view=%p",
                    g_shm, g_view);
        }
        ++frame;
    }
    LogSelf("OverlayThread: exiting loop, frames=%llu", frame);

    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanDev(); DestroyWindow(g_hWnd); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ===========================================================================
// InitThread — всё, что нельзя делать в DllMain (loader lock)
// ===========================================================================
static DWORD WINAPI InitThread(LPVOID) {
    LogOpen();
    LogSelf("InitThread: started pid=%lu", GetCurrentProcessId());

    LoadReal();

    g_map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\Cs2EspShared_v1");
    if (g_map) {
        g_shm = (SharedBlock*)MapViewOfFile(g_map, FILE_MAP_READ, 0, 0, sizeof(SharedBlock));
        LogSelf("InitThread: OpenFileMapping(Shared) ok map=%p shm=%p size=%zu",
                g_map, g_shm, sizeof(SharedBlock));
    } else {
        LogSelf("InitThread: OpenFileMapping(Shared) FAILED err=%lu — драйвер не загружен или имя не совпадает",
                GetLastError());
    }

    g_viewMap = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\Cs2EspView_v1");
    if (g_viewMap) {
        g_view = (float*)MapViewOfFile(g_viewMap, FILE_MAP_READ, 0, 0, 0x1000);
        LogSelf("InitThread: OpenFileMapping(View) ok map=%p view=%p",
                g_viewMap, g_view);
    } else {
        LogSelf("InitThread: OpenFileMapping(View) FAILED err=%lu", GetLastError());
    }

    CreateThread(nullptr, 0, DiagThread, nullptr, 0, nullptr);
    OverlayThread(nullptr);
    return 0;
}

// ===========================================================================
// DllMain — только CreateThread, никаких LoadLibrary/OpenFileMapping/CRT
// ===========================================================================
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
