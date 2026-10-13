// =====================================================================
//  SystemStatus.cpp
// =====================================================================
#include "SystemStatus.h"

#include "Log.h"
#include "SystemToggles.h"   // NightLightOn / LiveCaptionsRunning: the two switches that are not radios

#include <wlanapi.h>
#include <bluetoothapis.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <cstdio>
#include <cwchar>
#include <cstring>   // memcpy: the radio state is copied into a full-size structure before it is written

#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "bthprops.lib")

namespace island {

// --- the pure part ---------------------------------------------------

SignalStrength SignalFromPercent(int percent) {
    // Zero is "no signal", not "weak signal".
    //
    // The order matters: a naive cascade turns 0 into Weak, which draws one bar for a
    // connection that is not carrying anything. The WLAN API reports 0 both for a terrible
    // connection and for an interface that is not associated, so 0 has to mean none.
    if (percent <= 0) return SignalStrength::None;
    if (percent < 25) return SignalStrength::Weak;
    if (percent < 50) return SignalStrength::Fair;
    if (percent < 75) return SignalStrength::Good;
    return SignalStrength::Excellent;
}

NetworkKind NetworkKindFor(bool hasInterface, bool connected) {
    if (!hasInterface) return NetworkKind::None;
    return connected ? NetworkKind::Connected : NetworkKind::Disconnected;
}

void InputMethodLabel(unsigned short languageId, wchar_t* out, size_t capacity) {
    if (!out || capacity < 2) return;
    out[0] = L'\0';

    // The primary language id is the low 10 bits of a LANGID.
    switch (languageId & 0x03FF) {
        case 0x04: std::swprintf(out, capacity, L"中"); return;   // Chinese
        case 0x11: std::swprintf(out, capacity, L"あ"); return;   // Japanese
        case 0x12: std::swprintf(out, capacity, L"한"); return;   // Korean
        default: break;
    }

    // Everything else gets its language code, which is what a user of that layout recognises.
    // A blank indicator would be worse than an abbreviation: it reads as a rendering fault.
    switch (languageId & 0x03FF) {
        case 0x09: std::swprintf(out, capacity, L"EN"); return;
        case 0x07: std::swprintf(out, capacity, L"DE"); return;
        case 0x0C: std::swprintf(out, capacity, L"FR"); return;
        case 0x0A: std::swprintf(out, capacity, L"ES"); return;
        case 0x10: std::swprintf(out, capacity, L"IT"); return;
        case 0x16: std::swprintf(out, capacity, L"PT"); return;
        case 0x19: std::swprintf(out, capacity, L"RU"); return;
        default: break;
    }
    std::swprintf(out, capacity, L"%02X", languageId & 0xFF);
}

bool ShouldDrawInputMethod(const SystemStatus& status, int installedLayoutCount) {
    // Not drawn when there is nothing to indicate. The shell hides the indicator until a second
    // layout is installed, and the reason is the same here: a label that can never change is not
    // information, it is furniture -- and this bar has a limited row.
    if (status.inputLanguageId == 0) return false;
    return installedLayoutCount > 1;
}

bool ShouldDrawBattery(const SystemStatus& status) {
    // The desktop case, and the reason this is a function. GetSystemPowerStatus SUCCEEDS on a
    // machine with no battery and reports BatteryLifePercent = 255 with BatteryFlag = 128
    // ("no system battery"). A naive `if (percent <= 100)` draws nothing -- good -- but a naive
    // `percent < 100` or any arithmetic on the value draws a nonsense battery.
    if (!status.batteryPresent) return false;
    if (status.batteryPercent < 0 || status.batteryPercent > 100) return false;
    return true;
}

void NetworkTooltip(const SystemStatus& status, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';

    switch (status.network) {
        case NetworkKind::None:
            std::swprintf(out, capacity, L"No wireless adapter");
            return;
        case NetworkKind::Disconnected:
            // Naming the radio state here is honest when it is known and silent when it is not:
            // this build does not expose it, and "off" would then be a guess.
            std::swprintf(out, capacity, L"Not connected");
            return;
        case NetworkKind::Connected:
            break;
    }

    if (status.ssid[0] != L'\0') {
        std::swprintf(out, capacity, L"%ls (%d%%)", status.ssid, status.signalPercent);
    } else {
        // Connected but the SSID could not be read. Still connected, and saying so is better
        // than an empty tooltip.
        std::swprintf(out, capacity, L"Connected (%d%%)", status.signalPercent);
    }
}

void BluetoothTooltip(const SystemStatus& status, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = L'\0';

    if (!status.bluetoothPresent) {
        std::swprintf(out, capacity, L"No Bluetooth");
        return;
    }
    if (!status.bluetoothOn) {
        std::swprintf(out, capacity, L"Bluetooth off");
        return;
    }
    if (status.bluetoothName[0] != L'\0') {
        std::swprintf(out, capacity, L"Bluetooth: %ls", status.bluetoothName);
    } else {
        std::swprintf(out, capacity, L"Bluetooth on");
    }
}

// --- the part that talks to Windows ----------------------------------

namespace {

// The volume, through the same endpoint API the bar already uses for its own control. Kept
// local here so the status reader has no dependency on the audio module's lifetime.
bool ReadVolume(bool* muted, int* percent) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return false;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        enumerator->Release();
        return false;
    }

