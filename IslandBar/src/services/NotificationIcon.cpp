// =====================================================================
//  NotificationIcon.cpp
// =====================================================================
#include "services/NotificationIcon.h"

#include <windows.h>

#include <cwchar>
#include <cstdio>

#include "core/Log.h"
#include "media/MusicSession.h"   // FetchAppIconForTest: the shell's icon for a file, as a small PNG
#include "media/WicImage.h"       // DecodeImage: that PNG, or a sender's own, into pixels

namespace island {

bool IsPlainIconFilePath(const wchar_t* source) {
    if (!source || source[0] == L'\0') return false;
    const size_t length = std::wcslen(source);
    if (length >= MAX_PATH) return false;

    // Absolute, and only in the two ways Windows spells absolute: `X:\...` or `\\server\share`.
    const bool drive = length > 3 && source[1] == L':' && source[2] == L'\\' &&
                       ((source[0] >= L'A' && source[0] <= L'Z') ||
                        (source[0] >= L'a' && source[0] <= L'z'));
    const bool unc = length > 3 && source[0] == L'\\' && source[1] == L'\\';
    if (!drive && !unc) return false;

    // Characters a path cannot hold, and the ones a URI or a shell reference is full of. The colon is
    // checked by position above, so a second one anywhere is a refusal.
    for (size_t i = drive ? 2 : 2; i < length; ++i) {
        switch (source[i]) {
            case L':': case L'?': case L'*': case L'<': case L'>': case L'|': case L'"':
                return false;
            default:
                break;
        }
    }

    // A short list, so the shell is only ever asked about files whose icon extraction cannot act on
    // anything. The image types never reach the shell at all (WIC reads them), but they are listed so the
    // rule reads as one rule.
    static const wchar_t* const kSafe[] = {L".png", L".jpg", L".jpeg", L".bmp", L".gif",
                                           L".ico", L".tif", L".tiff", L".exe", L".dll"};
    for (const wchar_t* extension : kSafe) {
        const size_t extensionLength = std::wcslen(extension);
        if (length > extensionLength &&
            _wcsicmp(source + length - extensionLength, extension) == 0) {
            return true;
        }
    }
    return false;
}

bool NotificationIconSource(const Notification& notification, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = L'\0';

    if (notification.iconPath[0] != '\0') {
        return MultiByteToWideChar(CP_UTF8, 0, notification.iconPath, -1, out,
                                   static_cast<int>(capacity)) > 0;
    }
    if (notification.senderPid == 0) return false;

    // The sender's own executable. PROCESS_QUERY_LIMITED_INFORMATION rather than PROCESS_QUERY_INFORMATION:
    // the second one is refused for a process of a higher integrity level, and the common case here is a
    // tool running elevated and telling the bar about itself.
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, notification.senderPid);
    if (!process) return false;
    DWORD length = static_cast<DWORD>(capacity);
    const bool ok = QueryFullProcessImageNameW(process, 0, out, &length) != 0;
    CloseHandle(process);
    if (!ok) out[0] = L'\0';
    return ok;
}

bool LoadNotificationIconPixels(const wchar_t* source, std::vector<uint8_t>* pixels, int* width,
                                int* height) {
    if (!source || source[0] == L'\0' || !pixels || !width || !height) return false;

    // NOTHING reaches the shell unless it is a plain absolute file path (see IsPlainIconFilePath). The
    // refusal is logged once per source, because "no icon" and "refused an icon" need different fixes and
    // look identical on screen.
    if (!IsPlainIconFilePath(source)) {
        static wchar_t reported[256] = {};
        if (_wcsicmp(reported, source) != 0) {
            wcsncpy_s(reported, 256, source, _TRUNCATE);
            LogLine(L"  notification: icon source is not a file path, refused: '%ls'", source);
        }
        return false;
    }

    uint8_t encoded[16 * 1024] = {};
    size_t encodedSize = 0;

    const size_t length = std::wcslen(source);
    const bool isPng = length > 4 && _wcsicmp(source + length - 4, L".png") == 0;
    if (isPng) {
        FILE* file = nullptr;
        if (_wfopen_s(&file, source, L"rb") == 0 && file) {
            encodedSize = std::fread(encoded, 1, sizeof(encoded), file);
            std::fclose(file);
        }
    } else {
        encodedSize = FetchAppIconForTest(source, encoded, sizeof(encoded));
    }
    if (encodedSize == 0) return false;

    DecodedImage decoded;
    if (!DecodeImage(encoded, encodedSize, &decoded) || !decoded.pixels || decoded.width <= 0 ||
        decoded.height <= 0) {
        return false;
    }

    // COPIED, because the decoder's buffer belongs to the decoder and is overwritten by its next call -- while
    // these pixels are handed to frames that outlive this function.
    const size_t bytes = static_cast<size_t>(decoded.width) * static_cast<size_t>(decoded.height) * 4;
    pixels->assign(decoded.pixels, decoded.pixels + bytes);
    *width = decoded.width;
    *height = decoded.height;
    return true;
}

} // namespace island
