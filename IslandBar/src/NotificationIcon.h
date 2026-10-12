// =====================================================================
//  NotificationIcon.h
//  Where a notification's icon comes from, and the pixels it becomes.
//
//  Its own file rather than a corner of main.cpp for one reason: both rules are decisions that can be WRONG
//  in ways the screen shows as "no icon" -- which is also what a machine with no icon looks like -- so they
//  have to be assertable from the self-test rather than taken on trust. main.cpp's frame loop cannot be
//  called from there; this can.
// =====================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Notification.h"

namespace island {

// The path a notification's icon should be loaded from.
//
// Two sources, in this order, and the order is the whole rule:
//   1. the `icon` field the sender named, if it named one;
//   2. otherwise the icon of THE PROCESS THAT SENT IT -- the pipe knows who connected, and a notification
//      from a tool that shows that tool's icon is what the shell's own toasts do.
//
// Returns false when neither exists, in which case the card draws its text alone. Never returns an empty
// success: a caller cannot tell "no icon" from "an empty path" by looking at the string.
bool NotificationIconSource(const Notification& notification, wchar_t* out, size_t capacity);

// Loads that path into premultiplied BGRA pixels -- the form the renderer takes.
//
// A `.png` is decoded as the picture it is; anything else (an executable, a shortcut, an .ico) is asked of
// the shell for the icon it stands for. Reading an executable as if it were a PNG -- or the other way round --
// produces an empty space with no error anywhere, which is why the two are separated here rather than
// guessed at one call site.
//
// Returns false when there is nothing usable, leaving `pixels` untouched rather than half-filled.
bool LoadNotificationIconPixels(const wchar_t* source, std::vector<uint8_t>* pixels, int* width,
                                int* height);

// Whether a notification's icon field is a path this process may hand to the SHELL.
//
// UNREACHABLE SINCE THE NOTIFICATION CARD WAS REMOVED. The sender's icon was only ever drawn on that card,
// so nothing calls this any more (see the note on CardOwner in Renderer.h). Kept, with its tests, because
// the rule it encodes is what 8.20 found and the next pass should either delete the file or re-wire it
// deliberately -- do not leave it looking live.
//
// The field comes from a third-party notification payload (see NotificationPipe: any local process can
// send one), so it is UNTRUSTED, and the icon loader used to pass anything that was not a `.png`
// straight to `SHGetFileInfoW`. Asking the shell about a string is asking it to RESOLVE that string, and
// for some strings resolving means activating something: measured, hovering the island while a Codex
// notification was up brought the Codex window forward -- "我有通知的时候 鼠标放在灵动岛上就会出现 不放
// 是不会出现的" -- because the notification's icon field was not a file path at all.
//
// Accepted, then: an ABSOLUTE path (drive or UNC), no characters a path cannot contain, and an extension
// from a short safe list. Everything else -- `ms-appx:///...`, `shell:AppsFolder\...`, `::{GUID}`,
// `OpenAI.Codex_...!App`, a relative name -- is refused, and the icon is simply not drawn.
bool IsPlainIconFilePath(const wchar_t* source);

} // namespace island
