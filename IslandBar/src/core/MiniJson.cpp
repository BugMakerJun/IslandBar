// =====================================================================
//  MiniJson.cpp
// =====================================================================
#include "core/MiniJson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace island {
namespace json {
namespace {

// Emitted only when a code point cannot be encoded at all. Valid input never
// reaches it: a lone surrogate becomes U+FFFD through the normal path, not this.
constexpr char kReplacement = '?';

// Encodes one code point as UTF-8. Returns the number of bytes, or 0 when the
// code point cannot be represented (a lone surrogate, or out of range).
size_t EncodeUtf8(unsigned int codePoint, char* out, size_t remaining) {
    if (codePoint == 0) return 0;
    if (codePoint < 0x80) {
        if (remaining < 1) return 0;
        out[0] = static_cast<char>(codePoint);
        return 1;
    }
    if (codePoint < 0x800) {
        if (remaining < 2) return 0;
        out[0] = static_cast<char>(0xC0 | (codePoint >> 6));
        out[1] = static_cast<char>(0x80 | (codePoint & 0x3F));
        return 2;
    }
    if (codePoint < 0x10000) {
        if (remaining < 3) return 0;
        out[0] = static_cast<char>(0xE0 | (codePoint >> 12));
        out[1] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (codePoint & 0x3F));
        return 3;
    }
    if (codePoint <= 0x10FFFF) {
        if (remaining < 4) return 0;
        out[0] = static_cast<char>(0xF0 | (codePoint >> 18));
        out[1] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (codePoint & 0x3F));
        return 4;
    }
    return 0;
}

bool IsHighSurrogate(unsigned int v) { return v >= 0xD800 && v <= 0xDBFF; }
bool IsLowSurrogate(unsigned int v)  { return v >= 0xDC00 && v <= 0xDFFF; }

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ReadHex4(const char* text, unsigned int* value) {
    unsigned int result = 0;
    for (int i = 0; i < 4; ++i) {
        const int digit = HexDigit(text[i]);
        if (digit < 0) return false;
        result = (result << 4) | static_cast<unsigned int>(digit);
    }
    *value = result;
    return true;
}

// Skips a JSON string starting at text[0] == '"'. Sets `end` to the index one past
// the closing quote. Returns false when the string is unterminated.
bool SkipString(const char* text, size_t* end) {
    size_t i = 1;
    while (text[i] != '\0') {
        if (text[i] == '\\') {
            if (text[i + 1] == '\0') return false;   // trailing backslash
            i += 2;
            continue;
        }
        if (text[i] == '"') {
            *end = i + 1;
            return true;
        }
        ++i;
    }
    return false;
}

void SkipWhitespace(const char* text, size_t* i) {
    while (text[*i] == ' ' || text[*i] == '\t' || text[*i] == '\n' || text[*i] == '\r') {
        ++(*i);
    }
}

enum class ValueType { String, Number, Other };

// Finds the value of a top-level key.
//
// Only depth-1 keys are considered, and string contents are skipped rather than
// scanned for the key text. Both matter: the document is written by another
// process, and a naive strstr would happily match a key inside a nested object, or
// inside a message body that happens to contain the word "title".
bool FindTopLevelValue(const char* document, const char* key, size_t* valueStart,
                       ValueType* type) {
    if (!document || !key) return false;

    const size_t keyLength = std::strlen(key);
    int depth = 0;
    size_t i = 0;

    while (document[i] != '\0') {
        const char c = document[i];

        if (c == '"') {
            // SkipString scans the string that starts at the pointer it is given,
            // and reports a length relative to that pointer. Passing `document`
            // instead of `document + i` made it re-scan the FIRST string in the
            // document every time, so `end` pointed behind `i` and the loop
            // ping-ponged forever on the first quoted token.
            size_t tokenLength = 0;
            if (!SkipString(document + i, &tokenLength)) return false;

            // Only a quoted token sitting directly inside the root object can be a
            // key; deeper quoting is a nested object or an array element.
            if (depth == 1) {
                // tokenLength counts both quotes, so the body is length - 2.
                const size_t bodyLength = (tokenLength >= 2) ? tokenLength - 2 : 0;
                if (bodyLength == keyLength &&
                    std::strncmp(document + i + 1, key, keyLength) == 0) {
                    size_t j = i + tokenLength;
                    SkipWhitespace(document, &j);
                    if (document[j] == ':') {
                        ++j;
                        SkipWhitespace(document, &j);
                        *valueStart = j;
                        if (document[j] == '"') {
                            *type = ValueType::String;
                        } else if (document[j] == '-' ||
                                   (document[j] >= '0' && document[j] <= '9')) {
                            *type = ValueType::Number;
                        } else {
                            *type = ValueType::Other;
                        }
                        return true;
                    }
                }
            }

            // Always move past the token; this branch leaves the loop early, so the
            // advance at the bottom would otherwise be skipped.
            i += tokenLength;
            continue;
        }

        if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
            if (depth < 0) return false;   // malformed: more closers than openers
        }
        ++i;
    }
    return false;
}

} // namespace

