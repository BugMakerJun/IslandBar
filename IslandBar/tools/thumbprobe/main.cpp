// =====================================================================
//  thumbprobe -- does DWM draw a live thumbnail into a window of ours?
//
//  WHY THIS EXISTS
//
//  The bar shows a live thumbnail of the window under the pointer. Every
//  answer it can see said yes and the picture said no: the destination
//  window existed, was visible at the right rectangle, DwmRegisterThumbnail
//  returned S_OK, DwmUpdateThumbnailProperties returned S_OK, the source was
//  an ordinary visible window -- and not one pixel of the thumbnail was
//  drawn. Filling the destination with a solid colour proved the window IS
//  composited (the colour appeared), so the fault is in the thumbnail path
//  itself.
//
//  Rather than keep changing the bar and squinting at screenshots, this asks
//  the question on its own: two windows of one ordinary console program, a
//  source that paints a recognisable picture, a destination with the
//  thumbnail in it, and every HRESULT printed. It is the smallest program
//  that can answer "is this the recipe or the machine".
//
//  WHAT IT PRINTS: the registration and update results, what the compositor
//  believes the source measures, and where the destination window is on
//  screen -- then it holds both on screen for a few seconds so a screenshot
//  can be taken.
// =====================================================================

#include <windows.h>
#include <dwmapi.h>

#include <cstdio>

#pragma comment(lib, "dwmapi.lib")

