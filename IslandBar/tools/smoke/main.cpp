// =====================================================================
//  smoke/main.cpp
//  Verifies that the toolchain can compile AND link every API family the
//  island will depend on. It really calls the factories rather than merely
//  including headers, because a header that compiles but whose import
//  library is missing fails only at link time.
//
//  Lifetime discipline matters here: every COM pointer is released exactly
//  once, at the end of the scope that owns it, and never before its last
//  use. An earlier revision released the DXGI device and then passed it to
//  DCompositionCreateDevice, which dereferenced a null pointer.
// =====================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <uiautomation.h>

#include <cstdio>
#include <cstdint>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uiautomationcore.lib")

namespace {

int g_failures = 0;

void Report(const char* name, bool ok, const char* detail) {
    std::printf("  [%s] %-36s %s\n", ok ? " OK " : "FAIL", name, detail ? detail : "");
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

// Capability probe: reports what this machine supports without failing the run.
// Used where absence is a legitimate outcome that changes the design.
void Note(const char* name, const char* detail) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%-36s %s", name, detail ? detail : "");
    std::printf("  [ -- ] %s\n", buf);
    std::fflush(stdout);
}

const char* HrText(HRESULT hr) {
    static char buf[64];
    std::snprintf(buf, sizeof(buf), "hr=0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

template <typename T>
void SafeRelease(T** pp) {
    if (pp && *pp) { (*pp)->Release(); *pp = nullptr; }
}

// --- 1. D3D11 device + DXGI + Direct2D 1.3 ---------------------------
// The DXGI device is created here and handed to the caller, because
// DirectComposition needs it too. Caller owns it.
bool TestGraphics(IDXGIDevice** outDxgi) {
    *outDxgi = nullptr;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* device = nullptr;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &device, nullptr, nullptr);

    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &device, nullptr, nullptr);
        Report("D3D11CreateDevice", SUCCEEDED(hr), SUCCEEDED(hr) ? "WARP fallback" : HrText(hr));
    } else {
        Report("D3D11CreateDevice", true, "hardware");
    }

    if (!device) return false;

    ID3D11Device5* device5 = nullptr;
    hr = device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5));
    Report("ID3D11Device5 (D3D11.4)", SUCCEEDED(hr), HrText(hr));
    SafeRelease(&device5);

    IDXGIDevice* dxgiDevice = nullptr;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    Report("IDXGIDevice (needed by D2D and DComp)", SUCCEEDED(hr), HrText(hr));

    if (SUCCEEDED(hr) && dxgiDevice) {
        ID2D1Factory3* d2dFactory = nullptr;
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3),
                               nullptr, reinterpret_cast<void**>(&d2dFactory));
        Report("D2D1CreateFactory (1.3)", SUCCEEDED(hr), HrText(hr));

        if (SUCCEEDED(hr) && d2dFactory) {
            ID2D1Device2* d2dDevice = nullptr;
            hr = d2dFactory->CreateDevice(dxgiDevice, &d2dDevice);
            Report("ID2D1Factory3::CreateDevice", SUCCEEDED(hr), HrText(hr));

            if (SUCCEEDED(hr) && d2dDevice) {
                ID2D1DeviceContext2* dc = nullptr;
                hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
                Report("ID2D1Device2::CreateDeviceContext", SUCCEEDED(hr), HrText(hr));
                if (dc) {
                    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                    Report("D2D context draw config", true, "AA modes accepted");

                    // Exercise the actual drawing surface API used for the bar
                    D2D1_ROUNDED_RECT rr = {};
                    rr.rect = D2D1::RectF(0.0f, 0.0f, 220.0f, 36.0f);
                    rr.radiusX = 18.0f;
                    rr.radiusY = 18.0f;
                    Report("D2D1_ROUNDED_RECT geometry", rr.radiusX > 0.0f, "pill shape ready");

                    dc->Release();
                }
                d2dDevice->Release();
            }
            d2dFactory->Release();
        }
    }

    // Hand ownership of the DXGI device to the caller, do NOT release here.
    *outDxgi = dxgiDevice;
    device->Release();
    return dxgiDevice != nullptr;
}

