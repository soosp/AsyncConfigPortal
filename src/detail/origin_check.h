#pragma once

/**
 * @file origin_check.h
 * @brief Same-origin test for state-changing requests (cross-site request
 *        forgery). Pure string logic, host-testable.
 *
 * A page on another site can make the browser POST to the device, and the
 * browser adds the stored digest credentials by itself. What it cannot do is
 * hide where the request came from: every current browser sends an Origin
 * header with a POST (Referer as well, unless a policy removes it), and a page
 * cannot forge either. So a POST is genuine when the authority in its Origin —
 * or, without one, in its Referer — is the one the request was sent to, the
 * Host header.
 *
 * Authorities are compared without the scheme (a proxy may terminate TLS) and
 * case-insensitively; ports must match as written ("host" and "host:80" differ,
 * as browsers send neither with a default port).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace acp_detail {

/** @brief What the request headers say about where a request came from. */
enum class OriginVerdict : uint8_t {
    Match,      ///< Origin (or Referer) names the Host the request was sent to
    Mismatch,   ///< It names another site, "null", or cannot be read
    Missing     ///< Neither Origin nor Referer was sent
};

/**
 * @brief The authority of "scheme://authority[/path...]".
 * @return false if the URL has no "://" or an empty authority ("null", a bare
 *         word or a malformed value also land here).
 */
inline bool urlAuthority(const char* url, const char*& start, size_t& len) {
    if (!url) return false;
    const char* p = strstr(url, "://");
    if (!p || p == url) return false;
    p += 3;
    const char* e = p;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    // Credentials in the authority ("user@host") are not something a browser
    // sends in Origin or Referer; treat them as unreadable
    for (const char* q = p; q < e; q++) if (*q == '@') return false;
    if (e == p) return false;
    start = p;
    len   = (size_t)(e - p);
    return true;
}

/// Case-insensitive equality of an authority with the Host header value.
inline bool sameAuthority(const char* a, size_t alen, const char* host) {
    if (!host) return false;
    // A Host header may carry surrounding spaces after parsing; ignore them
    while (*host == ' ') host++;
    size_t hlen = strlen(host);
    while (hlen && host[hlen - 1] == ' ') hlen--;
    if (hlen != alen || alen == 0) return false;
    for (size_t i = 0; i < alen; i++) {
        char x = a[i], y = host[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

/**
 * @brief Classifies a request by its Origin, Referer and Host headers.
 * @param origin  Origin header value, or nullptr / "" when absent.
 * @param referer Referer header value, or nullptr / "" when absent.
 * @param host    Host header value, or nullptr / "" when absent.
 */
inline OriginVerdict checkOrigin(const char* origin, const char* referer,
                                 const char* host) {
    const char* src = (origin && *origin) ? origin
                    : (referer && *referer) ? referer
                    : nullptr;
    if (!src) return OriginVerdict::Missing;
    const char* a;
    size_t      alen;
    if (!urlAuthority(src, a, alen)) return OriginVerdict::Mismatch;
    return sameAuthority(a, alen, host) ? OriginVerdict::Match
                                        : OriginVerdict::Mismatch;
}

} // namespace acp_detail
