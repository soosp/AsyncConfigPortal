// Host test suite for JsonReadUtils.h.
//
//   make            build and run
//
// Two halves: a round-trip against a real /netdata document, and a corpus of
// malformed inputs that must all be rejected. The second half is the reason the
// hand-written parser is defensible instead of reckless.

#include <cstdio>
#include <cstring>
#include "JsonReadUtils.h"

static int g_fail = 0;
static int g_run  = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_run++;                                                              \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static JsonSpan S(const char* s) { return jsonRoot(s, strlen(s)); }

// -----------------------------------------------------------------------------
// A real /netdata document: array of entries, each with a nested "cfg".
// Note the escaped SSID (\u00XX is what WiFiProfile::_doJson emits) and the
// DHCP profile that omits ip/mask/gw/dnsN entirely.
// -----------------------------------------------------------------------------
static const char* NETDATA =
    "["
      "{\"idx\":0,\"type\":0,\"ssid\":\"\",\"dnscnt\":2,\"cfg\":{"
        "\"dhcp\":0,\"ip\":\"192.168.1.50\",\"mask\":\"255.255.255.0\","
        "\"gw\":\"192.168.1.1\",\"dns0\":\"8.8.8.8\",\"dns1\":\"1.1.1.1\","
        "\"host\":\"ntp-office\",\"prio\":1,\"mac\":\"AA:BB:CC:DD:EE:FF\"}},"
      "{\"idx\":1,\"type\":1,\"ssid\":\"My\\u0020Net\\u0022x\","
        "\"txmin\":-1.00,\"txmax\":19.50,\"txstep\":0.25,\"dnscnt\":2,\"cfg\":{"
        "\"dhcp\":1,\"host\":\"ntp-office\",\"prio\":2,"
        "\"mac\":\"11:22:33:44:55:66\",\"ssid\":\"My\\u0020Net\\u0022x\","
        "\"txpwr\":19.500000}}"
    "]";

static void testRoundTrip() {
    printf("Round-trip against a real /netdata document:\n");

    JsonSpan doc = S(NETDATA);
    CHECK(jsonValidate(doc), "document should validate");
    CHECK(jsonCount(doc) == 2, "expected 2 entries, got %zu", jsonCount(doc));

    // --- entry 0: static Ethernet -------------------------------------------
    JsonSpan e0 = jsonElement(doc, 0);
    CHECK((bool)e0, "entry 0 missing");

    uint8_t type = 99;
    CHECK(jsonNum(e0, "type", type) == 0 && type == 0, "type0=%u", type);

    JsonSpan c0 = jsonMember(e0, "cfg");
    CHECK((bool)c0, "cfg 0 missing");

    bool dhcp = true;
    CHECK(jsonNum(c0, "dhcp", dhcp) == 0 && dhcp == false, "dhcp0 should be false");

    uint8_t ip[4];
    CHECK(jsonIp4(c0, "ip", ip) == 0 &&
          ip[0] == 192 && ip[1] == 168 && ip[2] == 1 && ip[3] == 50, "ip0");

    uint8_t dns1[4];
    CHECK(jsonIp4(c0, "dns1", dns1) == 0 && dns1[0] == 1 && dns1[3] == 1, "dns1");

    char host[32];
    CHECK(jsonVal(c0, "host", host, sizeof(host)) == 10 &&
          strcmp(host, "ntp-office") == 0, "host0='%s'", host);

    uint8_t prio = 0;
    CHECK(jsonNum(c0, "prio", prio) == 0 && prio == 1, "prio0=%u", prio);

    // --- entry 1: DHCP Wi-Fi, escaped SSID ----------------------------------
    JsonSpan e1 = jsonElement(doc, 1);
    JsonSpan c1 = jsonMember(e1, "cfg");
    CHECK((bool)c1, "cfg 1 missing");

    CHECK(jsonNum(c1, "dhcp", dhcp) == 0 && dhcp == true, "dhcp1 should be true");

    // Under DHCP the profile omits the static fields — absent, not malformed.
    CHECK(jsonIp4(c1, "ip", ip) == JSON_ABSENT, "ip1 should be ABSENT");
    CHECK(jsonHas(c1, "mask") == false, "mask1 should be absent");

    // \u0020 -> space, \u0022 -> quote
    char ssid[40];
    const int n = jsonVal(c1, "ssid", ssid, sizeof(ssid));
    CHECK(n == 8 && strcmp(ssid, "My Net\"x") == 0, "ssid1='%s' n=%d", ssid, n);

    float tx = 0.0f;
    CHECK(jsonNum(c1, "txpwr", tx) == 0 && tx > 19.4f && tx < 19.6f, "txpwr=%f", (double)tx);

    float txmin = 0.0f;
    CHECK(jsonNum(e1, "txmin", txmin) == 0 && txmin < -0.9f, "txmin=%f", (double)txmin);
}

