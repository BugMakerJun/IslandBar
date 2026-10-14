# IslandBar

A taskbar replacement for Windows 11: a Dynamic-Island style bar that sits at the top
of the screen, carries the Start button, the open windows and a quick-settings panel,
and hides the shell's own taskbar.

Built with C++20, Direct2D and DirectComposition.

## What it looks like

```
[ system ][ Start ]   ( island )   [ app ] [ app ] [ app ] ...
                      ^
                      the time, or the current lyric while music plays
```

The system panel opens below its button, from under the bar:

```
[ v ][ # ]   ( island )   [ app ] [ app ] [ app ]
  +---------------------------------------------------+
  |  Wi-Fi      | Bluetooth    | Airplane              |
  |  Access.    | Battery saver| Live captions         |
  |  (volume)===========o                              |
  |                                              (gear)|
  +---------------------------------------------------+
```

## Current state

The bar is a borderless, always-on-top, non-activating strip spanning the monitor
width, drawn through a composition swap chain, with everything outside the shapes it
paints transparent to clicks.

**The island has ONE shape.** `Collapsed`, `Hover` and `MusicStrip` are all 220 x 36 --
"the island does not need to grow sideways any more" removed the per-state widths, so
moving the pointer over it changes nothing about its size. Only `Expanded`, the card,
is a different shape.

| State | Size (DIP) | Contents |
|---|---|---|
| Collapsed / Hover / MusicStrip | 220 x 36 | `hh:mm`, or the current lyric while music plays |
| Expanded | 520 x 250 | the system panel, a notification, or the music card |
| FullBar | monitor width x 32 | fills the reserved band when another window is fullscreen |

The window is created once at monitor width x 320 DIP and **is never resized**: every
transition moves and grows shapes inside it. Resizing a composition swap-chain window
flickers, so the alternative is to animate within a fixed canvas.

Two decks hang off the island -- the Start and system buttons on its left, the
application icons on its right -- and both are positioned from the **resting** pill,
not from the animated shape. That distinction matters: laying them out from a
card-sized box put the icon row inside the panel's own footprint, and centring a
520 DIP deck on a 220 DIP pill put the buttons under the card.

## Try it

```powershell
# build
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Config Debug -RunSmoke

# run it (this hides the shell taskbar; --keep-taskbar leaves it alone)
& .\build\Debug\src\Debug\island_bar.exe --keep-taskbar
```

Then: press the **chevron** on the left deck to open the quick-settings panel, press
**Start**, or click any **application icon**. Clicking the island or anywhere outside
the panel puts it away.

## Self-test

`island_bar.exe --self-test` runs **538 runtime assertions** covering monitor metrics,
the rounded-rectangle hit test, the JSON wire format (including adversarial and
malformed input), the notification store, the state machine and its easing, the layout
arithmetic for the decks and the card, the window's extended styles and DPI awareness,
the composition surface, the rendered pixels (read back from the GPU) and region-level
click-through. The exit code is the number of failures, so 0 is pass.

The geometry lives in pure functions over rectangles -- `Motion.cpp`,
`TaskStripLayout.h`, `CardLayout.h`, `StatePolicy.h`, `WindowList.h` -- precisely so it
can be asserted without a window, a GPU or a monitor.

## System panel

Six tiles, a volume slider and a settings gear, laid out the way the shell's own quick
settings are. Opened by the chevron, positioned under the button that opened it, and
closed by a click anywhere else.

The tiles open the matching `ms-settings:` page. They do **not** pretend to toggle:
Windows exposes no supported API for switching the WLAN radio, and a tile that *can*
read its state reports it honestly rather than showing a state it cannot change.

The glyphs come from `Segoe Fluent Icons`, and every codepoint was confirmed **by
rendering it** -- a missing glyph draws an empty box and reports no error, so the three
codepoints that were guessed first were all wrong.

## Notifications

The bar listens on a named pipe and shows what arrives as an expanded card. The push
channel is complete; reading Windows' own toast database (`wpndatabase.db`) is the
other half and is not done yet.

