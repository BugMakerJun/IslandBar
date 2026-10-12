// =====================================================================
//  AudioVolume.h
//  The system output volume, and changing it.
//
//  WASAPI's endpoint volume, not an audio session's. The endpoint is the master output, which
//  is what the volume flyout in the reference image controls and what a user expects a volume
//  control to move; a per-application session would only adjust whichever process happened to
//  own the audio, and would do nothing at all when nothing is playing.
//
//  Two details that decide whether this is usable:
//
//   * The callbacks. `IAudioEndpointVolume` is a COM object that can call back into the thread
//     that created it, and it is created on a worker rather than the frame loop, because
//     `IMMDeviceEnumerator` touches the audio service and can block for tens of milliseconds
//     on a machine whose audio stack is waking up. Blocking the frame loop is exactly what the
//     stutter reports were about.
//   * The notifications. The volume can be changed by the keyboard, the shell flyout, or a
//     headset button, so the value has to be pushed rather than polled.
// =====================================================================
#pragma once

#include <windows.h>

namespace island {

class AudioVolume {
public:
    AudioVolume() = default;
    ~AudioVolume();

    AudioVolume(const AudioVolume&) = delete;
    AudioVolume& operator=(const AudioVolume&) = delete;

    // Starts the worker and reads the current volume. Returns false when the audio endpoint
    // is unavailable, in which case the control is simply not drawn.
    bool Start();
    void Stop();

    // True once a volume has been read. Until then there is nothing to show.
    bool Available() const;

    // The current volume, 0..1. Returns false when unknown.
    bool Get(float* level) const;

    // True when the output is muted.
    bool IsMuted() const;

    // Steps the volume up or down by `delta` (0.02 is a sensible click step).
    //
    // Applied locally and pushed to the device, then reconciled with the device's own report,
    // so dragging or clicking feels immediate instead of waiting for a round trip.
    void Adjust(float delta);

    // Sets it outright, for a click on the bar.
    void SetLevel(float level);

    // The output's PEAK level, 0..1, as the audio engine measures it -- the reading a level meter moves to.
    //
    // It is a separate value from the master volume and is not a substitute for it: the volume is where the
    // slider is, the peak is how loud the sound coming out is right now. The call strip's bars are drawn from
    // this, which is why they move when someone talks and sit still when nobody does.
    //
    // Polled fast (about 30 Hz) on the worker, because a meter sampled once a frame at 60 Hz would be wasted
    // work and one sampled at the volume's 500 ms would not move at all.
    bool Peak(float* level) const;

    void ToggleMute();

    // Bumped whenever the level or mute state changes, so the frame loop knows to redraw.
    unsigned long long Revision() const;

private:
    static unsigned long __stdcall ThreadEntry(void* self);
    void Run();

    // Reads the device's current level into the shared state.
    bool ReadFromDevice();
    // Reads the meter's peak, acquiring the meter once and keeping it: a meter that re-opened the endpoint
    // thirty times a second would cost more than everything else this class does put together.
    bool ReadPeakFromDevice();

    HANDLE        stopEvent_ = nullptr;
    // Auto-reset: a write request wakes the worker without a full poll interval of latency.
    HANDLE        writeEvent_ = nullptr;
    HANDLE        thread_ = nullptr;
    volatile bool running_ = false;

    mutable CRITICAL_SECTION lock_{};
    bool lockReady_ = false;

    float          level_ = 0.0f;
    bool           muted_ = false;
    bool           haveLevel_ = false;
    volatile long  revision_ = 0;
    // The peak, and whether it has ever been read. Written by the worker under the same lock as the level.
    float          peak_ = 0.0f;
    bool           havePeak_ = false;

    // A write the frame loop asked for, handed to the worker because the device interfaces
    // belong to the worker's apartment.
    float          pendingLevel_ = 0.0f;
    bool           pendingWrite_ = false;
    bool           pendingToggleMute_ = false;

    // The meter, owned by the worker. void* because this header deliberately knows nothing but windows.h --
    // the same arrangement the WLAN handle uses in SystemStatus.
    void*          meterDevice_ = nullptr;   // IMMDevice*
    void*          meter_ = nullptr;         // IAudioMeterInformation*
};

} // namespace island