static void testLookupIsolation() {
    printf("Lookup must not be fooled by strings or nesting:\n");

    // "ip" appears inside a *string value* and inside a *nested object*.
    // A strstr()-based lookup returns the wrong one for both.
    const char* doc =
        "{\"note\":\"the \\u0022ip\\u0022 field\",\"inner\":{\"ip\":\"10.0.0.1\"},"
        "\"ip\":\"192.168.0.9\"}";
    JsonSpan d = S(doc);
    CHECK(jsonValidate(d), "should validate");

    uint8_t ip[4];
    CHECK(jsonIp4(d, "ip", ip) == 0 && ip[0] == 192 && ip[3] == 9,
          "top-level ip wrong: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

    JsonSpan inner = jsonMember(d, "inner");
    CHECK(jsonIp4(inner, "ip", ip) == 0 && ip[0] == 10, "inner ip wrong");

    // A key that is a prefix of another must not match.
    const char* pfx = "{\"dns10\":\"1.2.3.4\",\"dns1\":\"5.6.7.8\"}";
    JsonSpan p = S(pfx);
    CHECK(jsonIp4(p, "dns1", ip) == 0 && ip[0] == 5, "prefix key confusion");
}

static void testOverLongRejected() {
    printf("Over-long values rejected, not truncated:\n");

    // The house pattern: buffer one byte wider than the field, reject > MAX.
    const char* doc = "{\"host\":\"0123456789\"}";
    JsonSpan d = S(doc);

    char fits[11 + 1];                    // MAX 11 -> comfortably fits
    CHECK(jsonVal(d, "host", fits, sizeof(fits)) == 10, "should fit");

    char tight[10];                       // 9 chars max -> 10 does not fit
    CHECK(jsonVal(d, "host", tight, sizeof(tight)) == JSON_INVALID,
          "over-long must be INVALID, got '%s'", tight);
    CHECK(tight[0] == '\0', "buffer must be emptied on rejection");
}

static void testRangeChecked() {
    printf("Out-of-range numbers rejected, not wrapped:\n");

    JsonSpan d = S("{\"a\":300,\"b\":-1,\"c\":1.5,\"d\":42}");

    uint8_t a = 0;
    CHECK(jsonNum(d, "a", a) == JSON_INVALID, "300 into uint8_t must fail, got %u", a);

    uint8_t b = 0;
    CHECK(jsonNum(d, "b", b) == JSON_INVALID, "-1 into uint8_t must fail");

    int c = 0;
    CHECK(jsonNum(d, "c", c) == JSON_INVALID, "1.5 into int must fail, got %d", c);

    uint8_t dd = 0;
    CHECK(jsonNum(d, "d", dd) == 0 && dd == 42, "42 should succeed");

    int8_t e = 0;
    CHECK(jsonNum(d, "missing", e) == JSON_ABSENT, "missing key");
}

// -----------------------------------------------------------------------------
// Malformed corpus — every one of these must be rejected by jsonValidate().
// -----------------------------------------------------------------------------
static void testMalformedCorpus() {
    printf("Malformed corpus (all must be rejected):\n");

    static const char* bad[] = {
        "",                                  // empty
        "{",                                 // unterminated object
        "[",                                 // unterminated array
        "{\"a\":}",                          // missing value
        "{\"a\" 1}",                         // missing colon
        "{\"a\":1,}",                        // trailing comma (object)
        "[1,2,]",                            // trailing comma (array)
        "{a:1}",                             // unquoted key
        "{'a':1}",                           // single quotes
        "{\"a\":\"unterminated}",            // unterminated string
        "{\"a\":\"bad\\escape\"}",           // unknown escape
        "{\"a\":\"\\u12\"}",                 // short \u
        "{\"a\":\"\\uZZZZ\"}",               // non-hex \u
        "{\"a\":\"\\u0141\"}",               // wide \u — outside our subset
        "{\"a\":1e5}",                       // exponent
        "{\"a\":1.}",                        // trailing dot
        "{\"a\":.5}",                        // no integer part
        "{\"a\":+1}",                        // leading plus
        "{\"a\":01x}",                       // trailing garbage in token
        "{\"a\":NaN}",                       // NaN
        "{\"a\":1} garbage",                 // trailing garbage after root
        "{\"a\":1}{\"b\":2}",                // two roots
        "{\"a\":\"x\"} \t\n junk",           // trailing garbage after whitespace
        "{\"a\":\"raw\ncontrol\"}",          // raw control char in string
        "/*c*/{\"a\":1}",                    // comment
        "{\"a\":tru}",                       // truncated literal
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!jsonValidate(S(bad[i])), "should have been rejected: <%s>", bad[i]);
    }

    // Depth bomb: deeper than JSON_READ_MAX_DEPTH must be refused, not recursed.
    char deep[512];
    size_t w = 0;
    for (int i = 0; i < 40; i++) deep[w++] = '[';
    for (int i = 0; i < 40; i++) deep[w++] = ']';
    CHECK(!jsonValidate(jsonRoot(deep, w)), "depth bomb must be rejected");

    // Truncation at every prefix of a valid document must never crash and must
    // never validate (except the empty-object prefix cases, which are not
    // complete documents either).
    const size_t full = strlen(NETDATA);
    for (size_t cut = 0; cut < full; cut++) {
        CHECK(!jsonValidate(jsonRoot(NETDATA, cut)),
              "truncated at %zu must be rejected", cut);
    }
}

static void testWellFormedAccepted() {
    printf("Well-formed edge cases accepted:\n");

    static const char* good[] = {
        "{}",
        "[]",
        "  {  \"a\" :  1  }  ",
        "{\"a\":{}}",
        "{\"a\":[]}",
        "{\"a\":null}",
        "{\"a\":true,\"b\":false}",
        "{\"a\":-0.5}",
        "{\"a\":\"\"}",
        "{\"a\":\"\\u0000\"}",              // NUL byte escape is in-subset
        "[{\"x\":1},{\"x\":2}]",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        CHECK(jsonValidate(S(good[i])), "should have been accepted: <%s>", good[i]);
    }
}

int main() {
    printf("JsonReadUtils host test suite\n");
    printf("------------------------------------------------------------\n");
    testRoundTrip();
    testLookupIsolation();
    testOverLongRejected();
    testRangeChecked();
    testWellFormedAccepted();
    testMalformedCorpus();
    printf("------------------------------------------------------------\n");
    printf("%d checks, %d failures\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
