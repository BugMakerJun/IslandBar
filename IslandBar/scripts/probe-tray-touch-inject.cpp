// Temporary probe: does touch injection work on this machine at all? Deleted after the answer is read.
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t** argv) {
    const int x = argc > 1 ? _wtoi(argv[1]) : 1280;
    const int y = argc > 2 ? _wtoi(argv[2]) : 1300;

    SetLastError(0);
    const BOOL init = InitializeTouchInjection(10, TOUCH_FEEDBACK_DEFAULT);
    printf("InitializeTouchInjection=%d err=%lu\n", init, GetLastError());
    if (!init) return 1;

    POINTER_TOUCH_INFO contact = {};
    contact.pointerInfo.pointerType = PT_TOUCH;
    contact.pointerInfo.pointerId = 0;
    contact.pointerInfo.ptPixelLocation.x = x;
    contact.pointerInfo.ptPixelLocation.y = y;
    contact.pointerInfo.ptHimetricLocation.x = x;
    contact.pointerInfo.ptHimetricLocation.y = y;
    contact.touchFlags = TOUCH_FLAG_NONE;
    contact.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    contact.rcContact.left = x - 2;
    contact.rcContact.top = y - 2;
    contact.rcContact.right = x + 2;
    contact.rcContact.bottom = y + 2;
    contact.orientation = 90;
    contact.pressure = 32000;

    contact.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    SetLastError(0);
    const BOOL down = InjectTouchInput(1, &contact);
    printf("down=%d err=%lu\n", down, GetLastError());
    Sleep(40);
    contact.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    SetLastError(0);
    const BOOL move = InjectTouchInput(1, &contact);
    printf("update=%d err=%lu\n", move, GetLastError());
    Sleep(40);
    contact.pointerInfo.pointerFlags = POINTER_FLAG_UP;
    SetLastError(0);
    const BOOL up = InjectTouchInput(1, &contact);
    printf("up=%d err=%lu\n", up, GetLastError());
    printf("size of POINTER_TOUCH_INFO = %zu\n", sizeof(POINTER_TOUCH_INFO));
    return 0;
}
