// =====================================================================
//  NotificationPipe.cpp
// =====================================================================
#include "NotificationPipe.h"

#include <aclapi.h>    // SetEntriesInAcl / EXPLICIT_ACCESS for the pipe DACL
#include <sddl.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "advapi32.lib")

#include "MiniJson.h"

namespace island {
namespace {

// Security descriptor for the pipe.
//
// CreateNamedPipe's default DACL is created from the process token and, in
// practice, refuses same-user clients with ERROR_ACCESS_DENIED -- which is exactly
// what happened: the sender got win32 5 while the listener sat waiting.
//
// The descriptor grants full access to Everyone, and that is a deliberate,
// bounded decision:
//   * a named pipe is reachable only from this machine, never over the network;
//   * the only thing an attacker gains by writing is the ability to show a
//     notification -- the same thing the user can already do;
//   * notifications are parsed defensively (see MiniJson), so a hostile message
//     cannot corrupt the bar.
// The alternative -- a DACL naming only the current user -- would break the
// common case of sending from an elevated shell while the bar runs unelevated.
//
// Built once and freed by the destructor.
class PipeSecurity {
public:
    PipeSecurity() {
        // SECURITY_WORLD_SID_AUTHORITY with SECURITY_WORLD_RID is Everyone.
        SID_IDENTIFIER_AUTHORITY worldAuthority = SECURITY_WORLD_SID_AUTHORITY;
        if (!AllocateAndInitializeSid(&worldAuthority, 1, SECURITY_WORLD_RID,
                                      0, 0, 0, 0, 0, 0, 0, &everyone_)) {
            everyone_ = nullptr;
            return;
        }

        EXPLICIT_ACCESSW access = {};
        access.grfAccessPermissions = GENERIC_ALL;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone_);

        if (SetEntriesInAclW(1, &access, nullptr, &acl_) != ERROR_SUCCESS) {
            acl_ = nullptr;
            return;
        }

        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE);
        ready_ = true;
    }

    ~PipeSecurity() {
        if (acl_) LocalFree(acl_);
        if (everyone_) FreeSid(everyone_);
    }

    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    // Null when the descriptor could not be built, which makes CreateNamedPipe fall
    // back to its default rather than failing outright.
    SECURITY_ATTRIBUTES* Attributes() { return ready_ ? &attributes_ : nullptr; }

private:
    PSID everyone_ = nullptr;
    PACL acl_ = nullptr;
    SECURITY_DESCRIPTOR descriptor_ = {};
    SECURITY_ATTRIBUTES attributes_ = {};
    bool ready_ = false;
};

// Pipe buffer sizes. A notification is a few hundred bytes; 4 KB each way is
// generous and keeps the kernel allocation small.
constexpr DWORD kPipeBufferBytes = 4096;

bool ReadAll(HANDLE pipe, void* buffer, DWORD bytes) {
    auto* cursor = static_cast<unsigned char*>(buffer);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, remaining, &read, nullptr)) return false;
        if (read == 0) return false;   // peer closed mid-frame
        cursor += read;
        remaining -= read;
    }
    return true;
}

bool WriteAll(HANDLE pipe, const void* buffer, DWORD bytes) {
    const auto* cursor = static_cast<const unsigned char*>(buffer);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, remaining, &written, nullptr)) return false;
        if (written == 0) return false;
        cursor += written;
        remaining -= written;
    }
    return true;
}

} // namespace

bool WriteFrame(HANDLE pipe, const char* payload, size_t length) {
    if (!pipe || !payload) return false;
    if (length > kMaxMessageBytes) return false;

    const uint32_t header = static_cast<uint32_t>(length);
    if (!WriteAll(pipe, &header, sizeof(header))) return false;
    if (length == 0) return true;
    return WriteAll(pipe, payload, static_cast<DWORD>(length));
}