    IAudioEndpointVolume* volume = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                          reinterpret_cast<void**>(&volume));
    bool ok = false;
    if (SUCCEEDED(hr) && volume) {
        BOOL isMuted = FALSE;
        float level = 0.0f;
        if (SUCCEEDED(volume->GetMute(&isMuted)) && SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level))) {
            if (muted) *muted = isMuted != FALSE;
            if (percent) *percent = static_cast<int>(level * 100.0f + 0.5f);
            ok = true;
        }
        volume->Release();
    }
    device->Release();
    enumerator->Release();
    return ok;
}

// The Bluetooth radio, or nothing.
//
// BluetoothFindFirstRadio walks a device tree, so the answer is cached by the caller: the set
// of radios does not change while the bar runs, and paying for a device enumeration per frame
// would be absurd.
bool ProbeBluetooth(bool* present, bool* on, wchar_t* name, size_t nameCapacity) {
    BLUETOOTH_FIND_RADIO_PARAMS params = {};
    params.dwSize = sizeof(params);

    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
    if (!find) {
        if (present) *present = false;
        if (on) *on = false;
        return false;
    }

    bool found = false;
    do {
        BLUETOOTH_RADIO_INFO info = {};
        info.dwSize = sizeof(info);
        if (BluetoothGetRadioInfo(radio, &info) == ERROR_SUCCESS) {
            found = true;
            if (present) *present = true;
            // A successfully opened radio is a radio that is switched on: Windows does not hand
            // out a handle to one that is off, it enumerates none. So "present" and "on" are the
            // same answer here, and the distinction is kept in the struct for the day a build
            // reports otherwise rather than being assumed away.
            if (on) *on = true;
            if (name && nameCapacity > 0) {
                std::wcsncpy(name, info.szName, nameCapacity - 1);
                name[nameCapacity - 1] = L'\0';
            }
        }
        CloseHandle(radio);
    } while (BluetoothFindNextRadio(find, &radio));

    BluetoothFindRadioClose(find);
    if (!found) {
        if (present) *present = false;
        if (on) *on = false;
    }
    return found;
}

} // namespace

bool SystemStatusReader::Open() {
    // The installed layouts, counted once. This is what decides whether the input-method indicator
    // is drawn at all, so it is read here rather than per sample.
    installedLayoutCount_ = GetKeyboardLayoutList(0, nullptr);
    if (installedLayoutCount_ < 0) installedLayoutCount_ = 0;
    LogLine(L"  system status: %d keyboard layout(s) installed", installedLayoutCount_);

    if (!wlanOpen_) {
        DWORD negotiated = 0;
        HANDLE client = nullptr;
        const DWORD result = WlanOpenHandle(2, nullptr, &negotiated, &client);
        if (result == ERROR_SUCCESS) {
            wlanClient_ = client;
            wlanOpen_ = true;
            LogLine(L"  system status: WLAN client open (version %lu)", negotiated);
        } else {
            // Not fatal: the other indicators still work, and a bar that refused to start
            // because the wireless service was unavailable would be worse than one without a
            // WiFi icon.
            LogLine(L"  system status: WlanOpenHandle -> %lu, no wireless readings", result);
        }
    }
    return true;
}

void SystemStatusReader::Close() {
    if (wlanOpen_ && wlanClient_) {
        WlanCloseHandle(static_cast<HANDLE>(wlanClient_), nullptr);
        wlanClient_ = nullptr;
        wlanOpen_ = false;
    }
}

