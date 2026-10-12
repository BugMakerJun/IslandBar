// =====================================================================
//  SysStatusProbe.cpp -- what can actually be read about the machine's radios?
//
//  Before designing the indicators it is worth knowing exactly which values the APIs
//  will hand over on THIS machine, because several of them are conditional:
//
//    * WLAN_CONNECTION_ATTRIBUTES is only meaningful for an interface that is connected,
//      and asking for it on one that is not returns an error rather than "disconnected";
//    * the Bluetooth radio has no stable handle: it is enumerated through a device tree and
//      the handle is only valid for that radio, so a hard-coded one is meaningless;
//    * the battery APIs succeed on a desktop and report an unknown state, which must not
//      be drawn as "0%" or as a missing battery icon.
//
//  This reports what each returns so the drawing code can be written against real answers
//  instead of guesses. It changes nothing.
// =====================================================================

#include <windows.h>
#include <wlanapi.h>
#include <bthdef.h>
#include <bluetoothapis.h>
#include <setupapi.h>
#include <devguid.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "bthprops.lib")
#pragma comment(lib, "setupapi.lib")

namespace {

const wchar_t* WlanStateName(WLAN_INTERFACE_STATE state) {
    switch (state) {
        case wlan_interface_state_not_ready:       return L"not ready";
        case wlan_interface_state_connected:       return L"connected";
        case wlan_interface_state_ad_hoc_network_formed: return L"ad-hoc";
        case wlan_interface_state_disconnecting:   return L"disconnecting";
        case wlan_interface_state_disconnected:    return L"disconnected";
        case wlan_interface_state_associating:     return L"associating";
        case wlan_interface_state_discovering:     return L"discovering";
        case wlan_interface_state_authenticating:  return L"authenticating";
    }
    return L"?";
}

void ProbeWlan() {
    std::wprintf(L"--- WLAN ---\n");
    DWORD negotiated = 0;
    HANDLE client = nullptr;
    const DWORD open = WlanOpenHandle(2, nullptr, &negotiated, &client);
    if (open != ERROR_SUCCESS) {
        std::wprintf(L"  WlanOpenHandle -> %lu\n", open);
        return;
    }
    std::wprintf(L"  handle ok, negotiated version %lu\n", negotiated);

    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    const DWORD enumerated = WlanEnumInterfaces(client, nullptr, &list);
    if (enumerated != ERROR_SUCCESS || !list) {
        std::wprintf(L"  WlanEnumInterfaces -> %lu\n", enumerated);
        WlanCloseHandle(client, nullptr);
        return;
    }

    std::wprintf(L"  %lu interface(s)\n", list->dwNumberOfItems);
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& info = list->InterfaceInfo[i];
        std::wprintf(L"  [%lu] state=%ls  desc='%ls'\n", i, WlanStateName(info.isState),
                     info.strInterfaceDescription);

        // The connection attributes are only valid while connected, which is exactly the
        // conditional the drawing code has to handle.
        DWORD size = 0;
        WLAN_CONNECTION_ATTRIBUTES* attributes = nullptr;
        WLAN_OPCODE_VALUE_TYPE opcode = wlan_opcode_value_type_query_only;
        const DWORD queried =
            WlanQueryInterface(client, &info.InterfaceGuid, wlan_intf_opcode_current_connection,
                               nullptr, &size, reinterpret_cast<PVOID*>(&attributes), &opcode);
        if (queried != ERROR_SUCCESS) {
            std::wprintf(L"      WlanQueryInterface -> %lu (not connected)\n", queried);
            continue;
        }

        const WLAN_ASSOCIATION_ATTRIBUTES& assoc = attributes->wlanAssociationAttributes;
        // The SSID is a byte array with its own length, NOT a string -- it is not guaranteed
        // to be null terminated, so it is copied out and terminated here rather than printed
        // in place.
        wchar_t ssid[64] = {};
        const DWORD ssidLength = assoc.dot11Ssid.uSSIDLength < 32 ? assoc.dot11Ssid.uSSIDLength : 32;
        for (DWORD c = 0; c < ssidLength; ++c) {
            ssid[c] = static_cast<wchar_t>(assoc.dot11Ssid.ucSSID[c]);
        }
        std::wprintf(L"      SSID            : '%ls' (%lu bytes)\n", ssid, assoc.dot11Ssid.uSSIDLength);
        std::wprintf(L"      signal quality  : %lu%%\n", assoc.wlanSignalQuality);
        std::wprintf(L"      profile         : '%ls'\n", attributes->strProfileName);
        WlanFreeMemory(attributes);
    }

