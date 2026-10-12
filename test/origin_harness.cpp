// Host test suite for detail/origin_check.h (cross-site request check).
//
//   make origin     build and run

#include <cstdio>
#include "detail/origin_check.h"

using acp_detail::checkOrigin;
using V = acp_detail::OriginVerdict;

static int g_fail = 0;
static int g_run  = 0;

static void expect(V got, V want, const char* origin, const char* referer,
                   const char* host, int line) {
    g_run++;
    if (got != want) {
        g_fail++;
        printf("  FAIL line %d: origin=\"%s\" referer=\"%s\" host=\"%s\"\n", line,
               origin ? origin : "(none)", referer ? referer : "(none)",
               host ? host : "(none)");
    }
}
#define T(o, r, h, want) expect(checkOrigin(o, r, h), want, o, r, h, __LINE__)

int main() {
    printf("=== origin check ===\n");

    // The device's own pages
    T("http://192.168.1.50", nullptr, "192.168.1.50", V::Match);
    T("http://192.168.1.50:8080", nullptr, "192.168.1.50:8080", V::Match);
    T("http://Custos-1A2B3C.local", nullptr, "custos-1a2b3c.local", V::Match);
    T("https://dev.example.org", nullptr, "dev.example.org", V::Match);   // TLS proxy
    T(nullptr, "http://192.168.1.50/sms?x=1", "192.168.1.50", V::Match);
    T("", "http://192.168.1.50/", "192.168.1.50", V::Match);              // empty Origin
    T("http://192.168.1.50", "http://evil.example/", "192.168.1.50", V::Match); // Origin wins
    T("http://192.168.1.50", nullptr, " 192.168.1.50 ", V::Match);

    // Another site
    T("http://evil.example", nullptr, "192.168.1.50", V::Mismatch);
    T("http://192.168.1.5", nullptr, "192.168.1.50", V::Mismatch);         // prefix only
    T("http://192.168.1.500", nullptr, "192.168.1.50", V::Mismatch);
    T("http://192.168.1.50:81", nullptr, "192.168.1.50", V::Mismatch);     // other port
    T("http://192.168.1.50", nullptr, "192.168.1.50:8080", V::Mismatch);
    T(nullptr, "http://evil.example/page", "192.168.1.50", V::Mismatch);
    T("http://evil.example", "http://192.168.1.50/", "192.168.1.50", V::Mismatch); // Origin wins

    // Unreadable values count as another site
    T("null", nullptr, "192.168.1.50", V::Mismatch);   // privacy-stripped origin
    T("192.168.1.50", nullptr, "192.168.1.50", V::Mismatch);
    T("http://", nullptr, "192.168.1.50", V::Mismatch);
    T("http://user@192.168.1.50", nullptr, "192.168.1.50", V::Mismatch);
    T("http://192.168.1.50", nullptr, nullptr, V::Mismatch);              // no Host
    T("http://192.168.1.50", nullptr, "", V::Mismatch);

    // Nothing sent: curl, scripts, a browser that strips both
    T(nullptr, nullptr, "192.168.1.50", V::Missing);
    T("", "", "192.168.1.50", V::Missing);

    printf("  %d checks, %d failed\n", g_run, g_fail);
    printf("  [%s]\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
