// =====================================================================
//  AudioVolume.cpp
// =====================================================================
#include "media/AudioVolume.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <cmath>

#include "core/Log.h"

#pragma comment(lib, "ole32.lib")

namespace island {
namespace {

// How long to wait between re-reads of the device volume.
//
// The device pushes notifications, but a COM callback into a worker thread needs the
// interface to be created there and the notifications to be pumped, and a missed notification
// would freeze the displayed level. A slow poll is simpler and cannot get stuck: half a second
// is invisible to a user who just pressed a volume key.
constexpr DWORD kPollIntervalMs = 500;

// How often the PEAK is read while the worker is awake. The meter the call strip draws has to move with a
// voice, and 500 ms would not: 30 Hz is a meter, 2 Hz is a slideshow.
constexpr DWORD kPeakIntervalMs = 33;

// A quiet step, matching one notch of the shell's own volume slider.
constexpr float kMinStep = 0.02f;

float Clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

} // namespace

AudioVolume::~AudioVolume() {
    Stop();
}

bool AudioVolume::Start() {
    if (running_) return true;

    InitializeCriticalSection(&lock_);
    lockReady_ = true;

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    writeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
    if (!stopEvent_ || !writeEvent_) return false;

    DWORD threadId = 0;
    thread_ = CreateThread(nullptr, 0, &AudioVolume::ThreadEntry, this, 0, &threadId);
    if (!thread_) return false;

    // Wait for the first read, so Start() returning true means the level is known and the
    // control can be drawn on the very first frame rather than popping in half a second later.
    for (int i = 0; i < 400 && !haveLevel_; ++i) {
        Sleep(5);
    }

    running_ = haveLevel_;
    if (!running_) {
        Stop();
        return false;
    }
    return true;
}

void AudioVolume::Stop() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    if (writeEvent_) {
        CloseHandle(writeEvent_);
        writeEvent_ = nullptr;
    }
    if (lockReady_) {
        DeleteCriticalSection(&lock_);
        lockReady_ = false;
    }
    running_ = false;
}

bool AudioVolume::Available() const {
    return haveLevel_;
}

bool AudioVolume::Get(float* level) const {
    if (!level || !lockReady_ || !haveLevel_) return false;
    EnterCriticalSection(&lock_);
    *level = level_;
    LeaveCriticalSection(&lock_);
    return true;
}

bool AudioVolume::IsMuted() const {
    if (!lockReady_) return false;
    EnterCriticalSection(&lock_);
    const bool muted = muted_;
    LeaveCriticalSection(&lock_);
    return muted;
}

bool AudioVolume::Peak(float* level) const {
    if (!level || !lockReady_ || !havePeak_) return false;
    EnterCriticalSection(&lock_);
    *level = peak_;
    LeaveCriticalSection(&lock_);
    return true;
}

unsigned long long AudioVolume::Revision() const {
    return static_cast<unsigned long long>(revision_);
}

void AudioVolume::Adjust(float delta) {
    float current = 0.0f;
    if (!Get(&current)) return;
    SetLevel(current + delta);
}

void AudioVolume::SetLevel(float level) {
    if (!lockReady_) return;

    const float wanted = Clamp01(level);

    // Publish locally first, so the bar moves on the next frame instead of waiting for the
    // worker to get round to it. The worker reconciles with the device afterwards.
    EnterCriticalSection(&lock_);
    level_ = wanted;
    haveLevel_ = true;
    LeaveCriticalSection(&lock_);
    InterlockedIncrement(&revision_);

    // The device write needs the same interfaces as the poll, so it is handed to the worker
    // through a shared request rather than done here on the caller's thread.
    EnterCriticalSection(&lock_);
    pendingLevel_ = wanted;
    pendingWrite_ = true;
    LeaveCriticalSection(&lock_);
    SetEvent(writeEvent_);
}

void AudioVolume::ToggleMute() {
    EnterCriticalSection(&lock_);
    pendingToggleMute_ = true;
    LeaveCriticalSection(&lock_);
    SetEvent(writeEvent_);
}

// The meter, and the endpoint it belongs to, kept between ticks.
//
// Acquired once and re-acquired only when it fails: the call strip's bars are drawn from this reading, and
// re-opening the endpoint thirty times a second to move four pixels would be the most expensive thing in the
// frame loop. The interfaces belong to this worker's apartment, so they are created and released here.
bool AudioVolume::ReadPeakFromDevice() {
    if (!meter_ || !meterDevice_) {
        if (meter_) {
            static_cast<IAudioMeterInformation*>(meter_)->Release();
            meter_ = nullptr;
        }
        if (meterDevice_) {
            static_cast<IMMDevice*>(meterDevice_)->Release();
            meterDevice_ = nullptr;
        }

        IMMDeviceEnumerator* enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator))) ||
            !enumerator) {
            return false;
        }
        IMMDevice* device = nullptr;
        const HRESULT got = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(got) || !device) return false;

        IAudioMeterInformation* meter = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&meter))) ||
            !meter) {
            device->Release();
            return false;
        }
        meter_ = meter;
        meterDevice_ = device;
        LogLine(L"  volume: peak meter acquired");
    }

    float peak = 0.0f;
    if (FAILED(static_cast<IAudioMeterInformation*>(meter_)->GetPeakValue(&peak))) return false;
    if (peak < 0.0f) peak = 0.0f;
    if (peak > 1.0f) peak = 1.0f;

    EnterCriticalSection(&lock_);
    peak_ = peak;
    havePeak_ = true;
    LeaveCriticalSection(&lock_);
    return true;
}

