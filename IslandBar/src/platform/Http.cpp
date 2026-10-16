// platform/Http.cpp -- see Http.h for why this is one function rather than two.

#include "platform/Http.h"

#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace island {

bool HttpGet(const wchar_t* host, const wchar_t* path, const wchar_t* referer, int timeoutMs, char* out,
             size_t capacity, size_t* outLength, DWORD* outStatus) {
    if (outLength) *outLength = 0;
    if (outStatus) *outStatus = 0;
    if (!host || !path || !out || capacity == 0) return false;
    out[0] = '\0';

    // The services reject the default WinHTTP agent, so a browser-shaped one is sent.
    HINTERNET session = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        L"Chrome/122.0.0.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return false;
    }

    // WINHTTP_NO_REFERER is null, so a null `referer` argument means what it says.
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, nullptr,
                                           referer ? referer : WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    bool ok = false;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        if (outStatus) *outStatus = status;

        if (status == 200) {
            size_t total = 0;
            DWORD available = 0;
            ok = true;
            do {
                available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    ok = false;
                    break;
                }
                if (available == 0) break;

                // Never read more than the caller's buffer holds, and always leave a terminator: a huge or
                // endless response must not overrun it.
                DWORD chunk = available;
                if (total + chunk >= capacity) chunk = static_cast<DWORD>(capacity - total - 1);
                if (chunk == 0) break;

                DWORD read = 0;
                if (!WinHttpReadData(request, out + total, chunk, &read)) {
                    ok = false;
                    break;
                }
                if (read == 0) break;
                total += read;
            } while (available > 0);

            out[total] = '\0';
            if (outLength) *outLength = total;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

}  // namespace island
