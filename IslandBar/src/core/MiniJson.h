// =====================================================================
//  MiniJson.h
//  A very small JSON reader, for exactly one job: pulling a handful of flat
//  string and number fields out of a notification message.
//
//  Why not a real parser: the payloads are short, flat, and authored by the same
//  tool that reads them. A dependency-free extractor keeps the build simple and
//  the behaviour inspectable.
//
//  Why it is written defensively anyway: a JSON reader that trusts its input is a
//  remote code execution bug waiting to happen, and this one listens on a named
//  pipe that any local process can write to. Every function here is a pure
//  function of its inputs, so the self-test can hammer it with malformed text
//  without needing a server, a client, or a pipe.
// =====================================================================
#pragma once

#include <cstddef>

namespace island {
namespace json {

// Escapes a UTF-8 string for embedding in a JSON string literal. Control
// characters become \uXXXX, and the two mandatory escapes are applied. Returns
// the number of characters written, excluding the terminator.
size_t Escape(const char* text, char* out, size_t capacity);

// Unescapes the inside of a JSON string literal into UTF-8.
//
// Handles \" \\ \/ \b \f \n \r \t and \uXXXX, including surrogate pairs. A lone
// surrogate is not an error in practice -- the payload may be truncated -- so it
// becomes U+FFFD rather than a failure. Returns the number of bytes written,
// excluding the terminator.
size_t Unescape(const char* text, size_t length, char* out, size_t capacity);

// Reads the string value of a top-level `"key"` into `out`.
//
// `out` receives the decoded text, at most `capacity` bytes including the
// terminator. Returns true when the key was found and its value was a string.
bool GetString(const char* document, const char* key, char* out, size_t capacity);

// Reads a top-level integer value. Returns false when the key is missing or the
// value is not a number.
bool GetInt(const char* document, const char* key, long long* out);

// True when `key` exists at the top level with a boolean or null value.
bool HasKey(const char* document, const char* key);

} // namespace json
} // namespace island
