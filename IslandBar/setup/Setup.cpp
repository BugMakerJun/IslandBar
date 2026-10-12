// =====================================================================
//  Setup.cpp -- IslandBar's installer and uninstaller, in one executable.
//
//  WHAT IT IS
//
//  One file, IslandBarSetup.exe, with island_bar.exe carried inside it as a resource
//  (see payload.rc.in). Double-clicked, it installs the bar for the current user:
//  copies the program out, puts a shortcut in the Start Menu, makes it start at logon,
//  registers itself with Settings > Apps, and starts the bar so the taskbar is replaced
//  immediately. Run with /uninstall -- which is what the registered uninstall entry does,
//  through a copy of this program left in the install directory -- it undoes all of it.
//
//  WHY IT IS NOT INNO SETUP / NSIS / WiX
//
//  None of the three is installed on this machine, and a per-user install of one
//  executable and two shortcuts does not need a framework. What a framework would have
//  given us is the uninstall registration and the shortcut creation, and both are a few
//  API calls -- see WriteUninstallEntry and CreateShortcut.
//
//  NO ELEVATION, AND NOTHING OUTSIDE THE USER'S OWN FILES
//
//  Every write is under %USERPROFILE% or in HKCU, so there is no UAC prompt and no way
//  for this program to be the reason something else on the machine changed.
//
//  THE INSTALL DIRECTORY IS %USERPROFILE%\IslandBar, AND THAT IS NOT AN OVERSIGHT
//
//  `weather.txt`, `start_folder.txt` and the log are all read from BESIDE THE EXECUTABLE
//  (Weather.cpp, StartApps.cpp, Log.cpp), so the directory the program runs from is also
//  the directory that holds the user's own settings and the folder his pinned applications
//  live in. Installing to a new directory would look exactly like losing them. It is also
//  where the development scripts deploy to (scripts/verify-panel-anchor.ps1), so a test
//  build and an installed build land in the same place instead of two bars racing.
//
//  WHAT IT DOES NOT DO
//
//  It does not delete the user's settings on uninstall unless asked (/remove-settings):
//  the pinned applications and the location/temperature choices are the user's work, and
//  an uninstall that throws them away is a thing people learn not to trust.
//
//  UTF-8 source with wide literals (the project compiles with /utf-8), because the
//  messages are shown to a Chinese-speaking user.
// =====================================================================

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")

