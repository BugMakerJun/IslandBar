// =====================================================================
//  StartApps.cpp -- see the header for where the list comes from and why the pins are ours.
// =====================================================================
#include "StartApps.h"

#include "Log.h"
#include "WicImage.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <thread>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace island {
namespace {

// `shell:AppsFolder` as a shell item, which is the only handle on the list the shell's own "all apps"
// view is built from.
IShellItem* AppsFolderItem() {
    IShellItem* folder = nullptr;
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder)))) {
        return nullptr;
    }
    return folder;
}

// One item's name and target, from the item itself rather than from a string built here: the parsing name
// is what the shell uses for the same item, in whichever of the three shapes it has.
bool ReadItem(IShellItem* item, StartApp* out) {
    if (!item || !out) return false;
    PWSTR name = nullptr;
    PWSTR target = nullptr;
    const bool haveName = SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name;
    // SIGDN_PARENTRELATIVEPARSING is the name within `shell:AppsFolder`, which is exactly what
    // ShellExecute wants appended to it.
    const bool haveTarget =
        SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &target)) && target;
    if (haveName && haveTarget) {
        wcsncpy_s(out->name, 112, name, _TRUNCATE);
        wcsncpy_s(out->target, 512, target, _TRUNCATE);
    }
    if (name) CoTaskMemFree(name);
    if (target) CoTaskMemFree(target);
    return haveName && haveTarget;
}

// Sorts the way the shell's own list reads: by the name the user sees, in the user's language.
bool NameBefore(const StartApp& a, const StartApp& b) {
    const int order = CompareStringEx(LOCALE_NAME_USER_DEFAULT, 0, a.name, -1, b.name, -1, nullptr,
                                      nullptr, 0);
    if (order == CSTR_LESS_THAN) return true;
    if (order == CSTR_GREATER_THAN) return false;
    return std::wcscmp(a.target, b.target) < 0;
}

} // namespace

void StartApps::PinsPath(wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    wchar_t* lastSlash = std::wcsrchr(module, L'\\');
    if (!lastSlash) return;
    *(lastSlash + 1) = L'\0';
    std::swprintf(out, capacity, L"%lsstart_pins.txt", module);
}

int StartApps::RefreshList() {
    list_.clear();
    IShellItem* folderItem = AppsFolderItem();
    if (!folderItem) {
        LogLine(L"  start: shell:AppsFolder could not be opened");
        return 0;
    }
    IShellFolder* folder = nullptr;
    HRESULT hr = folderItem->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder));
    folderItem->Release();
    if (FAILED(hr) || !folder) {
        LogLine(L"  start: shell:AppsFolder is not a folder (0x%08X)", static_cast<unsigned>(hr));
        return 0;
    }

    IEnumIDList* items = nullptr;
    const HWND owner = GetShellWindow();
    hr = folder->EnumObjects(owner, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &items);
    if (FAILED(hr) || !items) {
        folder->Release();
        LogLine(L"  start: shell:AppsFolder could not be enumerated (0x%08X)", static_cast<unsigned>(hr));
        return 0;
    }

    ITEMIDLIST* child = nullptr;
    while (items->Next(1, &child, nullptr) == S_OK) {
        IShellItem* item = nullptr;
        // From the child id alone: the folder's own item is not needed for a name that is relative to it.
        if (SUCCEEDED(SHCreateItemWithParent(nullptr, folder, child, IID_PPV_ARGS(&item))) && item) {
            StartApp app;
            if (ReadItem(item, &app) && app.name[0] != L'\0' && app.target[0] != L'\0') {
                list_.push_back(app);
            }
            item->Release();
        }
        CoTaskMemFree(child);
    }
    items->Release();
    folder->Release();

    std::sort(list_.begin(), list_.end(), NameBefore);
    ++revision_;
    LogLine(L"  start: %d application(s) in shell:AppsFolder", static_cast<int>(list_.size()));
    return static_cast<int>(list_.size());
}

