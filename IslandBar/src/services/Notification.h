// =====================================================================
//  Notification.h
//  What a notification is, and the bounded store that holds them.
//
//  Scope note: this is the LOCAL PUSH channel -- something on this machine
//  telling the bar about an event over a named pipe. Reading Windows' own toast
//  database (wpndatabase.db) is the other channel from the roadmap and is not
//  here yet; it needs SQLite and cannot be verified without it.
//
//  The store is deliberately a fixed-capacity ring. A notification source that
//  runs away must not be able to grow the bar's memory without bound, and the
//  oldest entries are the ones nobody will miss.
// =====================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace island {

// How many notifications are kept. Enough to be useful, small enough to be a
// fixed cost.
inline constexpr int kMaxNotifications = 32;

// Field capacities, including the terminator. Notifications arrive from another
// process, so every string has a hard limit rather than a length check at use.
inline constexpr size_t kAppNameCapacity = 64;
inline constexpr size_t kTitleCapacity   = 160;
inline constexpr size_t kBodyCapacity    = 512;

// How long a fresh notification claims attention in the island. After this it
// stays in the store (and keeps counting as unread) but stops being displayed.
inline constexpr unsigned int kNotificationDisplayMs = 6000;

enum class Urgency : int {
    Normal = 0,
    Low    = 1,
    High   = 2,
};

struct Notification {
    long long    id = 0;          // assigned by the store, monotonic
    unsigned long long timestampMs = 0;   // GetTickCount64 when it arrived

    char app[kAppNameCapacity]   = {};
    char title[kTitleCapacity]   = {};
    char body[kBodyCapacity]     = {};
    char iconPath[260]           = {};
    // The process that SENT it, filled in by the pipe from the client's own connection.
    //
    // This is what makes a notification from a tool look like that tool: with no `icon` field -- and almost
    // no sender bothers to set one -- the card can still show the sender's own executable icon, which is
    // exactly what the shell's own toasts do. The protocol carried an icon field for a while and NOTHING
    // ever drew it; the sender's identity is the half that costs the sender nothing.
    unsigned long senderPid = 0;

    Urgency urgency = Urgency::Normal;
    bool    read    = false;

    // When set, the sender asked for this and the store must not override it: a
    // high-urgency item is worth showing even if a newer low-urgency one arrives.
    bool    sticky  = false;
};

// Fills a notification from a JSON document produced by the wire protocol.
//
// Returns false when the document has no usable content -- specifically when both
// `title` and `body` are empty. A pipe accepts input from any local process, so
// "well-formed but empty" has to be rejected explicitly rather than displayed as
// a blank card.
bool NotificationFromJson(const char* document, Notification* out);

// Serialises a notification as a JSON object, for --notify and for tests.
// `appendId` includes the store-assigned id; senders leave it out.
void NotificationToJson(const Notification& notification, char* out, size_t capacity,
                        bool appendId);

class NotificationStore {
public:
    // Adds a notification, assigns it an id and a timestamp, and returns a pointer
    // to the stored copy -- or nullptr when the store is full of sticky entries.
    //
    // The pointer is invalidated by any later push, so callers that need to keep
    // it should copy. It exists so the caller can read the assigned id without a
    // second lookup.
    const Notification* Push(const Notification& notification);

    // Marks every notification as read. Returns how many changed.
    int MarkAllRead();

    // Marks every notification read EXCEPT this id. Returns how many changed.
    //
    // For the one item a click must NOT dismiss: a WeChat CALL. The strip is a state the user asked to have
    // for the whole call ("微信通话 也应该在灵动岛 上"), and a click on it -- which is how every message is
    // dismissed, and which a person watching a call naturally does -- retired it for the rest of the call.
    // Measured in the running bar's log: `click routed: state=notice-strip, meaning=dismiss` and
    // `notifications: 1 marked read (click)` at 13:43:19, with the call still up, and the island never showed
    // that call again (reported as "微信通话 不显示了"). The call is retired by the call ENDING, which the
    // watcher knows and reports -- not by a click.
    int MarkAllReadExcept(long long keepId);

    // Marks ONE notification as read, by id. Returns how many changed (0 or 1).
    //
    // Needed by the one notification whose life is not a timer: a WeChat CALL, which is announced when the
    // call window appears and has to stop being displayed when it goes away. Marking everything read would
    // silently swallow whatever messages happened to be unread at that moment.
    int MarkRead(long long id);

    // Number of unread notifications.
    int UnreadCount() const;

    // Count currently stored (read and unread).
    int Count() const { return count_; }

    // 0 is the newest.
    const Notification* At(int indexFromNewest) const;

    // The newest notification that should be on screen right now, or nullptr.
    //
    // "Should be on screen" means unread and younger than
    // kNotificationDisplayMs. A sticky entry stays until it is marked read.
    const Notification* ActiveDisplay(unsigned long long nowMs) const;

    // The stored notification with this id, or nullptr.
    //
    // For the ONE item whose life is a state rather than a timer: a WeChat call. It is pushed sticky so the
    // island holds it for the whole call (see WeChatWatch), and the store keeps it -- but ActiveDisplay
    // answers with the NEWEST item only, so a message arriving one second into the call took the island, and
    // when that message expired six seconds later the island went back to the resting strip with the call
    // still running. Reported as "微信通话 不显示了", with the log to match: `call announced as '通话中'`
    // at 13:37:22, `有新消息` at 13:37:23, `state: music-strip` at 13:37:29. The frame loop asks for this id
    // when ActiveDisplay has nothing, so a fresh message still wins and the call comes back afterwards.
    const Notification* Find(long long id) const;

    // Forgets everything. Used by the self-test and on an explicit user action.
    void Clear();

private:
    // Ring index of the nth entry counted back from the newest. Defined in the
    // .cpp; it is private implementation detail, not part of the interface.
    int IndexFromNewest(int indexFromNewest) const;

    Notification items_[kMaxNotifications] = {};
    int count_ = 0;        // number of valid entries
    int head_ = 0;         // index of the newest
    long long nextId_ = 1;
};

} // namespace island