void SystemStatusReader::Sample(SystemStatus* out) {
    if (!out) return;

    // --- network ---
    if (wlanOpen_ && wlanClient_) {
        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        if (WlanEnumInterfaces(static_cast<HANDLE>(wlanClient_), nullptr, &list) == ERROR_SUCCESS &&
            list) {
            const bool hasInterface = list->dwNumberOfItems > 0;
            bool connected = false;
            out->signal = SignalStrength::None;
            out->signalPercent = 0;
            out->ssid[0] = L'\0';

            for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                const WLAN_INTERFACE_INFO& info = list->InterfaceInfo[i];
                if (info.isState != wlan_interface_state_connected) continue;
                connected = true;

                DWORD size = 0;
                WLAN_CONNECTION_ATTRIBUTES* attributes = nullptr;
                WLAN_OPCODE_VALUE_TYPE opcode = wlan_opcode_value_type_query_only;
                if (WlanQueryInterface(static_cast<HANDLE>(wlanClient_), &info.InterfaceGuid,
                                       wlan_intf_opcode_current_connection, nullptr, &size,
                                       reinterpret_cast<PVOID*>(&attributes), &opcode) ==
                    ERROR_SUCCESS) {
                    const WLAN_ASSOCIATION_ATTRIBUTES& assoc = attributes->wlanAssociationAttributes;
                    out->signalPercent = static_cast<int>(assoc.wlanSignalQuality);
                    out->signal = SignalFromPercent(out->signalPercent);

                    // The SSID is a byte array with its own length and is NOT null terminated,
                    // so it is copied with its length rather than by string functions.
                    const DWORD length =
                        assoc.dot11Ssid.uSSIDLength < 32 ? assoc.dot11Ssid.uSSIDLength : 32;
                    for (DWORD c = 0; c < length; ++c) {
                        out->ssid[c] = static_cast<wchar_t>(assoc.dot11Ssid.ucSSID[c]);
                    }
                    out->ssid[length] = L'\0';
                    WlanFreeMemory(attributes);
                }
                break;
            }

            out->network = NetworkKindFor(hasInterface, connected);

            // The radio itself, which is a different fact from "is there a network": a machine whose Wi-Fi has
            // been switched off still has an interface, and it is that state the panel's 飞行模式 tile shows.
            //
            // The buffer the API returns is VARIABLE LENGTH -- a DWORD count followed by one entry per phy --
            // so the first entry is read only after checking that the returned size actually covers it. The
            // obvious version reads `PhyRadioState[0]` straight out of the pointer, which overreads on a
            // machine that reports a single phy.
            if (list->dwNumberOfItems > 0) {
                DWORD size = 0;
                WLAN_RADIO_STATE* radio = nullptr;
                WLAN_OPCODE_VALUE_TYPE opcode = wlan_opcode_value_type_query_only;
                if (WlanQueryInterface(static_cast<HANDLE>(wlanClient_),
                                       &list->InterfaceInfo[0].InterfaceGuid,
                                       wlan_intf_opcode_radio_state, nullptr, &size,
                                       reinterpret_cast<PVOID*>(&radio), &opcode) == ERROR_SUCCESS &&
                    radio) {
                    const size_t firstEntry = offsetof(WLAN_RADIO_STATE, PhyRadioState) +
                                              sizeof(WLAN_PHY_RADIO_STATE);
                    if (size >= firstEntry && radio->dwNumberOfPhys > 0) {
                        out->wifiRadioOn =
                            radio->PhyRadioState[0].dot11SoftwareRadioState != dot11_radio_state_off;
                    }
                    WlanFreeMemory(radio);
                }
            }
            WlanFreeMemory(list);
        }
    }

    // --- bluetooth ---
    //
    // Probed once and remembered. `bluetoothPresent` is only ever made true by a successful
    // probe, so a machine without a radio keeps reporting none.
    if (!bluetoothProbed_) {
        bluetoothProbed_ = ProbeBluetooth(&out->bluetoothPresent, &out->bluetoothOn,
                                          out->bluetoothName, 248);
        LogLine(L"  system status: bluetooth present=%d on=%d name='%ls'",
                out->bluetoothPresent ? 1 : 0, out->bluetoothOn ? 1 : 0, out->bluetoothName);
    } else {
        // The cache is only about the SET of radios; the on/off state can change while running,
        // so it is re-probed each sample. That is a device-tree walk per poll, which is why the
        // poll interval is seconds rather than milliseconds.
        ProbeBluetooth(&out->bluetoothPresent, &out->bluetoothOn, out->bluetoothName, 248);
    }

    // --- volume ---
    {
        bool muted = false;
        int percent = 0;
        if (ReadVolume(&muted, &percent)) {
            out->muted = muted;
            out->volumePercent = percent;
        }
    }

    // --- the two switches that are not radios ---
    //
    // Night light: the OS's own registry value, which is what the Settings page shows (there is no API,
    // and measured: writing the value does not switch the feature -- see SystemToggles). Live captions:
    // whether its process is running, which is the only state it has.
    //
    // Sampled here rather than asked for at the moment of the draw, because the panel is rebuilt every
    // frame and a Toolhelp snapshot per frame is not.
    // ForDisplay, not NightLightOn: once this session has pressed the page's switch, the page's own answer
    // is the one the tile must show -- see NightLightOnForDisplay.
    out->nightLightOn = NightLightOnForDisplay();
    out->liveCaptionsOn = LiveCaptionsRunning();

    // --- power ---
    {
        SYSTEM_POWER_STATUS power = {};
        if (GetSystemPowerStatus(&power)) {
            // 255 is the documented "unknown" for every one of these fields, and 128 in
            // BatteryFlag means there is no battery at all. Both are checked rather than
            // inferred from the percentage, because 255 percent is what a naive read produces.
            const bool noBattery = (power.BatteryFlag & 128) != 0;
            const bool percentKnown = power.BatteryLifePercent != 255;
            const bool acKnown = power.ACLineStatus != 255;

            out->batteryPresent = !noBattery && percentKnown;
            if (percentKnown) out->batteryPercent = static_cast<int>(power.BatteryLifePercent);
            if (acKnown) out->onAcPower = power.ACLineStatus == 1;

            // SystemStatusFlag is 1 while battery saver is on. It is a separate field from the
            // battery's own flags, which are about the CELL rather than about the power policy.
            out->batterySaverOn = power.SystemStatusFlag == 1;
        }
    }

    // --- input method ---
    //
    // Read from the FOREGROUND window's thread, not from the bar's own: the bar is a
    // WS_EX_NOACTIVATE tool window that never has focus, so its own layout is not the one the user
    // is typing with. Asking the foreground is the whole point of the indicator.
    {
        const HWND foreground = GetForegroundWindow();
        if (foreground) {
            const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
            const HKL layout = GetKeyboardLayout(thread);
            if (layout) {
                // The low word is the language id and the high word the device handle. Splitting
                // them rather than taking the whole HKL is what makes the label lookup correct:
                // 0x08040804 as a whole is not a language.
                out->inputLanguageId = static_cast<unsigned short>(reinterpret_cast<UINT_PTR>(layout) & 0xFFFF);
            }
        }
    }

    out->ageMs = 0;
}