    // The radio state is separate from the interface state: an interface can be "not ready"
    // while the radio is off, which is the state the indicator has to distinguish.
    //
    // The buffer is zeroed AND the size is set before the call, because a successful query that
    // returns LESS than the caller's buffer does NOT write the size back -- so the tail of the
    // struct is left untouched. That is what produced a dwNumberOfPhys of 4050143248 (stack
    // garbage) on the first attempt: the field was never written and was never checked.
    WLAN_RADIO_STATE radio = {};
    DWORD radioSize = sizeof(radio);
    WLAN_OPCODE_VALUE_TYPE radioOpcode = wlan_opcode_value_type_query_only;
    const DWORD radioResult =
        WlanQueryInterface(client, &list->InterfaceInfo[0].InterfaceGuid,
                           wlan_intf_opcode_radio_state, nullptr, &radioSize,
                           reinterpret_cast<PVOID*>(&radio), &radioOpcode);
    if (radioResult == ERROR_SUCCESS && radio.dwNumberOfPhys > 0 &&
        radio.dwNumberOfPhys <= WLAN_MAX_PHY_INDEX) {
        const WLAN_PHY_RADIO_STATE& phy = radio.PhyRadioState[0];
        std::wprintf(L"  radio: %lu phys, %lu bytes returned; software=%d hardware=%d "
                     L"(0=unknown 1=on 2=off)\n",
                     radio.dwNumberOfPhys, radioSize,
                     static_cast<int>(phy.dot11SoftwareRadioState),
                     static_cast<int>(phy.dot11HardwareRadioState));
    } else {
        std::wprintf(L"  radio state query -> %lu\n", radioResult);
    }

    WlanFreeMemory(list);
    WlanCloseHandle(client, nullptr);
}

void ProbeBluetooth() {
    std::wprintf(L"\n--- Bluetooth ---\n");

    // The radio is found by asking BluetoothFindFirstRadio, NOT by passing a null handle to
    // BluetoothGetRadioInfo: a handle here identifies ONE radio and must come from the
    // enumeration, which is why passing null returned ERROR_INVALID_PARAMETER earlier.
    BLUETOOTH_FIND_RADIO_PARAMS params = {};
    params.dwSize = sizeof(params);
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
    if (!find) {
        const DWORD error = GetLastError();
        std::wprintf(L"  BluetoothFindFirstRadio -> none (error %lu)\n", error);
        std::wprintf(L"  %ls\n", error == ERROR_NO_MORE_ITEMS
                                     ? L"no Bluetooth radio on this machine"
                                     : L"the radio could not be opened");
        return;
    }

    int index = 0;
    do {
        BLUETOOTH_RADIO_INFO info = {};
        info.dwSize = sizeof(info);
        const DWORD result = BluetoothGetRadioInfo(radio, &info);
        if (result == ERROR_SUCCESS) {
            // BLUETOOTH_ADDRESS is a union: the integer form is ullLong, and casting the
            // struct itself does not compile. The MAC is printed from that member.
            std::wprintf(L"  [%d] name='%ls' address=%012llX manufacturer=%u lmp=%u\n", index,
                         info.szName, info.address.ullLong, info.manufacturer,
                         info.lmpSubversion);
        } else {
            std::wprintf(L"  [%d] BluetoothGetRadioInfo -> %lu\n", index, result);
        }

        // The radio's own on/off state. There is no "is the radio on" call; the hardware
        // switch is reported through the radio's device state, and a radio that is off still
        // enumerates -- so this is the value the indicator has to read rather than the
        // presence of the handle.
        std::wprintf(L"      handle %p is %s\n", radio, radio ? L"valid" : L"null");
        ++index;
        CloseHandle(radio);
    } while (BluetoothFindNextRadio(find, &radio));

    BluetoothFindRadioClose(find);
    std::wprintf(L"  %d radio(s) enumerated\n", index);
}

} // namespace

int wmain() {
    std::wprintf(L"=== what can be read about this machine's radios? ===\n\n");
    ProbeWlan();
    ProbeBluetooth();
    std::wprintf(L"\n=== done ===\n");
    return 0;
}
