// =====================================================================
//  glassprobe -- can a PILL-SIZED window get a real blurred backdrop?
//
//      glassprobe                     acrylic, rounded, 300x40, 20 s
//      glassprobe --plain             the same window with NO material: the control
//      glassprobe --region            acrylic + a rounded window REGION instead of a corner preference
//      glassprobe --at 1180,120 --size 300x40 --seconds 30
//
//  WHY THIS QUESTION, ASKED AGAIN (2026-10-17). The bar's three blur attempts were all made on a
//  full-width, 360 DIP tall window, and the one that WORKED -- ACCENT_ENABLE_ACRYLICBLURBEHIND -- failed
//  only because it blurs the WHOLE WINDOW RECTANGLE and ignores the window region: on that window it would
//  have smeared a blurred band across the top of the screen. A window whose rectangle IS the pill has no
//  such band, so the only thing left to check is whether the blur stops at the ROUNDED CORNERS or spills
//  into four little square corners of blur.
//
//  That is the whole experiment, and it is why this program draws nothing: what is being looked at is the
//  material the system puts under it.
//
//  WHAT EACH OUTPUT MEANS
//    * every call's return value, because this project has been burned five times by a call that returned
//      success and did nothing;
//    * the window's own rect and whether it is visible, so the photograph can be tied to a position;
//    * nothing is judged here -- the judgement is a PHOTOGRAPH of the window over a busy wallpaper, which
//      is the only thing that can tell blur from no blur and a rounded corner from a square one.
//
//  ASCII-only on purpose. Do not add non-ASCII text to this file.
// =====================================================================

#include <windows.h>
#include <dwmapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

// The composition attribute setter is not declared in any SDK header; the structure is the one every
// acrylic sample in the world uses.
enum AccentState {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};

struct AccentPolicy {
    AccentState state;
    int flags;
    unsigned int colour;   // ABGR, and only the alpha matters to acrylic
    int animationId;
};

struct WindowCompositionAttributeData {
    int attribute;
    void* data;
    size_t size;
};

constexpr int kAccentPolicy = 19;
constexpr int kCornerPreference = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr int kCornerRound = 2;         // DWMWCP_ROUND

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

int g_seconds = 20;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    // NOTHING IS PAINTED, deliberately: with a NULL class brush and no case for WM_PAINT the window never
    // erases itself, so what shows through is exactly what the system put behind it. A window that painted
    // a colour here would be measuring the colour.
    //
    // AND THE BACKGROUND IS NOT ERASED. This is the one line the first version of this probe was missing,
    // and it cost it the experiment: a window that lets the default erase happen paints itself with the
    // frame's own light colour, and the photograph then shows the FRAME rather than the material -- measured
    // by the fact that the --plain control and the acrylic window came out pixel-for-pixel identical.
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int main(int argc, char** argv) {
    bool plain = false;
    bool useRegion = false;
    bool layered = false;
    unsigned int tint = 0x40FFFFFF;   // ABGR: a faint white, which is the least a material can be asked for
    int x = 1180, y = 140, width = 300, height = 40;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--plain") == 0) plain = true;
        else if (std::strcmp(argv[i], "--region") == 0) useRegion = true;
        else if (std::strcmp(argv[i], "--layered") == 0) layered = true;
        else if (std::strcmp(argv[i], "--dark") == 0) tint = 0x99000000;
        else if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%d,%d", &x, &y);
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%dx%d", &width, &height);
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            g_seconds = std::atoi(argv[++i]);
        } else {
            std::printf("glassprobe: unknown argument '%s'\n", argv[i]);
        }
    }

    const wchar_t* className = L"IslandBar.GlassProbe";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc)) {
        std::printf("glassprobe: RegisterClassW failed (error %lu)\n", GetLastError());
        return 2;
    }

    // A NORMAL WINDOW ON PURPOSE: no WS_EX_NOREDIRECTIONBITMAP, no DirectComposition. The bar's own window
    // is the other kind, and 8.91 measured that this material does nothing there.
    //
    // AND NO CAPTION: the first version had WS_CAPTION and an empty frame extension, which together make
    // DWM paint the whole window with the frame's own colour -- the material never got a chance to show.
    const DWORD style = WS_POPUP;
    // `--layered` IS THE WHOLE POINT OF THE SECOND VERSION OF THIS PROBE. The material is drawn BEHIND the
    // window's content, so a plain window hides it completely -- measured: with no material and with acrylic
    // the two photographs came out pixel-for-pixel identical, which is what sent this back to the drawing
    // board. The bar's own window is per-pixel transparent (DirectComposition), and 8.86 measured the blur
    // working THERE; a layered window at alpha 0 is the cheap way to ask the same question about a window
    // whose rectangle is the pill.
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    if (layered) exStyle |= WS_EX_LAYERED;
    HWND hwnd = CreateWindowExW(exStyle, className,
                                L"glassprobe", style, x, y, width, height, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        std::printf("glassprobe: CreateWindowExW failed (error %lu)\n", GetLastError());
        return 2;
    }

    const HRESULT corner =
        DwmSetWindowAttribute(hwnd, kCornerPreference, &kCornerRound, sizeof(kCornerRound));

    if (layered) {
        const BOOL ok = SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        std::printf("glassprobe: SetLayeredWindowAttributes(alpha 0) -> %d (error %lu)\n", ok ? 1 : 0,
                    ok ? 0UL : GetLastError());
    }

    HRESULT acrylic = E_NOTIMPL;
    const char* material = "none (--plain)";
    if (!plain) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto setAttribute = reinterpret_cast<SetWindowCompositionAttributeFn>(
            user32 ? reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowCompositionAttribute"))
                   : nullptr);
        if (setAttribute) {
            AccentPolicy policy = {};
            policy.state = ACCENT_ENABLE_ACRYLICBLURBEHIND;
            policy.colour = tint;
            WindowCompositionAttributeData data = {kAccentPolicy, &policy, sizeof(policy)};
            // The HRESULT is what the call returns as a BOOL; recorded rather than trusted.
            acrylic = setAttribute(hwnd, &data) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
            material = "ACRYLIC (ACCENT_ENABLE_ACRYLICBLURBEHIND)";
        } else {
            std::printf("glassprobe: SetWindowCompositionAttribute is not exported by user32\n");
        }
    }

    if (useRegion) {
        const int radius = height / 2;
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
        const int applied = SetWindowRgn(hwnd, region, TRUE);
        std::printf("glassprobe: SetWindowRgn -> %d (the region now owns the HRGN)\n", applied);
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    RECT rect = {};
    GetWindowRect(hwnd, &rect);
    std::printf("glassprobe: material=%s\n", material);
    std::printf("glassprobe: tint=0x%08lX\n", static_cast<unsigned long>(tint));
    std::printf("glassprobe: DWMWA_WINDOW_CORNER_PREFERENCE=ROUND -> 0x%08lX\n",
                static_cast<unsigned long>(corner));
    std::printf("glassprobe: acrylic -> 0x%08lX\n", static_cast<unsigned long>(acrylic));
    std::printf("glassprobe: window %ld,%ld %ldx%ld, visible=%d, on screen for %d s\n", rect.left,
                rect.top, rect.right - rect.left, rect.bottom - rect.top,
                IsWindowVisible(hwnd) ? 1 : 0, g_seconds);
    std::fflush(stdout);

    const DWORD until = GetTickCount() + static_cast<DWORD>(g_seconds) * 1000;
    while (GetTickCount() < until) {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(30);
    }
    DestroyWindow(hwnd);
    return 0;
}
