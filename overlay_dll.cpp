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
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// ---- shared memory contract (must match driver) ----
#pragma pack(push, 1)
struct SharedBone { float x, y, z; };
struct SharedEntity { int valid; int hp; int team; int pad0; SharedBone bones[24]; };
struct SharedHeader { volatile LONG seq; ULONG maxEntities; ULONG entityCount; ULONG version; };
struct SharedBlock { SharedHeader hdr; SharedEntity ents[256]; };
#pragma pack(pop)

static HANDLE        g_map = nullptr;
static SharedBlock*  g_shm = nullptr;
static HANDLE        g_viewMap = nullptr;
static float*        g_view = nullptr;

// ---- proxy version.dll exports (signatures MUST match winver.h) ----
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

// ---- overlay ----
static HWND g_hWnd = nullptr;
static int  g_W, g_H;
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
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fls, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx))) return false;
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
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

static void Render(ImDrawList* d) {
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

static DWORD WINAPI OverlayThread(LPVOID) {
    Sleep(3000);
    g_W = GetSystemMetrics(SM_CXSCREEN);
    g_H = GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr),
                       LoadIcon(nullptr, IDI_APPLICATION), LoadCursor(nullptr, IDC_ARROW),
                       nullptr, nullptr, L"DiscordOvClass", nullptr };
    RegisterClassExW(&wc);
    g_hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_APPWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"Discord Overlay", WS_POPUP, 0, 0, g_W, g_H,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hWnd) return 0;
    MARGINS m = { -1 };
    DwmExtendFrameIntoClientArea(g_hWnd, &m);
    SetLayeredWindowAttributes(g_hWnd, 0, 255, LWA_ALPHA);
    SetWindowDisplayAffinity(g_hWnd, 0x11);

    if (!CreateDev(g_hWnd)) { CleanDev(); DestroyWindow(g_hWnd); return 0; }
    ShowWindow(g_hWnd, SW_SHOW);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    MSG msg = {};
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
        g_sc->Present(1, 0);
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanDev(); DestroyWindow(g_hWnd); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        LoadReal();
        g_map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\Cs2EspShared_v1");
        if (g_map) g_shm = (SharedBlock*)MapViewOfFile(g_map, FILE_MAP_READ, 0, 0, sizeof(SharedBlock));
        g_viewMap = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\Cs2EspView_v1");
        if (g_viewMap) g_view = (float*)MapViewOfFile(g_viewMap, FILE_MAP_READ, 0, 0, 64);
        CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    }
    return TRUE;
}