namespace {

// --- the names -----------------------------------------------------------------------
//
// The constants are here rather than spelled out at each use because three of them have to
// agree with something else: the resource id with payload.rc.in, and the two file names with
// the uninstall entry this program writes.
constexpr int kPayloadResourceId = 1001;      // 1001 in payload.rc.in -- change both together
constexpr wchar_t kAppName[] = L"IslandBar";
constexpr wchar_t kProgramName[] = L"island_bar.exe";
constexpr wchar_t kUninstallerName[] = L"IslandBarUninstall.exe";
constexpr wchar_t kStartupShortcut[] = L"IslandBar.lnk";
constexpr wchar_t kMenuShortcut[] = L"IslandBar.lnk";
// A second Start Menu entry, with --keep-taskbar. It exists for the machine this program is
// carried to: the bar hides the Windows taskbar on startup by design, and somebody trying it on an
// unfamiliar machine needs a way to run it that leaves the shell alone -- without knowing that
// `--keep-taskbar` is a thing. Named in ASCII so it cannot be mangled by a shell code page.
constexpr wchar_t kSafeModeShortcut[] = L"IslandBar (safe mode).lnk";
constexpr wchar_t kVersion[] = L"1.2";
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\IslandBar";
constexpr wchar_t kSettingsKey[] = L"Software\\IslandBar";

// Everything the run has to say, kept as it goes: it ends up in the log file and, for a run
// that was not asked to be silent, in the message box at the end. One place, so the box and
// the log cannot disagree.
std::vector<std::wstring> g_log;
bool g_silent = false;

// The log is opened and written LINE BY LINE, from the first line of the run.
//
// It used to be a vector of lines written out at the end, which is the wrong shape for an
// installer: any path that returns early -- and any crash -- leaves no file at all, so "the
// installer did nothing" is indistinguishable from "the installer is not running". It was
// opened here after exactly that: a run that exited with status 1 and wrote nothing.
// Declared here because the log needs to know where this program is, and the path helpers live
// further down with the rest of the filesystem work.
std::wstring ThisProgram();
std::wstring DirectoryOf(const std::wstring& path);

FILE* g_logFile = nullptr;
std::wstring g_logPath;

void OpenLog() {
    if (g_logFile) return;
    // MORE THAN ONE PLACE TO TRY, in order, and it is not defensive programming: the first
    // version of this program wrote its log to %TEMP% and nothing else, and a run that could not
    // write there left NO FILE AT ALL -- so a failure that had been carefully logged looked
    // exactly like a program that never started. (It was also how the very first bug in the
    // installer was found: a copy of this program run from inside the project tree is demoted to a
    // low integrity token by this development environment, and that token cannot create a file in
    // the user's %TEMP%.)
    std::vector<std::wstring> candidates;
    wchar_t temp[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length > 0 && length < MAX_PATH) {
        candidates.push_back(std::wstring(temp) + L"IslandBarSetup.log");
    }
    const std::wstring beside = DirectoryOf(ThisProgram());
    if (!beside.empty()) {
        candidates.push_back(beside + L"\\IslandBarSetup.log");
    }

    for (const std::wstring& candidate : candidates) {
        if (_wfopen_s(&g_logFile, candidate.c_str(), L"w, ccs=UTF-8") == 0 && g_logFile) {
            g_logPath = candidate;
            return;
        }
    }
    g_logFile = nullptr;
}

// Where the log went, for the summary: the one thing a person needs if the run did not do what
// they expected.
const wchar_t* LogPathForMessage() {
    OpenLog();
    return g_logPath.empty() ? L"(could not be written)" : g_logPath.c_str();
}

void Say(const std::wstring& line) {
    g_log.push_back(line);
    OutputDebugStringW((line + L"\n").c_str());
    OpenLog();
    if (g_logFile) {
        fwprintf(g_logFile, L"%s\n", line.c_str());
        fflush(g_logFile);
    }
}

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring FormatLastError(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result;
    if (length && text) {
        result.assign(text, length);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
            result.pop_back();
        }
    } else {
        result = L"(no description)";
    }
    if (text) LocalFree(text);
    return Format(L"%lu (%s)", code, result.c_str());
}

void SayError(const std::wstring& what, DWORD code) {
    Say(Format(L"%s  --  %s", what.c_str(), FormatLastError(code).c_str()));
}

// --- paths ---------------------------------------------------------------------------

std::wstring ThisProgram() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) return L"";
        if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The shell's own answer for "where is this user's profile", rather than reading an
// environment variable that a redirected profile or a run from another account can lie about.
bool KnownFolder(REFKNOWNFOLDERID id, std::wstring* out) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) || !raw) return false;
    *out = raw;
    CoTaskMemFree(raw);
    return true;
}

std::wstring ProfileRelative(const wchar_t* leaf) {
    std::wstring profile;
    if (KnownFolder(FOLDERID_Profile, &profile)) {
        return profile + L"\\" + leaf;
    }
    // The only reason to be here is a broken profile lookup, and %USERPROFILE% is the value the
    // rest of the program (and the developer scripts) use.
    wchar_t fallback[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", fallback, MAX_PATH) > 0) {
        return std::wstring(fallback) + L"\\" + leaf;
    }
    return L"";
}

void CloseLog() {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

// --- stopping the bar ----------------------------------------------------------------

// Whether this process runs from `directory` -- the question that decides whether an install
// has to stop somebody else's copy of the bar.
bool ImageIsInside(const std::wstring& image, const std::wstring& directory) {
    if (image.empty() || directory.empty()) return false;
    const std::wstring file = FileNameOf(image);
    if (_wcsicmp(file.c_str(), kProgramName) != 0) return false;
    return _wcsnicmp(image.c_str(), directory.c_str(), directory.size()) == 0;
}

// Ends every island_bar.exe running from `directory`, and answers how many that was.
//
// The bar holds its own executable open, so replacing it while it runs fails with a sharing
// violation -- which is the sort of failure that reads as "the installer is broken". It is
// also the moment the native taskbar is at risk: the bar hides it and restores it on a CLEAN
// exit, so a bar that is terminated here leaves the shell's taskbar hidden, and the uninstall
// path puts it back explicitly (see RestoreShellTaskbar).
int StopRunningBar(const std::wstring& directory, bool onlyFromThatDirectory) {
    const DWORD ownPid = GetCurrentProcessId();
    int stopped = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == ownPid) continue;
            if (_wcsicmp(entry.szExeFile, kProgramName) != 0) continue;

            if (onlyFromThatDirectory) {
                HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                           entry.th32ProcessID);
                if (!probe) continue;
                std::vector<wchar_t> image(MAX_PATH * 2);
                DWORD size = static_cast<DWORD>(image.size());
                const bool have = QueryFullProcessImageNameW(probe, 0, image.data(), &size) != FALSE;
                CloseHandle(probe);
                if (!have) continue;
                if (!ImageIsInside(std::wstring(image.data(), size), directory)) continue;
            }

            HANDLE victim = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                        entry.th32ProcessID);
            if (!victim) continue;
            if (TerminateProcess(victim, 0)) {
                ++stopped;
                WaitForSingleObject(victim, 3000);
            }
            CloseHandle(victim);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return stopped;
}

