// =====================================================================
//  TrayIcons.cpp
// =====================================================================
#include "TrayIcons.h"

#include "Log.h"

#include <tlhelp32.h>
// CLSIDFromString, for the registry's textual IconGuid: with WIN32_LEAN_AND_MEAN the COM declarations
// are not part of windows.h.
#include <objbase.h>

#include <algorithm>
#include <cwchar>
#include <string>

namespace island {
namespace {

// The file name of every process that is running, lower-cased, into `out`.
//
// Toolhelp rather than OpenProcess per pid: this runs on a timer and the machine has hundreds of
// processes, while a snapshot is one call and gives the names, which is all the filter needs.
void RunningProcessNames(std::vector<std::wstring>* out) {
    out->clear();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring name = entry.szExeFile;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            out->push_back(std::move(name));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

bool IsRunning(const std::vector<std::wstring>& running, const std::wstring& exePath) {
    const wchar_t* slash = std::wcsrchr(exePath.c_str(), L'\\');
    std::wstring name = slash ? std::wstring(slash + 1) : exePath;
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    for (const std::wstring& candidate : running) {
        if (candidate == name) return true;
    }
    return false;
}

void ReadStringValue(HKEY key, const wchar_t* name, std::wstring* out) {
    out->clear();
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes == 0) {
        return;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()),
                         &bytes) == ERROR_SUCCESS) {
        *out = buffer.data();
    }
}

void ReadBinaryValue(HKEY key, const wchar_t* name, std::vector<uint8_t>* out) {
    out->clear();
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_BINARY || bytes == 0) {
        return;
    }
    std::vector<uint8_t> buffer(bytes);
    if (RegQueryValueExW(key, name, nullptr, &type, buffer.data(), &bytes) == ERROR_SUCCESS) {
        buffer.resize(bytes);
        *out = std::move(buffer);
    }
}

// A file's modification time as a sortable number, or 0. Used for the newest-first order: a subkey's
// last write moves when its icon changes, which is the closest thing the tray has to "used recently".
long long KeyLastWriteTime(const std::wstring& subKey) {
    HKEY key = nullptr;
    const std::wstring path = std::wstring(L"Control Panel\\NotifyIconSettings\\") + subKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return 0;
    }
    FILETIME written = {};
    long long value = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr, &written) == ERROR_SUCCESS) {
        ULARGE_INTEGER ticks = {};
        ticks.LowPart = written.dwLowDateTime;
        ticks.HighPart = written.dwHighDateTime;
        value = static_cast<long long>(ticks.QuadPart);
    }
    RegCloseKey(key);
    return value;
}

} // namespace