void StartApps::LoadPins() {
    wchar_t path[MAX_PATH] = {};
    PinsPath(path, MAX_PATH);
    std::vector<StartApp> fresh;
    if (path[0] != L'\0') {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path, L"r, ccs=UTF-8") == 0 && file) {
            wchar_t line[1024] = {};
            while (fgetws(line, 1024, file)) {
                std::wstring text(line);
                while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' ||
                                         text.back() == L' ' || text.back() == L'\t')) {
                    text.pop_back();
                }
                while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
                    text.erase(text.begin());
                }
                // `#` starts a comment, and the file starts with one explaining itself.
                if (text.empty() || text[0] == L'#') continue;
                // A line may be `name|target` or just `target`; the name is looked up from the shell's
                // list either way, so a hand-edited line cannot invent an application.
                const size_t bar = text.find(L'|');
                const std::wstring target = (bar == std::wstring::npos) ? text : text.substr(bar + 1);
                StartApp pinned;
                bool known = false;
                for (const StartApp& app : list_) {
                    if (_wcsicmp(app.target, target.c_str()) == 0) {
                        pinned = app;
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    // Not in the shell's list (an application that was uninstalled, or a hand-edited line
                    // that never existed). Reported rather than silently dropped, because a pin that
                    // vanishes is otherwise indistinguishable from a file that was not read.
                    LogLine(L"  start: pinned target is not in shell:AppsFolder: %ls", target.c_str());
                    continue;
                }
                fresh.push_back(std::move(pinned));
            }
            fclose(file);
        }
    }
    pins_ = std::move(fresh);
    // One icon slot per pin, so the two vectors stay the same length however the list was built. The
    // icons themselves arrive later, from the workers started below.
    pinIcons_.assign(pins_.size(), std::vector<uint8_t>());
    ++revision_;
    for (int i = 0; i < static_cast<int>(pins_.size()); ++i) {
        ResolveIconAsync(pins_[i].target);
    }
    LogLine(L"  start: %d pinned application(s) from %ls", static_cast<int>(pins_.size()), path);
}

void StartApps::SavePins() const {
    wchar_t path[MAX_PATH] = {};
    PinsPath(path, MAX_PATH);
    if (path[0] == L'\0') return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"w, ccs=UTF-8") != 0 || !file) {
        LogLine(L"  start: could not write %ls", path);
        return;
    }
    std::fwprintf(file,
                  L"# IslandBar 开始菜单里自己固定的应用，一行一个。\n"
                  L"# 用法：目标（shell:AppsFolder 里的名字），可以再加 `显示名|目标`。\n"
                  L"# 目标可以是 exe 路径、`{GUID}\\name.exe` 或应用包 ID（...!App）。\n"
                  L"# 想取消固定，删掉那一行，或者在卡片里右键那个图标。\n");
    for (const StartApp& pinned : pins_) {
        std::fwprintf(file, L"%ls|%ls\n", pinned.name, pinned.target);
    }
    fclose(file);
}

bool StartApps::Pin(const wchar_t* target) {
    if (!target || !*target) return false;
    for (const StartApp& pinned : pins_) {
        if (_wcsicmp(pinned.target, target) == 0) return false;
    }
    for (const StartApp& app : list_) {
        if (_wcsicmp(app.target, target) == 0) {
            pins_.push_back(app);
            pinIcons_.push_back(std::vector<uint8_t>());
            SavePins();
            ++revision_;
            ResolveIconAsync(pins_.back().target);
            return true;
        }
    }
    return false;
}

bool StartApps::Unpin(int index) {
    if (index < 0 || index >= static_cast<int>(pins_.size())) return false;
    pins_.erase(pins_.begin() + index);
    if (index < static_cast<int>(pinIcons_.size())) {
        pinIcons_.erase(pinIcons_.begin() + index);
    }
    SavePins();
    ++revision_;
    return true;
}