// Puts the shell's taskbar back on screen.
//
// The uninstall path needs this and cannot avoid it: the bar had the native taskbar hidden, it
// is ended rather than closed (a file cannot be replaced while its image is open), and the
// restore that would have run on a clean exit therefore never happens. Without this, uninstall
// leaves a desktop with no taskbar at all -- which looks like the uninstaller broke Windows.
//
// Only windows that are NOT visible are shown, so a taskbar the user hid himself is left alone.
void RestoreShellTaskbar() {
    int shown = 0;
    const wchar_t* classes[] = {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"};
    for (const wchar_t* className : classes) {
        for (HWND window = FindWindowW(className, nullptr); window != nullptr;
             window = FindWindowExW(nullptr, window, className, nullptr)) {
            if (IsWindowVisible(window)) continue;
            ShowWindow(window, SW_SHOW);
            ++shown;
        }
    }
    if (shown > 0) {
        Say(Format(L"restored the Windows taskbar (%d window(s))", shown));
        // The work area is only rewritten by the AppBar path, which is off unless the bar was
        // started with --appbar; the shell recomputes it from the taskbar it now has on screen.
    }
}

// --- shortcuts -----------------------------------------------------------------------

bool CreateShortcut(const std::wstring& linkPath, const std::wstring& target,
                    const std::wstring& arguments, const std::wstring& workingDirectory,
                    const std::wstring& description) {
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link));
    if (FAILED(hr) || !link) {
        SayError(L"creating the shortcut object failed", static_cast<DWORD>(hr));
        return false;
    }

    link->SetPath(target.c_str());
    link->SetArguments(arguments.empty() ? L"" : arguments.c_str());
    link->SetWorkingDirectory(workingDirectory.c_str());
    link->SetDescription(description.empty() ? L"" : description.c_str());

    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(hr) && file) {
        hr = file->Save(linkPath.c_str(), TRUE);
        file->Release();
    }
    link->Release();

    if (FAILED(hr)) {
        SayError(Format(L"writing '%s' failed", linkPath.c_str()), static_cast<DWORD>(hr));
        return false;
    }
    Say(Format(L"shortcut written: %s", linkPath.c_str()));
    return true;
}

bool DeleteFileQuietly(const std::wstring& path) {
    if (!DeleteFileW(path.c_str())) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) return false;
    }
    return true;
}

// Removes a directory and everything in it, through the shell so the contents go to the RECYCLE
// BIN. This is only used by /remove-settings, and the difference matters: the folder being removed
// is the user's own Start folder, full of shortcuts he pinned by hand one at a time. A "delete my
// settings" that permanently destroys them is not a thing worth having.
bool DeleteDirectoryToRecycleBin(const std::wstring& path) {
    // SHFileOperation takes a double-null-terminated list, and refuses a relative path.
    std::wstring from = path;
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW operation = {};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int result = SHFileOperationW(&operation);
    if (result != 0 || operation.fAnyOperationsAborted) {
        Say(Format(L"could not remove '%s' (code %d)", path.c_str(), result));
        return false;
    }
    Say(Format(L"removed (to the Recycle Bin): %s", path.c_str()));
    return true;
}

// --- the registry --------------------------------------------------------------------

bool WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;
}

bool WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