Send one from another process -- the bar must already be running:

```bat
"build\Debug\src\Debug\island_bar.exe" --notify "Build finished" --body "0 errors, 3 warnings" --app DSH
```

Optional: `--urgency low|normal|high`, `--sticky` (do not auto-dismiss).

Wire protocol, in full:

    request  := uint32 length (little endian) | `length` bytes of UTF-8 JSON
    response := the same framing, a UTF-8 JSON object

A length prefix rather than newline framing, because a body may contain newlines.
Fields: `app`, `title`, `body`, `icon`, `urgency`, `sticky`. A message with neither
`title` nor `body` is rejected rather than shown as a blank card.

The card opens when a notification arrives and stays for six seconds; a `--sticky` one
stays until acknowledged. Clicking the pill marks everything read and closes the card.

The pipe grants access to Everyone, deliberately: a named pipe is local-only, the worst
a hostile writer can do is display a notification, and the parser treats every field as
untrusted. See the comment on `PipeSecurity` in `NotificationPipe.cpp`.

## Music and lyrics

The island reads what is playing from the system media controls (SMTC, the same session
the volume flyout uses) through C++/WinRT. It works for anything that publishes a media
session -- Spotify, browsers, QQ Music, Media Player -- with no per-app work.

While something is playing, the island shows the current lyric scrolling on one line,
falling back to the track name when there are no lyrics. Clicking it opens the details
card: album art, title, `Playing - M83 - MusicApp`, three transport buttons and a
progress bar. The transport calls run on the polling thread, never the UI thread. A
track with no reported duration -- a live stream -- gets no progress bar at all rather
than an empty one, because an always-empty bar reads as a bug.

Lyrics are fetched from two public APIs in order -- NetEase, then QQ Music -- on a
worker thread, never the UI thread. Two sources because a network can block one and not
the other, and each reports its own reason for failing, so the log shows both. Anything
else that goes wrong (no lyrics, no search hit, a timeout, a changed response shape) is
treated as "no lyrics" and the strip falls back to the track name. That is a normal
outcome, not an error to report.

`--music-preview` cycles the music shapes with sample data, which is how the layout is
checked without a track actually playing. `--lyrics-test "<title>" ["<artist>"]`
fetches lyrics for one track and prints what came back, which is how the network path
is checked without starting the bar. Add `--lyrics-verbose` to also print the exact URL
requested and the start of each response, which is the only way to tell a 403 apart
from an empty result.

## Hiding the native taskbar

The bar hides the shell's taskbar on startup and always restores it on exit, since the
point of the project is to *be* the taskbar. `--keep-taskbar` leaves the native bar
alone, which is what you want while developing.

**This needs the process to run at the same integrity level as Explorer.** UIPI
silently drops window messages from a lower integrity level to a higher one, so inside
a sandbox that lowers the token the call returns 0 and nothing happens, with no error.
The program detects this and says so on startup instead of failing quietly. Check with:

```powershell
& .\build\Debug\src\Debug\island_bar.exe --probe-taskbar
```

If the taskbar is ever left hidden (the process was killed rather than closed):

```powershell
taskkill /f /im explorer.exe && start explorer.exe
```

## One instance at a time

A named mutex. Two copies draw two bars on top of each other, and the symptom does not
look like "two programs are running" -- it looks like **one bar drawn in the wrong
place**, because the second copy is somewhere the first one is not. A second launch
logs and exits immediately; it does not open a dialog, because a dialog blocks the
process, and a refused copy that lingers is the very state the guard exists to prevent.

