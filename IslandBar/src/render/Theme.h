// =====================================================================
//  Theme.h
//  The material, as data: the colours and edges the bar is drawn from, in a list the user can
//  switch between -- and can add to, by dropping a text file in `themes\`.
//
//  WHERE THIS CAME FROM. A review brief asked for "Liquid Glass": a translucent dark surface, a soft
//  highlight along the top, a hairline edge, no heavy borders -- and for the palette to be black,
//  deep blue and ice blue. Everything it asks for is drawing, and all of that drawing reads a handful
//  of colours, so the colours are one struct rather than a style pass: switching a theme re-colours
//  every surface at once and cannot leave two of them disagreeing.
//
//  WHAT A THEME DELIBERATELY DOES NOT CONTROL: whether the desktop behind the bar is BLURRED. That is
//  not a colour question -- it is whether this window can have a backdrop at all, and it is measured
//  in scripts/probe-glass-blur.ps1 (acrylic does blur behind the window, but across its whole
//  rectangle, so the window would have to be shaped to what is drawn first). A theme that claimed
//  "glass" while the backdrop stayed sharp would be the kind of half-truth this project avoids.
//
//  THE CONTRAST FLOOR IS PART OF THE FORMAT, not a note in a document: 'no over-transparency that
//  makes the text unreadable' is checked by ThemeInkContrast, and the self-test runs it over every
//  built-in theme. A user theme is loaded whatever it says -- it is the user's screen -- but the
//  number is in the log when it is below the floor.
// =====================================================================
#pragma once

#include <windows.h>

#include <d2d1_3.h>

#include <string>

namespace island {

struct Theme {
    wchar_t name[32] = {};
    // The bar's own surface. Two stops rather than one, because a flat fill at 92% is what the bar
    // had and it reads as a black rectangle: the plan's "reflected light" is what the second stop and
    // the gloss below are for.
    D2D1_COLOR_F pillTop = {};
    D2D1_COLOR_F pillBottom = {};
    float gloss = 0.0f;             // 0..1: strength of the soft highlight along the pill's top
    // The cards -- the system panel, the tray page, the music card, a notification. One surface for
    // all of them, which is what makes them read as the same object at different sizes.
    D2D1_COLOR_F cardFill = {};
    float cardEdge = 0.0f;          // 0..1: the hairline around a card, as an alpha of the ink
    D2D1_COLOR_F accent = {};       // "on" tiles, slider fills, the meters
    D2D1_COLOR_F ink = {};          // text and glyphs -- the colour everything is written in
    // The two layers of the material that are neither colour nor edge (GlassMaterial.h): how far the soft
    // shadow reaches under the shape, and how strong the grain over it is. Both are 0 in 默认, so the
    // built-in that existed before themes keeps its exact pixels -- a theme opts into a material.
    float shadow = 0.0f;            // 0..1: opacity of the shadow at its darkest
    float grain = 0.0f;             // 0..1: the grain's own alpha
};

// The theme in force.
//
// Process-wide, and that is a deliberate simplification: the material is one decision for the whole
// bar, every drawing call reads it, none of them owns it, and threading it through the renderer's
// interfaces would add a parameter to twenty signatures to express a value that cannot differ
// between them.
const Theme& CurrentTheme();

// The list the 外观 tile walks: the built-ins first, then whatever `themes\*.txt` holds.
int ThemeCount();
const Theme& ThemeAt(int index);
int CurrentThemeIndex();

// Applies the next theme in that list and returns it (wrapping at the end).
const Theme& CycleTheme();

// Where the choice is remembered: `<exe dir>\theme.txt`, the same place and the same idea as
// weather.txt. An unknown name leaves the current theme alone and says so.
bool SaveThemeChoice(const wchar_t* name);
void ApplyRememberedTheme();

// The ink at an alpha, which is what most of the drawing asks for: a hundred call sites say "white at
// 0.72" and the theme is what decides which colour that is.
D2D1_COLOR_F ThemeInk(float alpha);

// WCAG contrast between the ink and the card, as it would appear over a BRIGHT desktop -- the worst
// case for light text on a translucent surface, and the one the plan's "do not over-transparent the
// background" is about.
float ThemeInkContrast(const Theme& theme);

// Reads a theme from the text of a `themes\*.txt`. PURE, so the format is testable without files:
// one `key=value` per line, `#` for comments, components 0..1 and either three (RGB) or four (RGBA).
bool ThemeFromText(const char* utf8, Theme* out, std::string* why);

// The names of the built-in themes, for the self-test and the log.
int BuiltInThemeCount();
const Theme& BuiltInTheme(int index);

} // namespace island