size_t Escape(const char* text, char* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    out[0] = '\0';
    if (!text) return 0;

    size_t written = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const char* replacement = nullptr;
        char unicodeEscape[8] = {};

        switch (c) {
            case '"':  replacement = "\\\""; break;
            case '\\': replacement = "\\\\"; break;
            case '\b': replacement = "\\b";  break;
            case '\f': replacement = "\\f";  break;
            case '\n': replacement = "\\n";  break;
            case '\r': replacement = "\\r";  break;
            case '\t': replacement = "\\t";  break;
            default: break;
        }

        if (!replacement && c < 0x20) {
            // Control characters have no short escape and would make the document
            // invalid if written raw.
            std::snprintf(unicodeEscape, sizeof(unicodeEscape), "\\u%04X", c);
            replacement = unicodeEscape;
        }

        const char single = static_cast<char>(c);
        const char* source = replacement ? replacement : &single;
        const size_t length = replacement ? std::strlen(replacement) : 1;

        if (written + length + 1 > capacity) break;   // keep room for the terminator
        std::memcpy(out + written, source, length);
        written += length;
    }

    out[written] = '\0';
    return written;
}

size_t Unescape(const char* text, size_t length, char* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    out[0] = '\0';
    if (!text) return 0;

    // An empty body must return before touching text[0]: Unescape is called with a
    // pointer just past the opening quote, and for "" that pointer is the closing
    // quote itself, so indexing it would read the wrong byte.
    if (length == 0) return 0;

    size_t written = 0;
    size_t i = 0;
    bool overflowed = false;

    // Writes one byte if there is room. Sets overflowed once when there is not, so
    // the loop can stop cleanly instead of scattering early exits through the
    // escape switch.
    const auto put = [&](char value) {
        if (overflowed) return;
        if (written + 1 >= capacity) {
            overflowed = true;
            return;
        }
        out[written++] = value;
    };

    const auto putCodePoint = [&](unsigned int cp) {
        char encoded[4] = {};
        const size_t n = EncodeUtf8(cp, encoded, sizeof(encoded));
        if (n == 0) {
            put(kReplacement);
            return;
        }
        for (size_t k = 0; k < n; ++k) {
            put(encoded[k]);
        }
    };

    while (i < length && !overflowed) {
        const char c = text[i];

        if (c != '\\') {
            put(c);
            ++i;
            continue;
        }

        // A trailing backslash cannot be interpreted; keep it verbatim.
        if (i + 1 >= length) {
            put(c);
            ++i;
            continue;
        }

        const char esc = text[i + 1];
        switch (esc) {
            case '"':  put('"');  i += 2; break;
            case '\\': put('\\'); i += 2; break;
            case '/':  put('/');  i += 2; break;
            case 'b':  put('\b'); i += 2; break;
            case 'f':  put('\f'); i += 2; break;
            case 'n':  put('\n'); i += 2; break;
            case 'r':  put('\r'); i += 2; break;
            case 't':  put('\t'); i += 2; break;
            case 'u': {
                unsigned int code = 0;
                if (i + 6 > length || !ReadHex4(text + i + 2, &code)) {
                    // Not a valid \u escape: keep the backslash and move on, so a
                    // malformed message degrades instead of disappearing.
                    put(c);
                    ++i;
                    break;
                }
                i += 6;

                if (IsHighSurrogate(code)) {
                    // A surrogate pair is two escapes; a lone high surrogate
                    // becomes the replacement character.
                    unsigned int low = 0;
                    if (i + 6 <= length && text[i] == '\\' && text[i + 1] == 'u' &&
                        ReadHex4(text + i + 2, &low) && IsLowSurrogate(low)) {
                        putCodePoint(0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00));
                        i += 6;
                    } else {
                        putCodePoint(0xFFFD);
                    }
                    break;
                }
                if (IsLowSurrogate(code)) {
                    putCodePoint(0xFFFD);
                    break;
                }
                putCodePoint(code);
                break;
            }
            default:
                // Unknown escape: emit the character after the backslash, which is
                // what every tolerant reader does.
                put(esc);
                i += 2;
                break;
        }
    }

    out[written] = '\0';
    return written;
}

bool GetString(const char* document, const char* key, char* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = '\0';

    size_t start = 0;
    ValueType type = ValueType::Other;
    if (!FindTopLevelValue(document, key, &start, &type)) return false;
    if (type != ValueType::String) return false;

    size_t end = 0;
    if (!SkipString(document + start, &end)) return false;

    // end counts the closing quote, so the body is [1, end - 1).
    const size_t bodyLength = (end >= 2) ? end - 2 : 0;
    Unescape(document + start + 1, bodyLength, out, capacity);
    return true;
}

bool GetInt(const char* document, const char* key, long long* out) {
    size_t start = 0;
    ValueType type = ValueType::Other;
    if (!FindTopLevelValue(document, key, &start, &type)) return false;
    if (type != ValueType::Number) return false;

    // strtoll stops at the first non-numeric byte, so a trailing comma or brace
    // needs no special handling; a fraction simply truncates.
    char* endPointer = nullptr;
    const long long value = std::strtoll(document + start, &endPointer, 10);
    if (endPointer == document + start) return false;   // no digits consumed
    if (out) *out = value;
    return true;
}

bool HasKey(const char* document, const char* key) {
    size_t start = 0;
    ValueType type = ValueType::Other;
    return FindTopLevelValue(document, key, &start, &type);
}

} // namespace json
} // namespace island