// NOT USED BY THE BAR ANY MORE, and the measurement that retired it is the reason it stays in writing.
//
// The tiles switch the radios through `Windows.Devices.Radios` (SystemToggles.h), which is what the shell
// itself uses and which works here for BOTH radios. This WLAN write was tried first, and on this machine it
// FAILS SILENTLY: all three shapes below report ERROR_SUCCESS and the radio does not move -- the user's own
// log carries two of them, from two presses of the 飞行模式 tile:
//
//     wifi radio: WlanSetInterface single=0 full=0 counted=0
//
// A write that says success and changes nothing is the one failure mode this project keeps re-learning, and
// it is precisely why the tile's caption is a READING rather than a memory. Kept rather than deleted because
// the documented WLAN route is the fallback if a machine ever turns up where the Radios API is refused --
// and it is labelled here so that nobody reaches for it without knowing what it did on this one.
bool SetWifiRadioOn(bool on, unsigned long* error) {
    if (error) *error = 0;

    // Its own handle for the one write, rather than the reader's: the write comes from the click handler and
    // the reader's handle lives on the frame loop's own sampling path, and sharing one client handle between
    // the two would put a WLAN call on whichever thread happened to hold it.
    DWORD negotiated = 0;
    HANDLE client = nullptr;
    const DWORD opened = WlanOpenHandle(2, nullptr, &negotiated, &client);
    if (opened != ERROR_SUCCESS || !client) {
        if (error) *error = opened;
        return false;
    }

    bool ok = false;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    const DWORD enumerated = WlanEnumInterfaces(client, nullptr, &list);
    if (enumerated == ERROR_SUCCESS && list) {
        for (DWORD i = 0; i < list->dwNumberOfItems && !ok; ++i) {
            const GUID& guid = list->InterfaceInfo[i].InterfaceGuid;

            // The CURRENT state is read first, because the write takes a whole WLAN_RADIO_STATE: it says what
            // every phy should do, so a structure built from nothing would claim the machine has no phys and
            // the call would be rejected or, worse, taken as "no opinion".
            DWORD size = 0;
            WLAN_RADIO_STATE* state = nullptr;
            WLAN_OPCODE_VALUE_TYPE opcode = wlan_opcode_value_type_query_only;
            if (WlanQueryInterface(client, &guid, wlan_intf_opcode_radio_state, nullptr, &size,
                                   reinterpret_cast<PVOID*>(&state), &opcode) != ERROR_SUCCESS ||
                !state) {
                continue;
            }

            const size_t firstEntry =
                offsetof(WLAN_RADIO_STATE, PhyRadioState) + sizeof(WLAN_PHY_RADIO_STATE);
            if (size < firstEntry || state->dwNumberOfPhys == 0) {
                LogLine(L"  wifi radio: the query returned %lu byte(s) and %lu phy(s); not switching",
                        static_cast<unsigned long>(size),
                        static_cast<unsigned long>(state->dwNumberOfPhys));
                WlanFreeMemory(state);
                continue;
            }

            // The write is built in a FULL-SIZE structure, and that is not a detail.
            //
            // The query hands back a VARIABLE-LENGTH buffer -- a count followed by one entry per phy -- while
            // the write expects the whole WLAN_RADIO_STATE. Passing the query's own size failed with
            // ERROR_INVALID_PARAMETER (87) on this machine: the API validated the size against the structure
            // it declares, not against what it had just returned. So the query's bytes are copied into a
            // zeroed full-size structure and THAT is what is written.
            WLAN_RADIO_STATE wanted = {};
            const size_t copy = size < sizeof(wanted) ? size : sizeof(wanted);
            std::memcpy(&wanted, state, copy);
            WlanFreeMemory(state);

            for (DWORD phy = 0; phy < wanted.dwNumberOfPhys; ++phy) {
                wanted.PhyRadioState[phy].dot11SoftwareRadioState =
                    on ? dot11_radio_state_on : dot11_radio_state_off;
            }

            // Three shapes, because the API's documentation does not say which one the call wants and the
            // drivers differ about what they accept: ONE PHY, which is what this machine takes; the full
            // declared structure; and the count-plus-one-entry form the QUERY itself returns.
            //
            // Measured here: full = ERROR_INVALID_PARAMETER (87), counted = 87, single = ERROR_SUCCESS. That
            // is why all three are tried and all three are reported -- a radio that refuses to switch and a
            // switch that was never asked for properly look identical from the tile.
            const DWORD fullSize = static_cast<DWORD>(sizeof(wanted));
            const DWORD countedSize =
                static_cast<DWORD>(offsetof(WLAN_RADIO_STATE, PhyRadioState) +
                                   sizeof(WLAN_PHY_RADIO_STATE));
            WLAN_PHY_RADIO_STATE one = {};
            one.dwPhyIndex = 0;
            one.dot11SoftwareRadioState = on ? dot11_radio_state_on : dot11_radio_state_off;

            const DWORD single = WlanSetInterface(client, &guid, wlan_intf_opcode_radio_state,
                                                  static_cast<DWORD>(sizeof(one)), &one, nullptr);
            DWORD full = ERROR_SUCCESS;
            DWORD counted = ERROR_SUCCESS;
            if (single != ERROR_SUCCESS) {
                full = WlanSetInterface(client, &guid, wlan_intf_opcode_radio_state, fullSize, &wanted,
                                        nullptr);
            }
            if (single != ERROR_SUCCESS && full != ERROR_SUCCESS) {
                counted = WlanSetInterface(client, &guid, wlan_intf_opcode_radio_state, countedSize,
                                           &wanted, nullptr);
            }
            LogLine(L"  wifi radio: WlanSetInterface single=%lu full=%lu counted=%lu",
                    static_cast<unsigned long>(single), static_cast<unsigned long>(full),
                    static_cast<unsigned long>(counted));

            const DWORD accepted = single == ERROR_SUCCESS ? single
                                  : full == ERROR_SUCCESS   ? full
                                                            : counted;
            if (error) *error = accepted;   // the first refusal when all three failed
            ok = accepted == ERROR_SUCCESS;
        }
        WlanFreeMemory(list);
    } else if (error) {
        *error = enumerated;
    }

    WlanCloseHandle(client, nullptr);
    return ok;
}

} // namespace island