// What Settings > Apps reads. All of it under HKCU, so no elevation and no machine-wide state.
bool WriteUninstallEntry(const std::wstring& directory, unsigned long long programBytes) {
    HKEY key = nullptr;
    const DWORD created = RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr,
                                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key,
                                          nullptr);
    if (created != ERROR_SUCCESS) {
        SayError(L"registering the uninstall entry failed", created);
        return false;
    }

    const std::wstring program = directory + L"\\" + kProgramName;
    const std::wstring uninstaller = directory + L"\\" + kUninstallerName;
    const std::wstring quiet = L"\"" + uninstaller + L"\" /uninstall /silent";

    bool ok = true;
    ok = WriteString(key, L"DisplayName", kAppName) && ok;
    ok = WriteString(key, L"DisplayVersion", kVersion) && ok;
    ok = WriteString(key, L"Publisher", L"BugMakerJun") && ok;
    ok = WriteString(key, L"InstallLocation", directory) && ok;
    ok = WriteString(key, L"DisplayIcon", program) && ok;
    ok = WriteString(key, L"UninstallString", L"\"" + uninstaller + L"\" /uninstall") && ok;
    ok = WriteString(key, L"QuietUninstallString", quiet) && ok;
    ok = WriteString(key, L"InstallSource", ThisProgram()) && ok;
    ok = WriteDword(key, L"NoModify", 1) && ok;
    ok = WriteDword(key, L"NoRepair", 1) && ok;
    // Settings > Apps shows this, and it is the only size it can know about an installation.
    ok = WriteDword(key, L"EstimatedSize",
                    static_cast<DWORD>(programBytes / 1024 + 64)) && ok;
    RegCloseKey(key);
    if (!ok) Say(L"some of the uninstall entry could not be written");

    // A second key, for this project's own tooling rather than for Windows: the install
    // directory is worth being able to read back without searching the disk.
    HKEY settings = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &settings, nullptr) == ERROR_SUCCESS) {
        WriteString(settings, L"InstallDir", directory);
        WriteString(settings, L"Version", kVersion);
        RegCloseKey(settings);
    }
    Say(L"registered under Settings > Apps (HKEY_CURRENT_USER)");
    return ok;
}

void RemoveRegistryTree(HKEY root, const wchar_t* subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    RegCloseKey(key);
    const DWORD result = RegDeleteTreeW(root, subkey);
    if (result == ERROR_SUCCESS) {
        Say(Format(L"registry key removed: %s", subkey));
    } else {
        SayError(Format(L"removing HKEY_CURRENT_USER\\%s failed", subkey), result);
    }
}

// --- the payload ---------------------------------------------------------------------

// The bar, out of this program's own resources, written to `destination`.
//
// Written to a temporary name and moved into place: a copy interrupted half way through would
// otherwise leave an island_bar.exe that starts and then crashes, which is worse than not
// installing at all. The move is atomic on the same volume.
bool ExtractPayload(const std::wstring& destination, std::wstring* error) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(kPayloadResourceId), RT_RCDATA);
    if (!resource) {
        *error = L"the payload is not in this executable (the resource is missing)";
        return false;
    }
    HGLOBAL loaded = LoadResource(nullptr, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(nullptr, resource);
    if (!bytes || size == 0) {
        *error = L"the payload resource is empty";
        return false;
    }

    const std::wstring temporary = destination + L".new";
    DeleteFileQuietly(temporary);
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = Format(L"creating '%s' failed (%s)", temporary.c_str(),
                        FormatLastError(GetLastError()).c_str());
        return false;
    }
    DWORD written = 0;
    const bool wrote = WriteFile(file, bytes, size, &written, nullptr) != FALSE && written == size;
    CloseHandle(file);
    if (!wrote) {
        *error = Format(L"writing '%s' failed (%s)", temporary.c_str(),
                        FormatLastError(GetLastError()).c_str());
        DeleteFileQuietly(temporary);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *error = Format(L"replacing '%s' failed (%s)", destination.c_str(),
                        FormatLastError(GetLastError()).c_str());
        DeleteFileQuietly(temporary);
        return false;
    }
    Say(Format(L"program written: %s (%lu bytes)", destination.c_str(), size));
    return true;
}