const uint8_t* StartApps::PinIconPng(int index, size_t* sizeOut) const {
    if (sizeOut) *sizeOut = 0;
    if (index < 0 || index >= static_cast<int>(pins_.size())) return nullptr;
    if (index >= static_cast<int>(pinIcons_.size())) return nullptr;
    const std::vector<uint8_t>& icon = pinIcons_[static_cast<size_t>(index)];
    if (icon.empty()) return nullptr;
    if (sizeOut) *sizeOut = icon.size();
    return icon.data();
}

void StartApps::ResolveIconAsync(const wchar_t* target) {
    if (!target || !*target) return;
    std::wstring wanted = target;
    // A copy of the target, because the vector this thread would otherwise read from is rewritten by the
    // next Pin/Unpin/LoadPins -- the same reason TrayPress copies what it needs out of the tray list.
    std::thread([this, wanted]() {
        std::vector<uint8_t> png;
        if (!StartAppIconPng(wanted.c_str(), 32, &png)) return;
        for (size_t i = 0; i < pins_.size() && i < pinIcons_.size(); ++i) {
            if (_wcsicmp(pins_[i].target, wanted.c_str()) == 0) {
                pinIcons_[i] = std::move(png);
                ++iconRevision_;
                return;
            }
        }
    }).detach();
}

bool StartAppIconPng(const wchar_t* target, int side, std::vector<uint8_t>* png) {
    if (!target || !*target || !png || side <= 0) return false;
    png->clear();
    wchar_t parsing[600] = {};
    std::swprintf(parsing, 600, L"shell:AppsFolder\\%ls", target);

    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(parsing, nullptr, IID_PPV_ARGS(&item))) || !item) return false;
    IShellItemImageFactory* factory = nullptr;
    const HRESULT hr = item->QueryInterface(IID_PPV_ARGS(&factory));
    item->Release();
    if (FAILED(hr) || !factory) return false;

    const SIZE wanted = {side, side};
    HBITMAP bitmap = nullptr;
    const HRESULT imageHr = factory->GetImage(wanted, SIIGBF_ICONONLY, &bitmap);
    factory->Release();
    if (FAILED(imageHr) || !bitmap) return false;

    // Through a 32-bit DIB so the pixels can be read out: the shell hands back a bitmap of its own format,
    // and GetDIBits is the documented way to ask it for top-down BGRA.
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = side;
    info.bmiHeader.biHeight = -side;   // negative: top-down, the order a PNG wants
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(static_cast<size_t>(side) * side * 4, 0);

    HDC screen = GetDC(nullptr);
    bool ok = false;
    if (screen) {
        ok = GetDIBits(screen, bitmap, 0, static_cast<UINT>(side), pixels.data(), &info,
                       DIB_RGB_COLORS) != 0;
        ReleaseDC(nullptr, screen);
    }
    DeleteObject(bitmap);
    if (!ok) return false;

    uint8_t scratch[16 * 1024] = {};
    const size_t size = EncodeBgraToPng(pixels.data(), side, side, scratch, sizeof(scratch));
    if (size == 0) return false;
    png->assign(scratch, scratch + size);
    return true;
}

bool LaunchStartApp(const wchar_t* target) {
    if (!target || !*target) return false;
    wchar_t parsing[600] = {};
    std::swprintf(parsing, 600, L"shell:AppsFolder\\%ls", target);
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", parsing, nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecute answers with a value CAST to HINSTANCE: anything above 32 is success, and the numbers
    // below it are the shell's error codes. Documented, and worth reading rather than testing "!= null".
    const bool launched = reinterpret_cast<INT_PTR>(result) > 32;
    LogLine(L"  start: launch '%ls' -> %s (%lld)", target, launched ? "ok" : "FAILED",
            static_cast<long long>(reinterpret_cast<INT_PTR>(result)));
    return launched;
}

} // namespace island