int TrayIcons::Refresh() {
    std::vector<std::wstring> running;
    RunningProcessNames(&running);
    processesSeen_ = static_cast<int>(running.size());

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SettingsKeyPath(), 0, KEY_READ, &root) != ERROR_SUCCESS) {
        entries_.clear();
        list_.clear();
        examined_ = 0;
        runningApps_ = 0;
        return 0;
    }

    std::vector<Entry> fresh;
    examined_ = 0;
    runningApps_ = 0;

    DWORD index = 0;
    wchar_t subKeyName[256] = {};
    DWORD subKeyLength = 256;
    while (RegEnumKeyExW(root, index, subKeyName, &subKeyLength, nullptr, nullptr, nullptr,
                         nullptr) == ERROR_SUCCESS) {
        ++index;
        subKeyLength = 256;

        HKEY subKey = nullptr;
        if (RegOpenKeyExW(root, subKeyName, 0, KEY_READ, &subKey) != ERROR_SUCCESS) continue;
        ++examined_;

        Entry entry;
        ReadStringValue(subKey, L"ExecutablePath", &entry.exePath);
        ReadBinaryValue(subKey, L"IconSnapshot", &entry.png);

        DWORD promoted = 0;
        DWORD promotedSize = sizeof(promoted);
        DWORD type = 0;
        if (RegQueryValueExW(subKey, L"IsPromoted", nullptr, &type,
                             reinterpret_cast<BYTE*>(&promoted), &promotedSize) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            entry.icon.promoted = promoted != 0;
        }

        // THE SHELL'S OWN IDENTITY FOR THE ICON, which is what a press needs (see TrayPress). A UID and
        // an IconGuid are the two ways an application registers an icon and each entry carries one or the
        // other -- measured: `UID` is a DWORD, `IconGuid` a string "{...}".
        {
            DWORD uid = 0;
            DWORD uidSize = sizeof(uid);
            DWORD uidType = 0;
            if (RegQueryValueExW(subKey, L"UID", nullptr, &uidType, reinterpret_cast<BYTE*>(&uid),
                                 &uidSize) == ERROR_SUCCESS &&
                uidType == REG_DWORD) {
                entry.icon.uid = uid;
                entry.icon.hasUid = true;
            }
            std::wstring guidText;
            ReadStringValue(subKey, L"IconGuid", &guidText);
            if (!guidText.empty()) {
                GUID parsed = {};
                // Through the string form rather than the bytes: the value is REG_SZ, braces and all.
                std::wstring trimmed = guidText;
                if (trimmed.size() >= 2 && trimmed.front() == L'{' && trimmed.back() == L'}') {
                    trimmed = trimmed.substr(1, trimmed.size() - 2);
                }
                if (SUCCEEDED(CLSIDFromString((L"{" + trimmed + L"}").c_str(), &parsed))) {
                    entry.icon.guid = parsed;
                    entry.icon.hasGuid = true;
                }
            }
        }
        RegCloseKey(subKey);

        // Only a PNG is drawn: the value is documented by measurement (see the header), and a future
        // Windows that changes the format must show nothing rather than a grey rectangle.
        if (entry.png.size() < 8 || entry.png[0] != 0x89 || entry.png[1] != 0x50) continue;
        if (!IsRunning(running, entry.exePath)) continue;
        ++runningApps_;

        // The name: what the application called itself, or failing that its file name. Read after the
        // filter so the registry is not walked twice for icons nobody will see.
        std::wstring tooltip;
        {
            HKEY subKeyAgain = nullptr;
            if (RegOpenKeyExW(root, subKeyName, 0, KEY_READ, &subKeyAgain) == ERROR_SUCCESS) {
                ReadStringValue(subKeyAgain, L"InitialTooltip", &tooltip);
                RegCloseKey(subKeyAgain);
            }
        }
        if (tooltip.empty()) {
            const wchar_t* slash = std::wcsrchr(entry.exePath.c_str(), L'\\');
            tooltip = slash ? std::wstring(slash + 1) : entry.exePath;
        }
        wcsncpy_s(entry.icon.name, 96, tooltip.c_str(), _TRUNCATE);
        entry.icon.pngSize = entry.png.size();
        entry.writtenAt = KeyLastWriteTime(subKeyName);

        fresh.push_back(std::move(entry));
    }
    RegCloseKey(root);

    // Newest registration first, so the icons that have changed recently -- which is what a running
    // application's tray icon does -- are the ones nearest the right-hand end.
    std::stable_sort(fresh.begin(), fresh.end(), [](const Entry& a, const Entry& b) {
        return a.writtenAt > b.writtenAt;
    });

    // The PNG pointers point into each entry's own buffer, so they are re-seated AFTER the sort and the
    // move: a pointer into a vector that has just been reordered is exactly the kind of dangling read a
    // screenshot cannot show.
    for (Entry& entry : fresh) entry.icon.png = entry.png.data();

    bool changed = fresh.size() != entries_.size();
    if (!changed) {
        for (size_t i = 0; i < fresh.size(); ++i) {
            if (std::wcscmp(fresh[i].icon.name, entries_[i].icon.name) != 0 ||
                fresh[i].png.size() != entries_[i].png.size() ||
                std::memcmp(fresh[i].png.data(), entries_[i].png.data(), fresh[i].png.size()) != 0) {
                changed = true;   // a different icon, or the same application's icon redrawn
                break;
            }
        }
    }

    entries_ = std::move(fresh);
    for (Entry& entry : entries_) entry.icon.png = entry.png.data();

    // ... and the contiguous view the renderer walks, built LAST so that every pointer in it refers to the
    // storage the entries will keep (see the note on `list_`).
    list_.clear();
    list_.reserve(entries_.size());
    for (const Entry& entry : entries_) list_.push_back(entry.icon);

    if (changed) ++revision_;
    return Count();
}

} // namespace island

