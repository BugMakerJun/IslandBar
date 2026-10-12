// =====================================================================
//  gsmtc_probe -- enumerate every GSMTC session and describe it.
//
//  WHY THIS EXISTS
//
//  WeChat calls are to be shown in the bar's pill. Whether that is possible
//  from the media path the bar ALREADY uses -- C++/WinRT over
//  GlobalSystemMediaTransportControlsSessionManager -- or only from WeChat's
//  call WINDOW decides the whole design:
//
//    * a Communication session carries a timeline, so the elapsed time comes
//      for free and the call is detected the same way music is;
//    * a window carries no peer name and no timeline, so the caller has to be
//      detected by window class and the time counted from first sight.
//
//  The question cannot be answered from PowerShell on this machine: its
//  WindowsRuntime interop is not loadable, so the WinRT types cannot be
//  reached from a script at all. It CAN be answered from C++/WinRT, which is
//  what the bar is written in -- hence this probe, built with the same
//  toolchain as the bar.
//
//  Read-only: it enumerates and prints. It starts nothing and touches nothing.
// =====================================================================

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

// WideCharToMultiByte and CP_UTF8. The WinRT headers pull in a great deal but not
// this, and the bar's own translation unit includes it explicitly for the same
// reason.
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

const char* PlaybackTypeName(
    const winrt::Windows::Foundation::IReference<winrt::Windows::Media::MediaPlaybackType>& type) {
    using winrt::Windows::Media::MediaPlaybackType;
    if (!type) return "(null)";
    switch (type.Value()) {
        case MediaPlaybackType::Music:   return "Music";
        case MediaPlaybackType::Video:   return "Video";
        case MediaPlaybackType::Image:   return "Image";
        case MediaPlaybackType::Unknown: return "Unknown";
        default:                         return "(other)";
    }
}

std::string Narrow(const winrt::hstring& s) {
    if (s.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), need, nullptr, nullptr);
    return out;
}

// One tick is 100 ns, the unit every WinRT time span uses.
//
// `count` is a MEMBER FUNCTION on WinRT's TimeSpan, not a data member: it is a
// std::chrono::duration underneath, so `span.count` alone is a pointer-to-member
// and does not compile. The bar's own code learned this the same way.
std::string TimeSpanText(winrt::Windows::Foundation::TimeSpan span) {
    const long long totalSeconds = span.count() / 10'000'000;
    const long long hours = totalSeconds / 3600;
    const long long minutes = (totalSeconds % 3600) / 60;
    const long long seconds = totalSeconds % 60;
    char buffer[64] = {};
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld", hours, minutes, seconds);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld", minutes, seconds);
    }
    return buffer;
}

const char* StatusName(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus s) {
    using Status = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
    switch (s) {
        case Status::Closed:  return "Closed";
        case Status::Opened:  return "Opened";
        case Status::Changing:return "Changing";
        case Status::Stopped: return "Stopped";
        case Status::Playing: return "Playing";
        case Status::Paused:  return "Paused";
        default:              return "(other)";
    }
}

}  // namespace

int main(int argc, char** argv) {
    // `--skip-next` asks the current session to skip, and prints how the request was answered.
    //
    // THE TOOL FOR "点击 立马切歌": whether a skip is slow because the REQUEST is not delivered, or because the
    // PLAYER is slow to act on it. The bar's own log says only that the bar sent the request and that the player
    // accepted it -- `music: next requested` then `transport next accepted` in 114 ms -- while the new track's
    // metadata appeared 7.5 s later. Those are two different faults and this flag separates them: it sends the
    // same call the bar does, from a process that is not the bar, and then reports what the session says about
    // itself for the next few seconds.
    const bool skipNext = (argc > 1 && std::strcmp(argv[1], "--skip-next") == 0);

    // C++/WinRT needs an apartment for the process. Uninitialised is fine here
    // because this probe only reads, but initialising keeps the behaviour the
    // same as the bar's, which does initialise.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    try {
        using namespace winrt::Windows::Media::Control;
        const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        const auto sessions = manager.GetSessions();

        std::printf("GSMTC sessions: %u\n", sessions.Size());
        int index = 0;
        for (const auto& session : sessions) {
            std::printf("\n--- session %d ---\n", index++);
            std::printf("  appId        : %s\n", Narrow(session.SourceAppUserModelId()).c_str());

            try {
                const auto props = session.TryGetMediaPropertiesAsync().get();
                std::printf("  PlaybackType : %s\n", PlaybackTypeName(props.PlaybackType()));
                std::printf("  Title        : %s\n", Narrow(props.Title()).c_str());
                std::printf("  Artist       : %s\n", Narrow(props.Artist()).c_str());
                std::printf("  AlbumTitle   : %s\n", Narrow(props.AlbumTitle()).c_str());
                std::printf("  Thumbnail    : %s\n",
                            props.Thumbnail() ? "present" : "none");
            } catch (const winrt::hresult_error& e) {
                std::printf("  media properties failed: 0x%08lX\n",
                            static_cast<unsigned long>(e.code().value));
            }

            try {
                std::printf("  Status       : %s\n", StatusName(session.GetPlaybackInfo().PlaybackStatus()));
            } catch (const winrt::hresult_error&) {
                std::printf("  Status       : (failed)\n");
            }

            try {
                const auto timeline = session.GetTimelineProperties();
                std::printf("  Position     : %s\n", TimeSpanText(timeline.Position()).c_str());
                std::printf("  StartTime    : %s\n", TimeSpanText(timeline.StartTime()).c_str());
                std::printf("  EndTime      : %s\n", TimeSpanText(timeline.EndTime()).c_str());
            } catch (const winrt::hresult_error&) {
                std::printf("  Timeline     : (failed)\n");
            }
        }

        std::printf("\nAny session with PlaybackType 'Unknown' and a WeChat appId is the call.\n");

        if (skipNext && sessions.Size() > 0) {
            const auto session = sessions.GetAt(0);
            const winrt::hstring before = [&] {
                try {
                    return session.TryGetMediaPropertiesAsync().get().Title();
                } catch (...) {
                    return winrt::hstring();
                }
            }();
            std::printf("\n--- skip next ---\n");
            std::printf("  before       : %s\n", Narrow(before).c_str());
            const auto sentAt = GetTickCount64();
            bool accepted = false;
            try {
                accepted = session.TrySkipNextAsync().get();
            } catch (const winrt::hresult_error& e) {
                std::printf("  request failed: 0x%08lX\n", static_cast<unsigned long>(e.code().value));
            }
            std::printf("  accepted     : %s after %llu ms\n", accepted ? "yes" : "no",
                        static_cast<unsigned long long>(GetTickCount64() - sentAt));

            // What the session says for the next 10 seconds, second by second: the title changing is "the
            // player acted", and when it changes is the answer to "how slow is a skip".
            for (int i = 0; i < 10; ++i) {
                Sleep(1000);
                winrt::hstring now;
                std::string status;
                try {
                    now = session.TryGetMediaPropertiesAsync().get().Title();
                    status = StatusName(session.GetPlaybackInfo().PlaybackStatus());
                } catch (...) {
                }
                std::printf("  +%2d s       : %-40s %s%s\n", i + 1, Narrow(now).c_str(),
                            status.c_str(),
                            (now != before) ? "   <- CHANGED" : "");
            }
        }
    } catch (const winrt::hresult_error& e) {
        std::printf("FATAL: 0x%08lX\n", static_cast<unsigned long>(e.code().value));
        return 1;
    }

    return 0;
}