bool ReadFrame(HANDLE pipe, char* out, size_t capacity) {
    if (!pipe || !out || capacity == 0) return false;
    out[0] = '\0';

    uint32_t length = 0;
    if (!ReadAll(pipe, &length, sizeof(length))) return false;

    // The length prefix comes from another process, so it is a claim, not a fact.
    // Rejecting an over-large claim here is what stops a bogus header from turning
    // into a huge allocation.
    if (length == 0 || length > kMaxMessageBytes) return false;

    const size_t wanted = (length < capacity - 1) ? length : capacity - 1;
    if (!ReadAll(pipe, out, static_cast<DWORD>(wanted))) return false;
    out[wanted] = '\0';

    // Drain anything beyond what we kept, so the stream stays framed for the next
    // message instead of desynchronising.
    DWORD remaining = static_cast<DWORD>(length) - static_cast<DWORD>(wanted);
    char scratch[512];
    while (remaining > 0) {
        const DWORD chunk = (remaining < sizeof(scratch)) ? remaining
                                                          : static_cast<DWORD>(sizeof(scratch));
        if (!ReadAll(pipe, scratch, chunk)) return false;
        remaining -= chunk;
    }

    return true;
}

// --- server ----------------------------------------------------------

NotificationServer::~NotificationServer() {
    Stop();
}

bool NotificationServer::Start(const wchar_t* pipeName) {
    if (running_) return true;

    swprintf(name_, 256, L"\\\\.\\pipe\\%s", pipeName ? pipeName : kPipeName);

    InitializeCriticalSection(&lock_);
    lockReady_ = true;

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset
    if (!stopEvent_) return false;

    // Created on the listener thread rather than here, so a failure to create the
    // pipe surfaces as "not running" instead of as a thread that quietly does
    // nothing.
    DWORD threadId = 0;
    thread_ = CreateThread(nullptr, 0, &NotificationServer::ThreadEntry, this, 0, &threadId);
    if (!thread_) return false;

    // Give the thread a moment to create the pipe, so Start() returning true means
    // a client can actually connect.
    for (int i = 0; i < 100 && !running_; ++i) {
        Sleep(5);
    }
    return running_;
}

void NotificationServer::Stop() {
    if (stopEvent_) SetEvent(stopEvent_);

    if (thread_) {
        WaitForSingleObject(thread_, 3000);

        // If the thread refused to end -- a blocked ConnectNamedPipe on a broken
        // client can do that -- the handle must still be closed and the thread
        // detached rather than waited on forever.
        CloseHandle(thread_);
        thread_ = nullptr;
    }

    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    if (lockReady_) {
        DeleteCriticalSection(&lock_);
        lockReady_ = false;
    }

    running_ = false;
}

unsigned long __stdcall NotificationServer::ThreadEntry(void* self) {
    static_cast<NotificationServer*>(self)->Run();
    return 0;
}

void NotificationServer::Run() {
    // One security descriptor for every instance the loop creates.
    PipeSecurity security;

    // The loop owns one pipe instance at a time: create, wait for a client, serve
    // it, and close. That serialises clients without any extra locking, and a
    // notification is a small message that is written and forgotten.
    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = CreateNamedPipeW(
            name_,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,                       // one instance: clients are serialised
            kPipeBufferBytes, kPipeBufferBytes,
            0, security.Attributes());

        if (pipe == INVALID_HANDLE_VALUE) {
            // Nothing useful to do but back off; a persistent failure here would
            // otherwise spin the CPU.
            Sleep(200);
            continue;
        }

        running_ = true;

        // Overlapped-free waiting: ConnectNamedPipe on a blocking handle parks the
        // thread until a client arrives, and the stop event is checked on the next
        // iteration. The wait below bounds how long that can take.
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        const DWORD connectError = connected ? 0 : GetLastError();
        const bool clientPresent = connected || connectError == ERROR_PIPE_CONNECTED;

        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        if (!clientPresent) {
            CloseHandle(pipe);
            continue;
        }

        char request[kMaxMessageBytes] = {};
        if (ReadFrame(pipe, request, sizeof(request))) {
            Notification notification;
            if (NotificationFromJson(request, &notification)) {
                // WHO sent it, taken from the connection rather than from the message.
                //
                // The card shows this process's icon, which is what makes a notification from a tool look
                // like that tool -- and it cannot be asked for in the message, because a sender that has to
                // describe itself is a sender that will not. Asked of the pipe, it is free and it cannot lie.
                ULONG clientPid = 0;
                if (GetNamedPipeClientProcessId(pipe, &clientPid)) {
                    notification.senderPid = clientPid;
                }
                Enqueue(notification);
                InterlockedIncrement(&receivedCount_);

                const int written = std::snprintf(
                    request, sizeof(request),
                    "{\"ok\":1,\"received\":%lld}",
                    static_cast<long long>(receivedCount_));
                if (written > 0) {
                    WriteFrame(pipe, request, static_cast<size_t>(written));
                }
            } else {
                InterlockedIncrement(&rejectedCount_);
                static const char kRejection[] =
                    "{\"ok\":0,\"error\":\"no title or body\"}";
                WriteFrame(pipe, kRejection, sizeof(kRejection) - 1);
            }
        } else {
            InterlockedIncrement(&rejectedCount_);
        }

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    running_ = false;
}