## Build

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Config Debug -RunSmoke
```

Options:

| Flag | Meaning |
|---|---|
| `-Config Debug\|Release` | build configuration, default Debug |
| `-Clean` | delete the build directory first |
| `-RunSmoke` | run the smoke test after building |
| `-NoSmoke` | do not build the smoke test at all |

To get the MSVC environment in an interactive session:

```powershell
. .\scripts\env.ps1
cl
```

## Run

```powershell
# --- PowerShell: & is the CALL OPERATOR, required before a quoted path ---
& .\build\Debug\src\Debug\island_bar.exe --self-test
& .\build\Debug\src\Debug\island_bar.exe --probe-taskbar
& .\build\Debug\src\Debug\island_bar.exe --demo
& .\build\Debug\src\Debug\island_bar.exe
```

```bat
REM --- cmd.exe: & is a COMMAND SEPARATOR and cannot precede the path ---
"build\Debug\src\Debug\island_bar.exe" --self-test
start /wait "" "build\Debug\src\Debug\island_bar.exe" --probe-taskbar
```

Writing `& "path"` in cmd.exe fails with *"& was unexpected at this time"* and the
program never runs.

**To capture output, use `cmd /c` and a redirect, not a PowerShell redirect.** This is
a GUI-subsystem binary and borrows the parent console via `AttachConsole`, so it writes
to the console buffer rather than the inherited handle. From PowerShell,
`& .\island_bar.exe --self-test > log.txt` leaves a **0-byte file**:

```bat
cmd /c "build\Debug\src\Debug\island_bar.exe" --self-test > selftest.txt 2>&1
```

Flags:

| Flag | Meaning |
|---|---|
| `--keep-taskbar` | do not hide the shell taskbar |
| `--keep-strip` | do not release the strip of work area the shell taskbar reserves (a maximized window then keeps a band at the top of the screen) |
| `--self-test` | run the runtime assertions; exit code = failures |
| `--probe-taskbar` | may this process hide the shell taskbar? exit 0 = yes, 1 = blocked (UIPI), 2 = taskbar failed to restore |
| `--demo` | cycle collapsed/hover/expanded with no input needed |
| `--state <name>` | pin one shape, so a layout can be looked at |
| `--system-panel` | start with the quick-settings panel open |
| `--music-preview` | cycle the music shapes with sample data |
| `--lyrics-test <title> [artist]` | fetch lyrics for one track and report what came back |
| `--appbar` | claim the screen edge as an AppBar (may be refused) |
| `--notify` | push a notification into a running bar |

## Layout

```
CMakeLists.txt            the top-level project (C++20, /W4, UNICODE, static CRT)
CMakePresets.json         the same two configurations, one preset at a time
cmake/WinRt.cmake         the C++/WinRT setup the bar and two probes share
scripts/env.ps1           imports the MSVC environment into a session
scripts/build.ps1         configure + build + optional smoke run
scripts/snapshot.ps1      timestamped local snapshot of the source tree
docs/                     the handoff, the feature list, the verified screenshots

src/                      the bar -- one directory per module
  main.cpp                  entry: --self-test, --demo, or run the bar
  island_bar.manifest       PerMonitorV2 DPI awareness
  island_bar.rc             the version resource
  app/                      the window, and what hangs off it
    IslandWindow            window: topmost, no focus steal, pass-through, hover
    AppBar                  claiming the screen edge, and being refused
    PlayerWindow            the mini player that follows a window
  core/                     pure logic, and what everything shares
    Geometry.h              metrics + THE rounded-rect hit test (one copy, shared)
    Motion                  the island's states, their geometry and the interpolation
    StatePolicy.h           which shape the pointer and the panel ask for
    TaskStripLayout.h       the two decks: where every button goes, and its hit test
    CardLayout.h            the card's contents: tiles, sliders, the gear, the music card
    Log                     every console write goes through here
    MiniJson                the JSON reader behind the notification protocol
  platform/                 the Win32 and shell APIs the bar stands on
    Monitor                 work area, DPI, inferred taskbar thickness
    NativeTaskbar           hide/restore the shell taskbar (+ UIPI detection)
    WindowList              the open windows, in a stable order
    DisplayBrightness       DDC/CI, with every write read back
    GlassMaterial           the backdrop materials
    ThumbnailPreview        DWM live thumbnails
  media/                    what is playing, and what it looks like
    MusicSession            SMTC now-playing (C++/WinRT)
    Lyrics                  LRC parsing, and the search behind it
    SoundOutput             the mixer; AudioVolume, the master volume
    WicImage                image decode: album art, the blurred wash
  render/                   everything that puts pixels on the composition
    Renderer                D3D11 + Direct2D + DirectComposition + pixel readback
    TextRenderer            DirectWrite: the clock, the tiles, the icons
    Theme                   the colour sets
  shell/                    the shell's own controls, mirrored
    StartMenu, StartApps    the Start button and its pinned apps
    SystemStatus            WLAN, Bluetooth, volume, power, input method
    SystemToggles           switching radios and night light, and reading the result back
    Weather                 the reading on the status line
    TrayIcons, TrayPress    the tray, and pressing its icons
  services/                 the channels that bring things in
    Notification*           the model, the named pipe, the sender's icon
    ToastStore              Windows' own toast database
    WeChatWatch             the call and message watcher
  selftest/Diagnostics      the runtime self-test (-self-test)

