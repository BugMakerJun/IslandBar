// =====================================================================
//  tools/jsonprobe/main.cpp
//  A standalone harness for the JSON reader and the notification parser.
//
//  This exists because a hang inside the main self-test could come from the
//  parser, the notification layer, or the DirectComposition setup around them, and
//  bisecting that by rebuilding the whole bar is slow. This binary links only the
//  two modules under suspicion, so an infinite loop is unambiguous and a debugger
//  attaches instantly.
//
//  It is not part of the shipped bar.
// =====================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../../src/MiniJson.h"
#include "../../src/Notification.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const char* detail = "") {
    std::printf("%s %-46s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail);
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

} // namespace

int main() {
    std::printf("jsonprobe: MiniJson + Notification\n");
    std::printf("-------------------------------------------------\n");

    char value[256] = {};

    std::printf("-- step 1: GetString on a flat document --\n");
    std::fflush(stdout);
    const char* simple = R"({"app":"dsh","title":"Build finished","body":"3 warnings"})";
    const bool got = island::json::GetString(simple, "app", value, sizeof(value));
    Check(got && std::strcmp(value, "dsh") == 0, "reads a flat string", value);

    std::printf("-- step 2: missing key --\n");
    std::fflush(stdout);
    Check(!island::json::GetString(simple, "missing", value, sizeof(value)),
          "missing key reports false");

    std::printf("-- step 3: small buffer --\n");
    std::fflush(stdout);
    Check(island::json::GetString(simple, "app", value, 3) && std::strcmp(value, "ds") == 0,
          "truncates into a small buffer", value);

    std::printf("-- step 4: whitespace --\n");
    std::fflush(stdout);
    const char* spaced = "{ \"title\" : \"x\" , \"n\" : 42 }";
    Check(island::json::GetString(spaced, "title", value, sizeof(value)) &&
              std::strcmp(value, "x") == 0,
          "whitespace around key and colon", value);

    std::printf("-- step 5: integers --\n");
    std::fflush(stdout);
    long long n = 0;
    Check(island::json::GetInt(spaced, "n", &n) && n == 42, "reads an integer");

    std::printf("-- step 6: nested key --\n");
    std::fflush(stdout);
    const char* nested = R"({"outer":{"title":"WRONG"},"title":"RIGHT"})";
    Check(island::json::GetString(nested, "title", value, sizeof(value)) &&
              std::strcmp(value, "RIGHT") == 0,
          "ignores a nested key", value);

    std::printf("-- step 7: key inside a value --\n");
    std::fflush(stdout);
    const char* inValue = R"({"body":"the word \"title\":\"nope\" is inside me"})";
    Check(!island::json::GetString(inValue, "title", value, sizeof(value)),
          "no match inside another value");

    std::printf("-- step 8: array element key --\n");
    std::fflush(stdout);
    const char* array = R"({"list":[{"title":"WRONG"}],"title":"RIGHT"})";
    Check(island::json::GetString(array, "title", value, sizeof(value)) &&
              std::strcmp(value, "RIGHT") == 0,
          "ignores a key inside an array", value);

    std::printf("-- step 9: escapes --\n");
    std::fflush(stdout);
    const char* escaped = R"({"title":"a\"b\\c\nd\te"})";
    Check(island::json::GetString(escaped, "title", value, sizeof(value)) &&
              std::strcmp(value, "a\"b\\c\nd\te") == 0,
          "decodes escapes", value);

    std::printf("-- step 10: \\u escape --\n");
    std::fflush(stdout);
    const char* unicode = R"({"title":"\u4e2d\u6587"})";
    Check(island::json::GetString(unicode, "title", value, sizeof(value)) &&
              std::strcmp(value, "\xe4\xb8\xad\xe6\x96\x87") == 0,
          "decodes \\uXXXX", value);

    std::printf("-- step 11: malformed documents --\n");
    std::fflush(stdout);
    const char* cases[] = {
        "", "{", "}", "{\"title\"", "{\"title\":}", "{\"title\":\"unterminated",
        "[]", "null", "{\"title\":\"ok\"}garbage",
    };
    for (int i = 0; i < static_cast<int>(sizeof(cases) / sizeof(cases[0])); ++i) {
        std::printf("   case %d: '%s'\n", i, cases[i]);
        std::fflush(stdout);
        char scratch[256] = {};
        (void)island::json::GetString(cases[i], "title", scratch, sizeof(scratch));
        (void)island::json::HasKey(cases[i], "title");
        long long x = 0;
        (void)island::json::GetInt(cases[i], "title", &x);
    }
    Check(true, "malformed documents returned");

    std::printf("-- step 12: notification parsing --\n");
    std::fflush(stdout);
    island::Notification parsed;
    Check(island::NotificationFromJson(R"({"app":"dsh","title":"Build ok","body":"clean"})",
                                      &parsed),
          "parses a message", parsed.title);
    Check(!island::NotificationFromJson(R"({"app":"dsh"})", &parsed),
          "rejects an empty message");

    std::printf("-- step 13: store push --\n");
    std::fflush(stdout);
    island::NotificationStore store;
    const island::Notification* pushed = store.Push(parsed);
    Check(pushed != nullptr && pushed->id == 1, "push assigns id 1");
    Check(store.Count() == 1 && store.UnreadCount() == 1, "counts as unread");

    std::printf("-- step 14: store overflow --\n");
    std::fflush(stdout);
    island::Notification item;
    for (int i = 0; i < island::kMaxNotifications + 10; ++i) {
        std::snprintf(item.title, island::kTitleCapacity, "item %d", i);
        store.Push(item);
    }
    Check(store.Count() == island::kMaxNotifications, "capacity respected");

    std::printf("-- step 15: mark read and clear --\n");
    std::fflush(stdout);
    (void)store.MarkAllRead();
    Check(store.UnreadCount() == 0, "nothing unread");
    store.Clear();
    Check(store.Count() == 0, "cleared");

    std::printf("-------------------------------------------------\n");
    std::printf("RESULT: %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures;
}