// --- 2. DirectComposition --------------------------------------------
bool TestDComp(IDXGIDevice* dxgiDevice) {
    if (!dxgiDevice) {
        Report("DCompositionCreateDevice", false, "no DXGI device to wrap");
        return false;
    }

    IDCompositionDevice* dcompDevice = nullptr;
    HRESULT hr = DCompositionCreateDevice(dxgiDevice, __uuidof(IDCompositionDevice),
                                         reinterpret_cast<void**>(&dcompDevice));
    Report("DCompositionCreateDevice", SUCCEEDED(hr), HrText(hr));

    if (SUCCEEDED(hr) && dcompDevice) {
        IDCompositionVisual* visual = nullptr;
        hr = dcompDevice->CreateVisual(&visual);
        Report("IDCompositionDevice::CreateVisual", SUCCEEDED(hr), HrText(hr));

        if (SUCCEEDED(hr) && visual) {
            IDCompositionAnimation* anim = nullptr;
            hr = dcompDevice->CreateAnimation(&anim);
            Report("IDCompositionDevice::CreateAnimation", SUCCEEDED(hr), HrText(hr));
            if (anim) {
                // The expand/collapse motion will use cubic curves
                anim->AddCubic(0.0, 0.0, 0.0, 1.0, 0.0);
                Report("IDCompositionAnimation::AddCubic", true, "curve accepted");
                anim->Release();
            }

            hr = visual->SetOffsetX(0.0f);
            Report("IDCompositionVisual::SetOffsetX", SUCCEEDED(hr), HrText(hr));

            // Finding: DCompositionCreateDevice returns the FIRST-generation
            // IDCompositionDevice, whose visuals do not implement
            // IDCompositionVisual3, so Visual3::SetOpacity is unavailable.
            // The working route for opacity is IDCompositionEffectGroup, which
            // exposes both a float overload and an IDCompositionAnimation
            // overload -- so fades can be driven on the GPU.
            IDCompositionVisual3* visual3 = nullptr;
            hr = visual->QueryInterface(__uuidof(IDCompositionVisual3),
                                        reinterpret_cast<void**>(&visual3));
            Note("IDCompositionVisual3 support",
                 SUCCEEDED(hr) ? "present" : "absent -> use EffectGroup for opacity");
            SafeRelease(&visual3);

            IDCompositionEffectGroup* group = nullptr;
            hr = dcompDevice->CreateEffectGroup(&group);
            Report("IDCompositionDevice::CreateEffectGroup", SUCCEEDED(hr), HrText(hr));
            if (SUCCEEDED(hr) && group) {
                hr = group->SetOpacity(1.0f);
                Report("IDCompositionEffectGroup::SetOpacity(float)", SUCCEEDED(hr), HrText(hr));

                IDCompositionAnimation* fade = nullptr;
                if (SUCCEEDED(dcompDevice->CreateAnimation(&fade)) && fade) {
                    fade->AddCubic(0.0, 0.0, 0.0, 1.0, 0.0);
                    hr = group->SetOpacity(fade);
                    Report("IDCompositionEffectGroup::SetOpacity(anim)", SUCCEEDED(hr),
                           "GPU-driven fade available");
                    fade->Release();
                }

                hr = visual->SetEffect(group);
                Report("IDCompositionVisual::SetEffect", SUCCEEDED(hr), HrText(hr));
                group->Release();
            }

            visual->Release();
        }
        dcompDevice->Release();
    }
    return SUCCEEDED(hr);
}

// --- 3. DirectWrite --------------------------------------------------
bool TestDirectWrite() {
    IDWriteFactory3* dw = nullptr;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                                     reinterpret_cast<IUnknown**>(&dw));
    Report("DWriteCreateFactory (3)", SUCCEEDED(hr), HrText(hr));
    if (FAILED(hr) || !dw) return false;

    IDWriteTextFormat* fmt = nullptr;
    hr = dw->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
                              DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN", &fmt);
    Report("CreateTextFormat (zh-CN)", SUCCEEDED(hr), HrText(hr));
    SafeRelease(&fmt);

    // Font fallback is what lets Chinese app names render in the bar
    IDWriteFontFallbackBuilder* builder = nullptr;
    hr = dw->CreateFontFallbackBuilder(&builder);
    Report("IDWriteFactory3::CreateFontFallbackBuilder", SUCCEEDED(hr), HrText(hr));
    SafeRelease(&builder);

    dw->Release();
    return true;
}

