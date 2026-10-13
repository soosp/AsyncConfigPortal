// Host test suite for detail/result_json.h (the answer to a fetch() post).
//
//   make result     build and run

#include <cstdio>
#include <string>
#include "JsonReadUtils.h"
#include "detail/result_json.h"

using acp_detail::buildResultJson;
using acp_detail::RESULT_JSON_FRAME;

static int g_fail = 0;
static int g_run  = 0;

static void check(bool ok, const char* what, int line, const char* got = nullptr) {
    g_run++;
    if (!ok) {
        g_fail++;
        printf("  FAIL line %d: %s%s%s\n", line, what, got ? " -> " : "", got ? got : "");
    }
}
#define CHECK(c, what) check((c), (what), __LINE__)
#define CHECK_IS(buf, want) check(std::string(buf) == (want), want, __LINE__, buf)

// The bound AsyncConfigPortal::resultJsonLen() gives (code of 31 bytes)
static size_t bound(size_t messageLen) {
    return sizeof(RESULT_JSON_FRAME) - 1 + 31 + 2 * messageLen;
}

// Valid JSON with the expected result code; the decoded message in @p msg
static bool readBack(const char* buf, const char* code, std::string& msg) {
    JsonSpan doc = jsonRoot(buf, strlen(buf));
    if (!jsonValidate(doc)) return false;
    char r[64];
    if (jsonVal(doc, "result", r, sizeof(r)) < 0 || strcmp(r, code) != 0) return false;
    msg.clear();
    if (jsonHas(doc, "message")) {
        static char m[2048];
        if (jsonVal(doc, "message", m, sizeof(m)) < 0) return false;
        msg = m;
    }
    return true;
}

// Every byte sequence complete
static bool wholeUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (size_t k = 1; k < n; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

int main() {
    printf("=== result answer ===\n");
    char b[512];

    // Shapes
    buildResultJson(b, sizeof(b), "ok", -1, nullptr, false, false);
    CHECK_IS(b, "{\"result\":\"ok\"}");
    buildResultJson(b, sizeof(b), "ok", -1, "", false, true);
    CHECK_IS(b, "{\"result\":\"ok\",\"restart\":true}");
    buildResultJson(b, sizeof(b), "bad_number", 2, nullptr, false, false);
    CHECK_IS(b, "{\"result\":\"bad_number\",\"index\":2}");
    buildResultJson(b, sizeof(b), "refused", -1, "Say \"hi\"\\\n\t\x01", false, false);
    CHECK_IS(b, "{\"result\":\"refused\",\"message\":\"Say \\\"hi\\\"\\\\\\n\\t\\u0001\"}");
    const size_t n = buildResultJson(b, sizeof(b), "ok", 0, "Restored", true, true);
    CHECK_IS(b, "{\"result\":\"ok\",\"index\":0,\"message\":\"Restored\",\"restart\":true}");
    CHECK(n == strlen(b), "length returned");
    std::string m;
    buildResultJson(b, sizeof(b), "refused", -1, "Hűtő \"A\" <b>", false, false);
    CHECK(readBack(b, "refused", m) && m == "Hűtő \"A\" <b>", "round trip through the reader");

    // The bound: a code at its longest and a message of quotes fit whole
    {
        const std::string code(31, 'c');
        const std::string msg(200, '"');
        std::string out(bound(200) + 1, 'X');
        buildResultJson(&out[0], out.size(), code.c_str(), 32767, msg.c_str(), false, true);
        std::string got;
        CHECK(readBack(out.c_str(), code.c_str(), got) && got == msg, "bound: message whole");
    }

    // Too long: cut at a character, "…", still valid
    {
        char s[64];
        buildResultJson(s, sizeof(s), "refused", -1,
                        "Not on this device, skipped: 28FF641E8216C3A1, 28AA0012345678FF",
                        false, false);
        std::string got;
        CHECK(strlen(s) < sizeof(s), "cut: within the buffer");
        CHECK(readBack(s, "refused", got), "cut: valid JSON");
        CHECK(got.size() >= 3 && got.compare(got.size() - 3, 3, "\xE2\x80\xA6") == 0,
              "cut: ends in an ellipsis");
        CHECK(got.compare(0, 10, "Not on thi") == 0, "cut: keeps the start");
    }

    // Every size: never overflows, always valid or nothing; UTF-8 never split
    {
        const char* msg = "Árvíztűrő tükörfúrógép \"x\"\n\x02 ok";
        bool ok = true, utf8 = true, cut = false;
        for (size_t size = 1; size <= 120; size++) {
            std::string out(size + 8, '#');            // guard bytes after the buffer
            const size_t len = buildResultJson(&out[0], size, "refused", 7, msg, false, true);
            if (out.compare(size, 8, std::string(8, '#')) != 0) ok = false;
            if (len == 0) { if (out[0] != '\0') ok = false; continue; }
            if (len >= size || strlen(out.c_str()) != len) ok = false;
            std::string got;
            if (!readBack(out.c_str(), "refused", got)) ok = false;
            if (!wholeUtf8(got)) utf8 = false;
            if (got != msg) cut = true;
        }
        CHECK(ok, "every size: in bounds and valid");
        CHECK(utf8, "every size: whole characters");
        CHECK(cut, "every size: some were cut");
    }

    // The code is never cut: no room for it, no answer
    CHECK(buildResultJson(b, 16, "a_long_result_code", -1, nullptr, false, false) == 0,
          "code does not fit");
    CHECK(b[0] == '\0', "empty on failure");
    CHECK(buildResultJson(b, 16, "ok", -1, nullptr, false, false) == 15, "exact fit");
    CHECK(buildResultJson(b, 15, "ok", -1, nullptr, false, false) == 0, "one short");

    printf("%d checks, %d failed\n", g_run, g_fail);
    if (g_fail == 0) printf("ALL PASSED\n");
    return g_fail ? 1 : 0;
}
