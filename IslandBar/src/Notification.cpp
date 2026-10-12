// =====================================================================
//  Notification.cpp
// =====================================================================
#include "Notification.h"

#include <windows.h>   // GetTickCount64

#include <cstdio>
#include <cstring>

#include "MiniJson.h"

namespace island {
namespace {

// Copies into a fixed field, always terminating. An over-long field is truncated
// rather than rejected: losing the tail of a long message is better than losing
// the notification.
void CopyField(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0) return;
    if (!source) {
        destination[0] = '\0';
        return;
    }
    strncpy_s(destination, capacity, source, _TRUNCATE);
}

Urgency ParseUrgency(const char* text) {
    if (!text) return Urgency::Normal;
    if (_stricmp(text, "high") == 0) return Urgency::High;
    if (_stricmp(text, "low") == 0) return Urgency::Low;
    return Urgency::Normal;
}

const char* UrgencyName(Urgency urgency) {
    switch (urgency) {
        case Urgency::High: return "high";
        case Urgency::Low:  return "low";
        default:            return "normal";
    }
}

// Writes a JSON string field. `first` tracks the separator so no call site has to.
void WriteField(char* out, size_t capacity, size_t* used, bool* first,
                const char* key, const char* value) {
    if (!out || !used || !first || *used >= capacity) return;

    char escaped[kBodyCapacity * 2] = {};
    json::Escape(value ? value : "", escaped, sizeof(escaped));

    const int written = std::snprintf(out + *used, capacity - *used, "%s\"%s\":\"%s\"",
                                      *first ? "" : ",", key, escaped);
    if (written > 0) {
        *used += static_cast<size_t>(written);
        *first = false;
    }
}

} // namespace

bool NotificationFromJson(const char* document, Notification* out) {
    if (!document || !out) return false;

    Notification parsed;

    char title[kTitleCapacity] = {};
    char body[kBodyCapacity] = {};
    if (!json::GetString(document, "title", title, sizeof(title))) title[0] = '\0';
    if (!json::GetString(document, "body", body, sizeof(body))) body[0] = '\0';

    // A notification with nothing to say is not shown. This is the one rejection
    // that matters, because the pipe accepts writes from any local process and a
    // blank card would be indistinguishable from a rendering bug.
    if (title[0] == '\0' && body[0] == '\0') return false;

    char app[kAppNameCapacity] = {};
    char icon[260] = {};
    char urgency[16] = {};
    json::GetString(document, "app", app, sizeof(app));
    json::GetString(document, "icon", icon, sizeof(icon));
    json::GetString(document, "urgency", urgency, sizeof(urgency));

    CopyField(parsed.app, kAppNameCapacity, app);
    CopyField(parsed.title, kTitleCapacity, title);
    CopyField(parsed.body, kBodyCapacity, body);
    CopyField(parsed.iconPath, 260, icon);
    parsed.urgency = ParseUrgency(urgency);

    long long sticky = 0;
    if (json::GetInt(document, "sticky", &sticky)) {
        parsed.sticky = sticky != 0;
    }

    *out = parsed;
    return true;
}

void NotificationToJson(const Notification& notification, char* out, size_t capacity,
                        bool appendId) {
    if (!out || capacity == 0) return;
    out[0] = '\0';

    const int opened = std::snprintf(out, capacity, "{");
    if (opened < 0) return;
    size_t used = static_cast<size_t>(opened);
    bool first = true;

    if (appendId) {
        const int written = std::snprintf(out + used, capacity - used, "\"id\":%lld",
                                          notification.id);
        if (written > 0) {
            used += static_cast<size_t>(written);
            first = false;
        }
    }

    WriteField(out, capacity, &used, &first, "app", notification.app);
    WriteField(out, capacity, &used, &first, "title", notification.title);
    WriteField(out, capacity, &used, &first, "body", notification.body);
    WriteField(out, capacity, &used, &first, "icon", notification.iconPath);
    WriteField(out, capacity, &used, &first, "urgency", UrgencyName(notification.urgency));

    if (notification.sticky) {
        const int written = std::snprintf(out + used, capacity - used, "%s\"sticky\":1",
                                          first ? "" : ",");
        if (written > 0) {
            used += static_cast<size_t>(written);
            first = false;
        }
    }

    if (used + 2 <= capacity) {
        out[used++] = '}';
        out[used] = '\0';
    }
}

// --- store -----------------------------------------------------------

int NotificationStore::IndexFromNewest(int indexFromNewest) const {
    return (head_ - indexFromNewest + kMaxNotifications) % kMaxNotifications;
}

const Notification* NotificationStore::Push(const Notification& notification) {
    Notification stored = notification;
    stored.id = nextId_++;
    stored.timestampMs = GetTickCount64();
    stored.read = false;

    // The ring always advances. When it is full this overwrites the oldest entry,
    // which is the one nobody will miss, and it is what stops a runaway sender from
    // growing the bar's memory without bound.
    head_ = (head_ + 1) % kMaxNotifications;
    if (count_ < kMaxNotifications) ++count_;

    items_[head_] = stored;
    return &items_[head_];
}

int NotificationStore::MarkAllRead() {
    int changed = 0;
    for (int i = 0; i < count_; ++i) {
        Notification& item = items_[IndexFromNewest(i)];
        if (!item.read) {
            item.read = true;
            ++changed;
        }
    }
    return changed;
}

int NotificationStore::MarkRead(long long id) {
    for (int i = 0; i < count_; ++i) {
        Notification& item = items_[IndexFromNewest(i)];
        if (item.id == id && !item.read) {
            item.read = true;
            return 1;
        }
    }
    return 0;
}

int NotificationStore::MarkAllReadExcept(long long keepId) {
    int changed = 0;
    for (int i = 0; i < count_; ++i) {
        Notification& item = items_[IndexFromNewest(i)];
        if (item.read || item.id == keepId) continue;
        item.read = true;
        ++changed;
    }
    return changed;
}

int NotificationStore::UnreadCount() const {
    int unread = 0;
    for (int i = 0; i < count_; ++i) {
        if (!items_[IndexFromNewest(i)].read) ++unread;
    }
    return unread;
}

const Notification* NotificationStore::At(int indexFromNewest) const {
    if (indexFromNewest < 0 || indexFromNewest >= count_) return nullptr;
    return &items_[IndexFromNewest(indexFromNewest)];
}

const Notification* NotificationStore::ActiveDisplay(unsigned long long nowMs) const {
    if (count_ == 0) return nullptr;

    const Notification& newest = items_[head_];
    if (newest.read) return nullptr;

    // A sticky notification stays until it is read; otherwise the display window
    // runs from when it arrived. A timestamp in the future (a tick wrap, or a
    // test) is treated as brand new rather than as expired.
    if (!newest.sticky) {
        const unsigned long long age = (nowMs >= newest.timestampMs)
                                           ? nowMs - newest.timestampMs
                                           : 0;
        if (age > kNotificationDisplayMs) return nullptr;
    }
    return &newest;
}

void NotificationStore::Clear() {
    count_ = 0;
    head_ = 0;
    nextId_ = 1;
}

const Notification* NotificationStore::Find(long long id) const {
    if (id == 0) return nullptr;
    for (int i = 0; i < count_; ++i) {
        const Notification& item = items_[IndexFromNewest(i)];
        if (item.id == id) return &item;
    }
    return nullptr;
}

} // namespace island