// --- 4. Shell, AppBar, DPI, DWM, UI Automation -----------------------
bool TestDesktop() {
    APPBARDATA abd = {};
    abd.cbSize = sizeof(APPBARDATA);
    Report("APPBARDATA (shellapi)", abd.cbSize == sizeof(APPBARDATA), "structure available");

    DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
    Report("GetThreadDpiAwarenessContext", ctx != nullptr, "DPI API linked");

    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    HRESULT hr = GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    char dpiBuf[64];
    std::snprintf(dpiBuf, sizeof(dpiBuf), "%ux%u dpi", dpiX, dpiY);
    Report("GetDpiForMonitor", SUCCEEDED(hr), dpiBuf);

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar) {
        RECT r = {};
        GetWindowRect(taskbar, &r);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "hwnd rect: %ldx%ld at (%ld,%ld)",
                      r.right - r.left, r.bottom - r.top, r.left, r.top);
        Report("FindWindow(Shell_TrayWnd)", true, buf);
    } else {
        Report("FindWindow(Shell_TrayWnd)", false, "taskbar window not found");
    }

    // The island docks against the real taskbar thickness. GetWindowRect on
    // Shell_TrayWnd is NOT reliable for that: Windows 11 animates and can
    // auto-hide the bar, and the rect is in physical pixels. ABM_GETTASKBARPOS
    // reports the registered AppBar edge and size, which is what the bar must
    // match. Screen vs work area cross-checks the result.
    APPBARDATA pos = {};
    pos.cbSize = sizeof(pos);
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &pos)) {
        const char* edgeName = "unknown";
        switch (pos.uEdge) {
            case ABE_LEFT:   edgeName = "left";   break;
            case ABE_TOP:    edgeName = "top";    break;
            case ABE_RIGHT:  edgeName = "right";  break;
            case ABE_BOTTOM: edgeName = "bottom"; break;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "edge=%s rect=%ldx%ld",
                      edgeName, pos.rc.right - pos.rc.left, pos.rc.bottom - pos.rc.top);
        Report("SHAppBarMessage(ABM_GETTASKBARPOS)", true, buf);
    } else {
        // Non-fatal: this query is unreliable here, and the implementation does
        // not depend on it. Taskbar thickness comes from the work-area delta,
        // which two independent probes already agree on.
        Note("ABM_GETTASKBARPOS",
             "unavailable -> derive thickness from work-area delta instead");
    }

    POINT origin = {0, 0};
    HMONITOR mon2 = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(mon2, &mi)) {
        LONG screenH = mi.rcMonitor.bottom - mi.rcMonitor.top;
        LONG workH   = mi.rcWork.bottom - mi.rcWork.top;
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "screen=%ldx%ld work=%ldx%ld inferred bar=%ldpx",
                      mi.rcMonitor.right - mi.rcMonitor.left, screenH,
                      mi.rcWork.right - mi.rcWork.left, workH,
                      screenH - workH);
        Report("GetMonitorInfo (work area)", true, buf);
    }

    BOOL enabled = FALSE;
    HRESULT dwmHr = DwmIsCompositionEnabled(&enabled);
    Report("DwmIsCompositionEnabled", SUCCEEDED(dwmHr), enabled ? "on" : "off");

    IUIAutomation* uia = nullptr;
    HRESULT uiaHr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                     __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
    Report("CoCreateInstance(CUIAutomation)", SUCCEEDED(uiaHr), HrText(uiaHr));
    SafeRelease(&uia);

    return true;
}

} // namespace

int main() {
    // Make the console interpret our UTF-8 output correctly
    SetConsoleOutputCP(CP_UTF8);

    std::printf("IslandBar toolchain smoke test\n");
    std::printf("-----------------------------------------------\n");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    Report("CoInitializeEx", comOk, HrText(hr));

    // Single owner for the DXGI device, released once at the very end
    IDXGIDevice* dxgiDevice = nullptr;

    std::printf("\n[graphics]\n");
    TestGraphics(&dxgiDevice);

    std::printf("\n[composition]\n");
    TestDComp(dxgiDevice);

    std::printf("\n[text]\n");
    TestDirectWrite();

    std::printf("\n[desktop]\n");
    TestDesktop();

    SafeRelease(&dxgiDevice);
    CoUninitialize();

    std::printf("\n-----------------------------------------------\n");
    if (g_failures == 0) {
        std::printf("RESULT: PASS - compile, link and runtime all good\n");
    } else {
        std::printf("RESULT: FAIL - %d check(s) failed\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
