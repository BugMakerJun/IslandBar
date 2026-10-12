// =====================================================================
//  NotificationPipe.h
//  The local push channel: a named pipe the bar listens on.
//
//  Protocol, in full:
//
//      request  := uint32 length (little endian) | `length` bytes of UTF-8 JSON
//      response := same framing, a UTF-8 JSON object
//
//  A length prefix rather than newline framing, because a message body may
//  legitimately contain newlines and splitting on them would truncate it.
//
//  Why a named pipe and not a socket: it is the Windows-native mechanism, it needs
//  no firewall exception, and its default security descriptor already restricts it
//  to the local machine. The bar is a desktop shell component; it should not be
//  listening on a network port unless someone asks for that.
// =====================================================================
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "Notification.h"

namespace island {

// The default pipe name, without the \\.\pipe\ prefix.
inline constexpr const wchar_t* kPipeName = L"IslandBar.Notify";

// Largest request accepted, in bytes. A pipe is writable by any local process, so
// the length prefix must not be trusted to size an allocation: a bogus 4 GB claim
// would otherwise be an out-of-memory denial of service.
inline constexpr uint32_t kMaxMessageBytes = 64 * 1024;

// The server side. One thread accepts connections and reads framed messages.
//
// Threading note: the caller's UI thread must never block on a client, so the
// listener runs on its own thread and hands finished notifications over through a
// small lock-protected queue. That is the only synchronisation in the class.
class NotificationServer {
public:
    NotificationServer() = default;
    ~NotificationServer();

    NotificationServer(const NotificationServer&) = delete;
    NotificationServer& operator=(const NotificationServer&) = delete;

    // Creates the pipe and starts the listener thread.
    // `pipeName` defaults to kPipeName; tests pass a unique name so they cannot
    // collide with a running bar.
    bool Start(const wchar_t* pipeName = kPipeName);

    void Stop();

    bool IsRunning() const { return running_; }

    // Pops one received notification into `out`. Returns false when none are
    // waiting. Non-blocking: call it from the frame loop.
    bool Take(Notification* out);

    // Number of notifications received since Start, for reporting.
    long long ReceivedCount() const { return static_cast<long long>(receivedCount_); }

    // Number of messages rejected as malformed. Kept separate from received so a
    // misbehaving client is visible rather than silently ignored.
    long long RejectedCount() const { return static_cast<long long>(rejectedCount_); }

private:
    static unsigned long __stdcall ThreadEntry(void* self);
    void Run();
    void Enqueue(const Notification& notification);

    HANDLE        stopEvent_ = nullptr;
    HANDLE        thread_ = nullptr;
    wchar_t       name_[256] = {};   // full \\.\pipe\... path
    volatile bool running_ = false;

    // Received notifications awaiting pickup by the UI thread.
    CRITICAL_SECTION lock_{};
    bool             lockReady_ = false;
    Notification     queue_[64] = {};
    int              queueCount_ = 0;

    volatile long receivedCount_ = 0;
    volatile long rejectedCount_ = 0;
};

// --- client ----------------------------------------------------------

// Sends one notification to a running bar. Returns true when the bar acknowledged
// it with an "ok" response.
//
// Fails immediately when no bar is listening, rather than waiting: this is called
// from a short-lived --notify invocation, and hanging would look like a freeze.
bool SendNotification(const Notification& notification,
                      const wchar_t* pipeName = kPipeName,
                      char* responseOut = nullptr,
                      size_t responseCapacity = 0,
                      DWORD connectTimeoutMs = 1500);

// Reads the framed response from a connected pipe. Exposed for tests.
bool ReadFrame(HANDLE pipe, char* out, size_t capacity);

// Writes a length-prefixed frame. Exposed for tests.
bool WriteFrame(HANDLE pipe, const char* payload, size_t length);

} // namespace island