tools/                    development probes; none of them is shipped
setup/                    the installer, with the bar embedded in it
```

## Notes for contributors

- **Keep `.ps1` files pure ASCII.** Windows PowerShell 5.1 reads scripts with the
  system ANSI codepage unless they carry a UTF-8 BOM, so non-ASCII literals get
  corrupted. Derive paths from `$PSCommandPath` instead of hardcoding them.
- Chinese literals in **C++** sources go in as `\uXXXX` escapes, never as literal
  UTF-8: this has already been silently re-encoded into mojibake by tooling once.
- Opacity must be driven through `IDCompositionEffectGroup`, not `IDCompositionVisual3`
  -- see HANDOFF.md section 6.3.
- Taskbar thickness is measured at runtime from the work-area delta; never hardcode it.
- The window is created once at its final size and is never resized. Resizing a
  composition swap-chain window flickers; animate inside it instead.
- The renderer, the hit test and the window's input REGION must all use the same
  layout function. Three times now a change moved one of them and not the others, and
  the symptom each time was different from the cause: a button that was drawn but
  unclickable, a panel clipped to a strip, icons that vanished.
- `FillRoundedRectangle` / `DrawRoundedRectangle` take an `ID2D1Brush`, not a colour.
  See HANDOFF.md 6.5 for the other API traps already hit.
- **Every drawing call, `Clear` included, must sit between `BeginDraw()` and
  `EndDraw()`.** D2D silently discards calls made outside that pair: the frame comes
  out blank while every HRESULT reports success. This exact mistake once produced an
  invisible pill behind a fully green 45-check self-test.
- Therefore never trust "the API returned S_OK" for anything visual. Assert on pixels
  -- `Renderer::ReadBackFrame` exists for that.
- The pill is **top-anchored**, and every state shares one top edge. Read the comment
  on `IslandAnchor` before changing it.
- Keep screen coordinates and window-local coordinates distinct. The anchor is
  expressed in window-local DIPs; subtracting the window origin there double-counts the
  offset and puts the island outside its own window, which is invisible on screen and
  only the self-test catches it.
- **Set a frame field BEFORE anything reads it.** `contentOpacity`, `pill.box`,
  `systemPanelOpen` and `outerCardUp` have each been assigned after their first reader
  at least once, and every time the symptom looked like a rendering fault rather than
  an ordering one.
- Diagnostics should report which step failed and with what HRESULT, not just
  true/false. `ReadBackFrameVerbose` exists because a bare bool left a real failure
  with no actionable information.
- When two related numbers are in doubt, print both on one line. Two wrong numbers side
  by side are obvious; either one alone looks like "it has not got there yet".

For the full cross-session record -- decisions, verified environment facts, API
findings, pitfalls and the roadmap, round by round in Chinese -- read `HANDOFF.md`.
`FEATURES.md` is the inventory, with an evidence level per feature.
