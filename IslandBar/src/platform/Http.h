// platform/Http.h -- one HTTPS GET, shared by everything that fetches from the network
//
// WHY THIS IS A COMPONENT. Two readers fetch over the network -- the lyric provider (NetEase and QQ
// Music) and the weather reader (Open-Meteo) -- and each had written the whole WinHTTP sequence out:
// WinHttpOpen, WinHTTP's four timeouts, connect, open request, send, receive, read the status, read the
// body in chunks without overrunning the caller's buffer, close three handles in the order they were
// opened. check-duplicate-code.ps1 (2026-10-16) reported them as the longest duplicated run in the tree;
// the two copies differed in exactly two things, which is what the arguments carry.
//
// IT LIVES IN platform/ BECAUSE BOTH CALLERS SIT ABOVE IT: the lyric provider is media/ and the weather
// reader is shell/. Anything higher would be an upward include, which the build refuses (see
// ARCHITECTURE.md section 3).
//
// TWO ARGUMENTS THAT LOOK LIKE DETAILS AND ARE NOT, both learned the hard way and kept in the comments at
// the call sites: the browser-shaped User-Agent (the services reject WinHTTP's default one) and the
// optional Referer (NetEase and QQ answer 403 or a login redirect without it). The status is returned
// rather than only the body, because "the service answered 403" and "the service answered nothing" have
// different fixes, and a caller that cannot tell them apart reports the wrong one.

#pragma once

#include <windows.h>

#include <cstddef>

namespace island {

// One GET over TLS into `out`, which is always null-terminated.
//
// `referer` may be null, in which case no Referer header is sent at all. `timeoutMs` is applied to all
// four WinHTTP timeouts (resolve, connect, send, receive) -- a fetch that hangs is worse than one that
// fails, because it holds a worker and the worker's stop event is only checked between requests.
//
// Returns true when the request completed AND the body was read; a false return with `outStatus` set to
// 200 means the body was truncated or the connection broke half-way through it. `outLength` and
// `outStatus` may be null.
bool HttpGet(const wchar_t* host, const wchar_t* path, const wchar_t* referer, int timeoutMs, char* out,
             size_t capacity, size_t* outLength, DWORD* outStatus);

}  // namespace island