unsigned long long FileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return 0;
    return (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

// --- usage ---------------------------------------------------------------------------

struct Options {
    bool uninstall = false;
    bool autostart = true;
    bool launch = true;
    bool removeSettings = false;
    bool help = false;
    std::wstring directory;
};

const wchar_t* kUsage =
    L"IslandBarSetup.exe -- installs IslandBar for the current user.\n"
    L"\n"
    L"  (no arguments)     install and start it\n"
    L"  /uninstall         remove it (your settings are kept)\n"
    L"  /remove-settings   with /uninstall: delete the settings too\n"
    L"  /dir=PATH          install somewhere other than %USERPROFILE%\\IslandBar\n"
    L"  /no-autostart      do not make it start at logon\n"
    L"  /no-launch         install without starting the bar\n"
    L"  /silent            no message boxes; the summary goes to the log only\n"
    L"\n"
    L"Nothing here needs an administrator: the program goes in your own profile and the\n"
    L"one registry key it writes is under HKEY_CURRENT_USER.\n";

Options ParseCommandLine() {
    Options options;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return options;

    for (int i = 1; i < count; ++i) {
        const std::wstring argument = arguments[i];
        if (_wcsicmp(argument.c_str(), L"/uninstall") == 0 ||
            _wcsicmp(argument.c_str(), L"-uninstall") == 0) {
            options.uninstall = true;
        } else if (_wcsicmp(argument.c_str(), L"/remove-settings") == 0) {
            options.removeSettings = true;
        } else if (_wcsicmp(argument.c_str(), L"/no-autostart") == 0) {
            options.autostart = false;
        } else if (_wcsicmp(argument.c_str(), L"/no-launch") == 0) {
            options.launch = false;
        } else if (_wcsicmp(argument.c_str(), L"/silent") == 0) {
            g_silent = true;
        } else if (_wcsnicmp(argument.c_str(), L"/dir=", 5) == 0) {
            options.directory = argument.substr(5);
            // A quoted value arrives with its quotes intact through CommandLineToArgvW only when
            // the quotes were written around the whole thing; take them off either way.
            if (options.directory.size() >= 2 && options.directory.front() == L'"' &&
                options.directory.back() == L'"') {
                options.directory = options.directory.substr(1, options.directory.size() - 2);
            }
            while (!options.directory.empty() && options.directory.back() == L'\\') {
                options.directory.pop_back();
            }
        } else if (_wcsicmp(argument.c_str(), L"/?") == 0 ||
                   _wcsicmp(argument.c_str(), L"/help") == 0) {
            options.help = true;
        }
    }
    LocalFree(arguments);
    return options;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // The first line of the log, before anything that can fail. An installer that leaves no trace
    // of having run is the hardest kind to diagnose, and it is what this program did once: see the
    // note on OpenLog.
    Say(Format(L"IslandBar setup %s -- %s", kVersion, ThisProgram().c_str()));

    // The shortcuts are COM objects, and this has to happen before the first one is created. The
    // apartment is the STA because IShellLink is not free-threaded.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const Options options = ParseCommandLine();
    const std::wstring program = ThisProgram();

    if (options.help) {
        MessageBoxW(nullptr, kUsage, L"IslandBar setup", MB_OK | MB_ICONINFORMATION);
        Say(L"usage shown; nothing was changed");
        CloseLog();
        CoUninitialize();
        return 0;
    }

    // Where the install is: what was asked for, otherwise this program's own directory when this
    // IS the uninstaller copy (it lives in the install directory, and being run from there is the
    // most reliable statement of where the thing was installed), otherwise the default.
    std::wstring directory = options.directory;
    if (directory.empty()) {
        if (options.uninstall && _wcsicmp(FileNameOf(program).c_str(), kUninstallerName) == 0) {
            directory = DirectoryOf(program);
        } else {
            directory = ProfileRelative(L"IslandBar");
        }
    }
    if (directory.empty()) {
        MessageBoxW(nullptr, L"Cannot tell where the user's profile is.", L"IslandBar setup",
                    MB_OK | MB_ICONERROR);
        Say(L"the user's profile folder could not be found");
        CloseLog();
        CoUninitialize();
        return 1;
    }

    const std::wstring programPath = directory + L"\\" + kProgramName;
    const std::wstring uninstallerPath = directory + L"\\" + kUninstallerName;

    Say(Format(L"IslandBar %s setup", kVersion));
    Say(Format(L"mode: %s", options.uninstall ? L"uninstall" : L"install"));
    Say(Format(L"directory: %s", directory.c_str()));

    int result = 0;
    std::wstring summary;

    std::wstring startupFolder;
    std::wstring menuFolder;
    KnownFolder(FOLDERID_Startup, &startupFolder);
    KnownFolder(FOLDERID_Programs, &menuFolder);
    const std::wstring startupLink = startupFolder.empty()
                                         ? L""
                                         : startupFolder + L"\\" + kStartupShortcut;
    const std::wstring menuLink =
        menuFolder.empty() ? L"" : menuFolder + L"\\" + kMenuShortcut;
    const std::wstring safeLink =
        menuFolder.empty() ? L"" : menuFolder + L"\\" + kSafeModeShortcut;

    if (!options.uninstall) {
        // --- install -----------------------------------------------------------------
        if (!CreateDirectoryW(directory.c_str(), nullptr)) {
            const DWORD code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS) {
                SayError(Format(L"creating '%s' failed", directory.c_str()), code);
                summary = Format(L"Could not create the install directory:\n\n%s", directory.c_str());
                result = 1;
            }
        }

        if (result == 0) {
            const int stopped = StopRunningBar(directory, /*onlyFromThatDirectory=*/true);
            if (stopped > 0) {
                Say(Format(L"stopped %d running bar(s) from the install directory", stopped));
                Sleep(700);   // let the image go before replacing the file
                // The bar that was ended had the shell's taskbar hidden, and its own restore ran
                // only in the exit path that a TerminateProcess skips. Leaving it hidden would
                // hand the NEW bar a taskbar that is already invisible -- and then that bar would
                // not own the hide, so ITS clean exit would leave the desktop with no taskbar at
                // all. Showing it here costs a flash of the native bar during the install and
                // removes that whole family of state.
                RestoreShellTaskbar();
            }

            std::wstring error;
            if (!ExtractPayload(programPath, &error)) {
                // The file is there but could not be replaced: the running copy is the only thing
                // that can be holding it, and it is not running from where we looked.
                const int others = StopRunningBar(directory, /*onlyFromThatDirectory=*/false);
                if (others > 0) {
                    Say(Format(L"stopped %d bar(s) started from elsewhere", others));
                    Sleep(700);
                }
                if (!ExtractPayload(programPath, &error)) {
                    Say(Format(L"the program could not be installed: %s", error.c_str()));
                    summary = L"The program could not be installed.\n\n" + error +
                              L"\n\nIs IslandBar running from another folder? Close it and try again.";
                    result = 1;
                }
            }
        }

        if (result == 0) {
            // The uninstaller is a copy of this program, so the entry in Settings > Apps points at
            // something that exists even if the file the user downloaded is long gone.
            if (!CopyFileW(program.c_str(), uninstallerPath.c_str(), FALSE)) {
                SayError(L"copying the uninstaller failed (uninstall will be unavailable)",
                         GetLastError());
            } else {
                Say(Format(L"uninstaller written: %s", uninstallerPath.c_str()));
            }

            if (!menuLink.empty()) {
                CreateShortcut(menuLink, programPath, L"", directory, L"IslandBar");
            }
            if (!safeLink.empty()) {
                CreateShortcut(safeLink, programPath, L"--keep-taskbar", directory,
                               L"保留 Windows 系统任务栏，只当浮层用（排错）");
            }
            if (options.autostart && !startupLink.empty()) {
                CreateShortcut(startupLink, programPath, L"", directory,
                               L"Start IslandBar at logon");
            } else if (!options.autostart) {
                Say(L"autostart not requested: no shortcut in the Startup folder");
            }
            WriteUninstallEntry(directory, FileSizeOf(programPath));

            if (options.launch) {
                // Started through the shell, which is the whole point: the bar hides the shell's
                // taskbar, and UIPI drops that request when it comes from a process at a lower
                // integrity level than the shell's. A process started from Explorer is alongside it.
                SHELLEXECUTEINFOW run = {};
                run.cbSize = sizeof(run);
                run.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
                run.lpFile = programPath.c_str();
                run.lpDirectory = directory.c_str();
                run.nShow = SW_SHOWNORMAL;
                if (ShellExecuteExW(&run)) {
                    if (run.hProcess) CloseHandle(run.hProcess);
                    Say(L"the bar has been started");
                } else {
                    SayError(L"starting the bar failed", GetLastError());
                }
            }

            summary = Format(
                L"IslandBar %s is installed.\n\n"
                L"  program:   %s\n"
                L"  autostart: %s\n"
                L"  menu:      %s\n"
                L"             %s\n"
                L"  uninstall: %s\n\n"
                L"Your settings and your pinned applications stay in:\n  %s\n\n"
                L"To stop it for a while: end island_bar.exe in Task Manager and then run the\n"
                L"uninstaller above, or restart Explorer, and the Windows taskbar comes back.",
                kVersion, programPath.c_str(),
                options.autostart ? L"on (a shortcut in your Startup folder)"
                                  : L"off (not requested)",
                menuLink.empty() ? L"(no Start Menu folder)" : menuLink.c_str(),
                safeLink.c_str(),
                uninstallerPath.c_str(), directory.c_str());
        }
    } else {
        // --- uninstall -------------------------------------------------------------------
        // Only the copy THIS installation put there. A bar started from somewhere else -- a
        // development build, another install -- is not this uninstaller's to end, and if it hid the
        // shell's taskbar it will put it back itself when it exits.
        const int stopped = StopRunningBar(directory, /*onlyFromThatDirectory=*/true);
        if (stopped > 0) {
            Say(Format(L"stopped %d running bar(s)", stopped));
            Sleep(500);
            // It was ended rather than closed, so its own restore did not run.
            RestoreShellTaskbar();
        }

        if (!startupLink.empty() && !DeleteFileQuietly(startupLink)) {
            SayError(L"removing the autostart shortcut failed", GetLastError());
        } else if (!startupLink.empty()) {
            Say(L"autostart shortcut removed");
        }
        if (!menuLink.empty() && !DeleteFileQuietly(menuLink)) {
            SayError(L"removing the Start Menu shortcut failed", GetLastError());
        }
        if (!safeLink.empty()) DeleteFileQuietly(safeLink);

        RemoveRegistryTree(HKEY_CURRENT_USER, kUninstallKey);
        RemoveRegistryTree(HKEY_CURRENT_USER, kSettingsKey);

        DeleteFileQuietly(programPath);
        Say(Format(L"program removed: %s", programPath.c_str()));

        std::wstring keeper;
        if (options.removeSettings) {
            // Everything, including the pinned applications and the location choice.
            for (const wchar_t* name :
                 {L"weather.txt", L"start_folder.txt", L"start_pins.txt", L"island_bar.log",
                  L"start_pins.txt.migrated"}) {
                DeleteFileQuietly(directory + L"\\" + name);
            }
            // The pinned applications: the folder is inside the install directory, so it is ours to
            // remove -- and it is recycled rather than erased, because those shortcuts are the user's
            // own work. A folder the user MOVED elsewhere (start_folder.txt can point anywhere) is
            // left alone; the summary says so.
            const std::wstring pins = directory + L"\\Start";
            if (GetFileAttributesW(pins.c_str()) != INVALID_FILE_ATTRIBUTES) {
                DeleteDirectoryToRecycleBin(pins);
            }
            if (!RemoveDirectoryW(directory.c_str())) {
                keeper = Format(L"%s could not be removed (something in it is still in use)",
                                directory.c_str());
            }
            summary = Format(L"IslandBar has been uninstalled.\n\nProgram, shortcuts, autostart "
                             L"and the registry entry are gone, and so are your settings -- the pinned\n"
                             L"applications went to the Recycle Bin, so they can still be recovered.\n%s",
                             keeper.empty() ? L"" : (L"\n" + keeper).c_str());
        } else {
            summary = Format(
                L"IslandBar has been uninstalled.\n\n"
                L"  removed:   the program, the shortcuts, the autostart entry, the registry entry\n"
                L"  kept:      your settings and your pinned applications\n"
                L"             %s\n"
                L"  taskbar:   %s\n\n"
                L"To remove the settings as well, run the uninstaller again with /remove-settings.",
                directory.c_str(),
                stopped > 0 ? L"restored" : L"was already in place");
        }
        Say(L"uninstalled");

        // This program is the uninstaller copy, and Windows cannot delete the image it is running.
        // The standard answer is to ask for the delete at the next restart.
        if (_wcsicmp(FileNameOf(program).c_str(), kUninstallerName) == 0) {
            MoveFileExW(program.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }

    CloseLog();
    if (result != 0 && summary.find(L"Could not") == std::wstring::npos &&
        summary.find(L"could not") == std::wstring::npos) {
        summary += L"\n\nThe log is at:\n" + std::wstring(LogPathForMessage());
    }
    if (!g_silent) {
        MessageBoxW(nullptr, summary.c_str(), L"IslandBar setup",
                    MB_OK | (result == 0 ? MB_ICONINFORMATION : MB_ICONERROR));
    }

    CoUninitialize();
    return result;
}