bool AudioVolume::ReadFromDevice() {
    // Enumeration and activation on every read would be wasteful, but the endpoint can change
    // under us (a headset plugged in, the default device switched), so it is re-acquired rather
    // than cached forever. This runs on the worker, never the frame loop.
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return false;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device) return false;

    IAudioEndpointVolume* volume = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&volume));
    device->Release();
    if (FAILED(hr) || !volume) return false;

    float level = 0.0f;
    BOOL muted = FALSE;
    const bool ok = SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level)) &&
                    SUCCEEDED(volume->GetMute(&muted));

    if (ok) {
        EnterCriticalSection(&lock_);
        // Only accept the device's value when the user is not mid-adjustment: an in-flight
        // click would otherwise be overwritten by the level the device still reports, and the
        // bar would snap back.
        if (!pendingWrite_) {
            level_ = Clamp01(level);
            muted_ = muted != FALSE;
        }
        haveLevel_ = true;
        LeaveCriticalSection(&lock_);
    }

    volume->Release();
    return ok;
}

unsigned long __stdcall AudioVolume::ThreadEntry(void* self) {
    static_cast<AudioVolume*>(self)->Run();
    return 0;
}

void AudioVolume::Run() {
    // This thread owns its own apartment. `IAudioEndpointVolume` is created here and used
    // here, so it never crosses into the frame loop's apartment.
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool haveApartment = SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE;

    unsigned long long lastVolumeRead_ = GetTickCount64();
    if (haveApartment) ReadFromDevice();

    while (true) {
        HANDLE waits[2] = {stopEvent_, writeEvent_};
        // The wait is the PEAK interval, not the volume's: the loop wakes often and does the cheap peak read
        // every time, while the volume re-read (which opens the device) only happens on its own slower clock.
        const DWORD signalled = WaitForMultipleObjects(2, waits, FALSE, kPeakIntervalMs);
        if (signalled == WAIT_OBJECT_0) break;   // stop

        if (!haveApartment) break;

        const bool writeRequested = (signalled == WAIT_OBJECT_0 + 1);

        if (writeRequested) {
            float wanted = 0.0f;
            bool doWrite = false;
            bool doToggleMute = false;

            EnterCriticalSection(&lock_);
            if (pendingWrite_) {
                wanted = pendingLevel_;
                pendingWrite_ = false;
                doWrite = true;
            }
            if (pendingToggleMute_) {
                pendingToggleMute_ = false;
                doToggleMute = true;
            }
            LeaveCriticalSection(&lock_);

            if (doWrite || doToggleMute) {
                IMMDeviceEnumerator* enumerator = nullptr;
                if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                               CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                               reinterpret_cast<void**>(&enumerator))) &&
                    enumerator) {
                    IMMDevice* device = nullptr;
                    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                                      &device)) &&
                        device) {
                        IAudioEndpointVolume* volume = nullptr;
                        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume),
                                                       CLSCTX_ALL, nullptr,
                                                       reinterpret_cast<void**>(&volume))) &&
                            volume) {
                            if (doWrite) {
                                volume->SetMasterVolumeLevelScalar(wanted, nullptr);
                            }
                            if (doToggleMute) {
                                BOOL muted = FALSE;
                                if (SUCCEEDED(volume->GetMute(&muted))) {
                                    volume->SetMute(!muted, nullptr);
                                }
                            }
                            volume->Release();
                        }
                        device->Release();
                    }
                    enumerator->Release();
                }
            }
        }

        // The PEAK every tick, which is one call on an interface this worker already owns; the VOLUME on its
        // own slower clock, because that one opens the device and walking it thirty times a second to catch a
        // change that happens when somebody presses a volume key would be work for nothing.
        ReadPeakFromDevice();
        if (GetTickCount64() - lastVolumeRead_ >= kPollIntervalMs) {
            lastVolumeRead_ = GetTickCount64();
            // Re-read afterwards, so a change made elsewhere (the keyboard, the shell flyout) still shows up.
            // The read is skipped while a write is pending, to avoid fighting the user.
            if (ReadFromDevice()) {
                InterlockedIncrement(&revision_);
            }
        }
    }

    if (haveApartment) CoUninitialize();
    running_ = false;
}

} // namespace island