namespace {

constexpr wchar_t kSourceClass[] = L"ThumbProbeSource";
constexpr wchar_t kDestinationClass[] = L"ThumbProbeDestination";

// The source paints something unmistakable: green background, a red square, and a white bar whose
// position depends on a counter -- so a still frame can be told apart from a live one.
int g_phase = 0;

LRESULT CALLBACK SourceProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client = {};
            GetClientRect(hwnd, &client);
            HBRUSH green = CreateSolidBrush(RGB(0, 160, 0));
            FillRect(dc, &client, green);
            DeleteObject(green);
            HBRUSH red = CreateSolidBrush(RGB(220, 0, 0));
            RECT square = {20, 20, 120, 120};
            FillRect(dc, &square, red);
            DeleteObject(red);
            HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
            const int x = 20 + (g_phase % 20) * 10;
            RECT bar = {x, 150, x + 80, 190};
            FillRect(dc, &bar, white);
            DeleteObject(white);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// The destination paints NOTHING -- the standard recipe -- so that a thumbnail, if drawn, is the only
// thing in it.
LRESULT CALLBACK DestinationProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterClassNamed(const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = name;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

int main() {
    std::printf("--- DWM thumbnail probe ---\n");
    std::fflush(stdout);

    if (!RegisterClassNamed(kSourceClass, &SourceProc) ||
        !RegisterClassNamed(kDestinationClass, &DestinationProc)) {
        std::printf("FATAL: could not register a window class (error %lu)\n", GetLastError());
        return 1;
    }

    // The SOURCE: an ordinary top-level window with a recognisable picture in it.
    HWND source = CreateWindowExW(0, kSourceClass, L"thumbprobe source", WS_OVERLAPPEDWINDOW,
                                 100, 100, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr),
                                 nullptr);
    if (!source) {
        std::printf("FATAL: could not create the source window (error %lu)\n", GetLastError());
        return 1;
    }
    ShowWindow(source, SW_SHOWNOACTIVATE);
    UpdateWindow(source);

    // The DESTINATION: a plain popup, no background, no painting -- what the samples use.
    HWND destination = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                       kDestinationClass, L"thumbprobe destination", WS_POPUP,
                                       800, 200, 480, 360, nullptr, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
    if (!destination) {
        std::printf("FATAL: could not create the destination window (error %lu)\n", GetLastError());
        return 1;
    }
    ShowWindow(destination, SW_SHOWNOACTIVATE);
    SetWindowPos(destination, HWND_TOPMOST, 800, 200, 480, 360, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    RECT destinationRect = {};
    GetWindowRect(destination, &destinationRect);
    std::printf("destination window at %ld,%ld %ldx%ld (screen pixels)\n", destinationRect.left,
                destinationRect.top, destinationRect.right - destinationRect.left,
                destinationRect.bottom - destinationRect.top);

    HTHUMBNAIL thumbnail = nullptr;
    const HRESULT registered = DwmRegisterThumbnail(destination, source, &thumbnail);
    std::printf("DwmRegisterThumbnail: 0x%08lX, handle %s\n",
                static_cast<unsigned long>(registered), thumbnail ? "ok" : "NONE");

    if (thumbnail) {
        SIZE sourceSize = {0, 0};
        const HRESULT queried = DwmQueryThumbnailSourceSize(thumbnail, &sourceSize);
        std::printf("DwmQueryThumbnailSourceSize: 0x%08lX -> %ldx%ld (the source window is 640x480)\n",
                    static_cast<unsigned long>(queried), sourceSize.cx, sourceSize.cy);

        DWM_THUMBNAIL_PROPERTIES properties = {};
        properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY |
                             DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination = {0, 0, destinationRect.right - destinationRect.left,
                                    destinationRect.bottom - destinationRect.top};
        properties.rcSource = {0, 0, 0, 0};
        properties.opacity = 255;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly = FALSE;
        const HRESULT updated = DwmUpdateThumbnailProperties(thumbnail, &properties);
        std::printf("DwmUpdateThumbnailProperties: 0x%08lX\n",
                    static_cast<unsigned long>(updated));
    }

    // --- CONFIGURATION B: the destination as a CHILD window of an ordinary host ----------------
    //
    // The other recipe in circulation, and the one the samples use: a top-level host with a borderless
    // child inside it, the thumbnail registered into the CHILD. Worth testing because the popup above
    // fails here with every call reporting success, and a recipe that works is worth more than a theory
    // about why one does not.
    HWND host = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                kDestinationClass, L"thumbprobe host", WS_POPUP, 200, 800, 480, 360,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND child = nullptr;
    HTHUMBNAIL childThumbnail = nullptr;
    if (host) {
        child = CreateWindowExW(0, kDestinationClass, L"thumbprobe child", WS_CHILD | WS_VISIBLE, 0,
                                0, 480, 360, host, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(host, SW_SHOWNOACTIVATE);
        SetWindowPos(host, HWND_TOPMOST, 200, 800, 480, 360, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (child) {
            const HRESULT childRegistered = DwmRegisterThumbnail(child, source, &childThumbnail);
            std::printf("B: DwmRegisterThumbnail (child) 0x%08lX, handle %s\n",
                        static_cast<unsigned long>(childRegistered),
                        childThumbnail ? "ok" : "NONE");
            if (childThumbnail) {
                DWM_THUMBNAIL_PROPERTIES childProperties = {};
                childProperties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE |
                                          DWM_TNP_SOURCECLIENTAREAONLY;
                childProperties.rcDestination = {0, 0, 480, 360};
                childProperties.opacity = 255;
                childProperties.fVisible = TRUE;
                childProperties.fSourceClientAreaOnly = FALSE;
                const HRESULT childUpdated =
                    DwmUpdateThumbnailProperties(childThumbnail, &childProperties);
                std::printf("B: DwmUpdateThumbnailProperties 0x%08lX\n",
                            static_cast<unsigned long>(childUpdated));
            }
        }
    }

    // --- CONFIGURATION C: the same popup, with an EXPLICIT source rectangle ----------------------
    //
    // `rcSource = {0,0,0,0}` is documented as "the whole source window", and configuration A sends
    // exactly that and draws nothing. So the same thing is asked for the other way round: the rectangle
    // the compositor itself reported (DwmQueryThumbnailSourceSize), stated in full.
    HWND explicitDestination = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                              kDestinationClass, L"thumbprobe explicit", WS_POPUP,
                                              800, 600, 480, 360, nullptr, nullptr,
                                              GetModuleHandleW(nullptr), nullptr);
    HTHUMBNAIL explicitThumbnail = nullptr;
    if (explicitDestination) {
        ShowWindow(explicitDestination, SW_SHOWNOACTIVATE);
        SetWindowPos(explicitDestination, HWND_TOPMOST, 800, 600, 480, 360,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        const HRESULT explicitRegistered =
            DwmRegisterThumbnail(explicitDestination, source, &explicitThumbnail);
        std::printf("C: DwmRegisterThumbnail 0x%08lX, handle %s\n",
                    static_cast<unsigned long>(explicitRegistered),
                    explicitThumbnail ? "ok" : "NONE");
        if (explicitThumbnail) {
            SIZE size = {0, 0};
            DwmQueryThumbnailSourceSize(explicitThumbnail, &size);
            DWM_THUMBNAIL_PROPERTIES explicitProperties = {};
            explicitProperties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE |
                                         DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
            explicitProperties.rcDestination = {0, 0, 480, 360};
            explicitProperties.rcSource = {0, 0, size.cx, size.cy};
            explicitProperties.opacity = 255;
            explicitProperties.fVisible = TRUE;
            const HRESULT explicitUpdated =
                DwmUpdateThumbnailProperties(explicitThumbnail, &explicitProperties);
            std::printf("C: DwmUpdateThumbnailProperties 0x%08lX with rcSource 0,0 %ldx%ld\n",
                        static_cast<unsigned long>(explicitUpdated), size.cx, size.cy);
        }
    }

    // Cloaked, or not: a window DWM has cloaked is composited as if it were hidden, and that is the one
    // state that would explain "everything succeeds and nothing is drawn".
    {
        int sourceCloak = 0;
        int destinationCloak = 0;
        DwmGetWindowAttribute(source, DWMWA_CLOAKED, &sourceCloak, sizeof(sourceCloak));
        DwmGetWindowAttribute(destination, DWMWA_CLOAKED, &destinationCloak,
                              sizeof(destinationCloak));
        std::printf("cloaked: source=%d destination=%d (0 means not cloaked)\n", sourceCloak,
                    destinationCloak);
    }

    std::printf("A = popup 800,200; B = child (invalid); C = popup 800,600 with explicit source\n");
    std::fflush(stdout);

    // Ten seconds of message loop, with the source repainting twice a second so that a LIVE thumbnail is
    // distinguishable from a stale one: the white bar moves.
    const DWORD started = GetTickCount64();
    while (GetTickCount64() - started < 10000) {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        ++g_phase;
        InvalidateRect(source, nullptr, FALSE);
        UpdateWindow(source);
        Sleep(500);
    }

    if (childThumbnail) DwmUnregisterThumbnail(childThumbnail);
    if (explicitThumbnail) DwmUnregisterThumbnail(explicitThumbnail);
    if (thumbnail) DwmUnregisterThumbnail(thumbnail);
    if (explicitDestination) DestroyWindow(explicitDestination);
    if (host) DestroyWindow(host);
    DestroyWindow(destination);
    DestroyWindow(source);
    std::printf("done\n");
    return 0;
}