void NotificationServer::Enqueue(const Notification& notification) {
    if (!lockReady_) return;
    EnterCriticalSection(&lock_);
    if (queueCount_ < static_cast<int>(sizeof(queue_) / sizeof(queue_[0]))) {
        queue_[queueCount_++] = notification;
    } else {
        // The UI thread has not kept up. Drop the oldest rather than the newest:
        // a burst should still show its most recent item.
        for (int i = 1; i < queueCount_; ++i) {
            queue_[i - 1] = queue_[i];
        }
        queue_[queueCount_ - 1] = notification;
    }
    LeaveCriticalSection(&lock_);
}

bool NotificationServer::Take(Notification* out) {
    if (!out || !lockReady_) return false;
    bool have = false;
    EnterCriticalSection(&lock_);
    if (queueCount_ > 0) {
        *out = queue_[0];
        for (int i = 1; i < queueCount_; ++i) {
            queue_[i - 1] = queue_[i];
        }
        --queueCount_;
        have = true;
    }
    LeaveCriticalSection(&lock_);
    return have;
}

// --- client ----------------------------------------------------------

bool SendNotification(const Notification& notification, const wchar_t* pipeName,
                      char* responseOut, size_t responseCapacity,
                      DWORD connectTimeoutMs) {
    wchar_t name[256] = {};
    swprintf(name, 256, L"\\\\.\\pipe\\%s", pipeName ? pipeName : kPipeName);

    // WaitNamedPipe rather than a single CreateFile attempt: the bar serves one
    // client at a time, so a sender arriving while another is being served should be
    // waited out rather than failing.
    //
    // The reason is reported rather than swallowed. "could not reach the bar" with
    // no cause is what turned a two-line fix into a long detour: the two likely
    // cases look identical from outside but need opposite responses --
    // ERROR_FILE_NOT_FOUND means nothing is listening, while ERROR_SEM_TIMEOUT
    // means a listener exists but is busy or stuck.
    if (!WaitNamedPipeW(name, connectTimeoutMs)) {
        const DWORD error = GetLastError();
        if (responseOut && responseCapacity) {
            const char* reason = (error == ERROR_FILE_NOT_FOUND)
                                     ? "no listener (is the bar running?)"
                                     : (error == ERROR_SEM_TIMEOUT
                                            ? "listener busy or not accepting"
                                            : "WaitNamedPipe failed");
            std::snprintf(responseOut, responseCapacity, "%s (win32 %lu)", reason,
                          static_cast<unsigned long>(error));
        }
        return false;
    }

    HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (responseOut && responseCapacity) {
            std::snprintf(responseOut, responseCapacity, "CreateFile failed (win32 %lu)",
                          static_cast<unsigned long>(error));
        }
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    char payload[kMaxMessageBytes] = {};
    NotificationToJson(notification, payload, sizeof(payload), false);

    const bool sent = WriteFrame(pipe, payload, std::strlen(payload));
    bool ok = false;

    if (sent) {
        char response[1024] = {};
        if (ReadFrame(pipe, response, sizeof(response))) {
            long long acknowledged = 0;
            ok = json::GetInt(response, "ok", &acknowledged) && acknowledged != 0;
            if (responseOut && responseCapacity) {
                strncpy_s(responseOut, responseCapacity, response, _TRUNCATE);
            }
        }
    }

    CloseHandle(pipe);
    return ok;
}

} // namespace island
