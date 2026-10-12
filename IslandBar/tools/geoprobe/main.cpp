// =====================================================================
//  geoprobe -- can this machine tell us WHERE it is?
//
//  WHY THIS EXISTS
//
//  The weather on the bar's status line needs a latitude and a longitude. Today it
//  gets them from a city name in weather.txt, because IP geolocation was measured
//  answering "Tokyo" on a machine whose clock is on Asia/Shanghai (its traffic
//  leaves through a proxy). The user asked for the real thing instead:
//
//      "天气的能不能调用定位 而不是根据ip"
//
//  Windows has a location service, and this asks it -- because the answer decides
//  the design and cannot be guessed:
//
//    * if it returns a position near the user, the bar should ask Windows and keep
//      the file only as a fallback;
//    * if it returns a position on another continent (the same IP-based service
//      underneath), the bar must NOT use it -- wrong weather at the right time is
//      worse than no weather -- and the file stays the primary source;
//    * if it refuses (the system setting is off, or an unpackaged desktop app is
//      denied consent), the bar has to fall back, and only a real call can say
//      which of the two it does here.
//
//  It cannot be asked from PowerShell on this machine: its WindowsRuntime interop
//  is not loadable (see gsmtcprobe). Hence C++/WinRT, built with the bar's own
//  toolchain.
//
//  Read-only, except for one thing it cannot avoid: asking for consent is what
//  may put a Windows location prompt on screen. It starts nothing and changes
//  nothing else.
// =====================================================================

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>

#include <windows.h>

#include <chrono>
#include <cstdio>

using namespace winrt::Windows::Devices::Geolocation;

namespace {

const char* AccessName(GeolocationAccessStatus status) {
    switch (status) {
        case GeolocationAccessStatus::Allowed: return "Allowed";
        case GeolocationAccessStatus::Denied: return "Denied";
        case GeolocationAccessStatus::Unspecified: return "Unspecified";
        default: return "(other)";
    }
}

const char* StatusName(PositionStatus status) {
    switch (status) {
        case PositionStatus::Ready: return "Ready";
        // "Initializing", not "Initialized": the enum's own spelling, which is what the SDK
        // header says and the reason this table was rewritten against it rather than from memory.
        case PositionStatus::Initializing: return "Initializing";
        case PositionStatus::NoData: return "NoData";
        case PositionStatus::Disabled: return "Disabled";
        case PositionStatus::NotInitialized: return "NotInitialized";
        case PositionStatus::NotAvailable: return "NotAvailable";
        default: return "(other)";
    }
}

void ReportState(const char* when, Geolocator const& locator) {
    std::printf("  %-22s: %s\n", when, StatusName(locator.LocationStatus()));
}

} // namespace

int main() {
    std::printf("--- Windows location ---\n");

    // Whether the SERVICE is on at all, before anything asks for a position: this is the
    // switch in Settings > Privacy > Location, and it is a different question from whether
    // THIS program is allowed -- a machine with the service off cannot answer however
    // politely it is asked. It is read from the service's own configuration key, because
    // this SDK exposes no such WinRT call (an `IsLocationEnabledForUser` was written here
    // first and does not exist; the header is the truth about that).
    {
        DWORD enabled = 0;
        DWORD size = sizeof(enabled);
        DWORD type = 0;
        const LONG query =
            RegGetValueW(HKEY_LOCAL_MACHINE,
                         L"SYSTEM\\CurrentControlSet\\Services\\lfsvc\\Service\\Configuration",
                         L"Status", RRF_RT_REG_DWORD, &type, &enabled, &size);
        std::printf("  system location switch: %s\n",
                    (query == ERROR_SUCCESS) ? (enabled ? "on" : "OFF") : "(cannot read)");
    }

    // And whether THIS process may use it. For an unpackaged desktop program this is
    // the call that raises the Windows consent prompt the first time.
    const auto askedAt = GetTickCount64();
    GeolocationAccessStatus access = GeolocationAccessStatus::Unspecified;
    try {
        access = Geolocator::RequestAccessAsync().get();
    } catch (const winrt::hresult_error& e) {
        std::printf("  RequestAccessAsync threw 0x%08lX\n",
                    static_cast<unsigned long>(e.code().value));
    }
    std::printf("  consent               : %s (asked in %llu ms)\n", AccessName(access),
                static_cast<unsigned long long>(GetTickCount64() - askedAt));

    Geolocator locator;
    locator.DesiredAccuracy(PositionAccuracy::Default);
    ReportState("status before asking", locator);

    // A fifteen second cap of our own, on top of the call's own timeout: a provider
    // that never answers must not leave the probe sitting there.
    std::printf("  asking for a position (up to 15 s)...\n");
    std::fflush(stdout);

    Geoposition position = nullptr;
    try {
        position = locator.GetGeopositionAsync().get();
    } catch (const winrt::hresult_error& e) {
        std::printf("  GetGeopositionAsync threw 0x%08lX\n",
                    static_cast<unsigned long>(e.code().value));
    }
    ReportState("status after asking", locator);

    if (!position) {
        std::printf("NO POSITION\n");
        return 1;
    }

    const Geocoordinate coordinate = position.Coordinate();
    const auto point = coordinate.Point().Position();
    std::printf("  latitude / longitude  : %.6f, %.6f\n", point.Latitude, point.Longitude);
    std::printf("  accuracy              : %.1f m\n", coordinate.Accuracy());
    std::printf("  altitude              : %s\n",
                coordinate.Altitude() ? "present" : "none");
    std::printf("  timestamp             : %llu\n",
                static_cast<unsigned long long>(coordinate.Timestamp().time_since_epoch().count()));

    // What the position is actually MADE of. `PositionSource` is the interesting one:
    // WiFi and Satellite mean the machine worked it out itself, while IPAddress means
    // Windows asked the same kind of service the bar could have asked, and a proxied
    // connection would answer somewhere else entirely.
    try {
        const auto source = coordinate.PositionSource();
        const char* name = "Unknown";
        switch (source) {
            case PositionSource::Cellular: name = "Cellular"; break;
            case PositionSource::Satellite: name = "Satellite"; break;
            case PositionSource::WiFi: name = "WiFi"; break;
            case PositionSource::IPAddress: name = "IPAddress"; break;
            case PositionSource::Unknown: name = "Unknown"; break;
            default: break;
        }
        std::printf("  position source       : %s\n", name);
    } catch (...) {
        std::printf("  position source       : (not available)\n");
    }

    std::printf("DONE\n");
    return 0;
}
