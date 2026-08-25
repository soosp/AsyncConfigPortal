#pragma once

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits>
#include <type_traits>

/**
 * @file JsonReadUtils.h
 * @brief String-free, allocation-free JSON *reading* helpers.
 *
 * The read-side counterpart to WebFormUtils.h. That header turns a POST body
 * field into a fixed buffer; this one does the same for a JSON document, with a
 * deliberately identical calling shape:
 *
 *     int n = postVal(req,  "ssid0", buf, sizeof(buf));   // form path
 *     int n = jsonVal(cfg,  "ssid",  buf, sizeof(buf));   // restore path
 *
 * so the "read one byte wider than the field and reject anything longer"
 * pattern used throughout NetConfigComponent works unchanged on both.
 *
 * Scope — this is NOT a general JSON parser:
 *   It reads back documents this firmware produced. The accepted grammar is
 *   restricted to what the serialisers emit, and anything outside it is
 *   *rejected* rather than interpreted. Silent misinterpretation of a config
 *   value is worse than a visible failure.
 *
 * Design notes:
 *
 *   - Spans, not C strings. The restore document arrives as an uploaded body
 *     that is not guaranteed to be null-terminated, so every scan is bounded by
 *     an explicit end pointer. This is the primary over-read defence.
 *
 *   - Validate once, then look up. jsonValidate() performs a single strict
 *     recursive pass. After it passes, a failed lookup unambiguously means
 *     "absent" rather than "malformed syntax somewhere", which is why no
 *     `mandatory` flag is needed: the caller can tell the two apart from the
 *     return value alone.
 *
 *   - No dependency on ESPAsyncWebServer (unlike WebFormUtils.h), so the parser
 *     compiles and runs on a host with nothing but a stub Arduino.h. That is
 *     what makes the malformed-input test corpus cheap to run.
 *
 *   - No dynamic allocation, no recursion beyond JSON_READ_MAX_DEPTH.
 */

// -----------------------------------------------------------------------------
// Limits
// -----------------------------------------------------------------------------

#ifndef CONFIG_PORTAL_JSON_READ_MAX_DEPTH
// Nesting depth cap. The deepest document the library produces is the /netdata
// array of objects each holding a "cfg" object — depth 3. 8 leaves room for
// growth while bounding the recursion an uploaded file can trigger.
#  define CONFIG_PORTAL_JSON_READ_MAX_DEPTH 8
#endif

#ifndef CONFIG_PORTAL_JSON_READ_IP_STR_LEN
// Textual IPv4/IPv6 length, excluding the terminator. Mirrors
// CONFIG_PORTAL_FORM_IP_STR_LEN in WebFormUtils.h, restated rather than
// included so this header stays free of the web-server dependency.
#  define CONFIG_PORTAL_JSON_READ_IP_STR_LEN 45
#endif

static constexpr uint8_t JSON_READ_MAX_DEPTH   = CONFIG_PORTAL_JSON_READ_MAX_DEPTH;

static constexpr size_t  JSON_READ_IP_STR_LEN  = CONFIG_PORTAL_JSON_READ_IP_STR_LEN;
static constexpr size_t  JSON_READ_IP_STR_SIZE = JSON_READ_IP_STR_LEN + 1;

/** @brief Key was not present in the object. Mirrors postVal()'s -1. */
static constexpr int JSON_ABSENT = -1;

/** @brief Key was present but unusable: wrong type, out of range, or longer
 *  than the destination. Always a hard failure for the caller. */
static constexpr int JSON_INVALID = -2;

// -----------------------------------------------------------------------------
// Span
// -----------------------------------------------------------------------------

/**
 * @brief A bounded view of JSON text. Never assumes a null terminator.
 */
struct JsonSpan {
    const char* ptr = nullptr;
    size_t      len = 0;

    JsonSpan() = default;
    JsonSpan(const char* p, size_t n) : ptr(p), len(n) {}

    /** @brief True if the span refers to something. */
    explicit operator bool() const { return ptr != nullptr; }

    const char* end() const { return ptr + len; }
};

/** @brief Wraps a raw buffer as a span. */
inline JsonSpan jsonRoot(const char* doc, size_t len) {
    return JsonSpan(doc, len);
}

// -----------------------------------------------------------------------------
// Internals
// -----------------------------------------------------------------------------

namespace jsonread_detail {

inline const char* skipWs(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

inline bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

/**
 * @brief Scans a string token. @p p must sit on the opening quote.
 * @return Pointer just past the closing quote, or nullptr if malformed.
 *
 * Accepted escapes: the two-character forms, and \\u00XX only. A \\u escape
 * with a non-zero high byte is rejected — the serialisers only ever emit
 * \\u00XX (one escape per byte), so anything wider did not come from us and we
 * refuse to guess an encoding for it.
 */
inline const char* scanString(const char* p, const char* end) {
    if (p >= end || *p != '"') return nullptr;
    p++;
    while (p < end) {
        const char c = *p;
        if (c == '"') return p + 1;
        if (c == '\\') {
            p++;
            if (p >= end) return nullptr;
            const char e = *p;
            if (e == '"' || e == '\\' || e == '/' || e == 'b' ||
                e == 'f' || e == 'n' || e == 'r' || e == 't') {
                p++;
                continue;
            }
            if (e == 'u') {
                if (p + 4 >= end) return nullptr;
                for (int i = 1; i <= 4; i++) {
                    if (!isHex(p[i])) return nullptr;
                }
                // High byte must be zero: \u00XX is the only form we emit.
                if (p[1] != '0' || p[2] != '0') return nullptr;
                p += 5;
                continue;
            }
            return nullptr;  // unknown escape
        }
        // Raw control characters are not legal inside a JSON string.
        if ((unsigned char)c < 0x20) return nullptr;
        p++;
    }
    return nullptr;  // unterminated
}

/**
 * @brief Scans a bare token: true, false, null, or a number.
 * @return Pointer just past the token, or nullptr if malformed.
 *
 * Numbers are restricted to  -?digits(.digits)?  — no exponent. The producers
 * are snprintf's %u, %d and %f, none of which emit one for the value ranges
 * involved, so an exponent means the document did not come from us.
 */
inline const char* scanBare(const char* p, const char* end) {
    const size_t avail = (size_t)(end - p);
    if (avail >= 4 && memcmp(p, "true", 4) == 0) return p + 4;
    if (avail >= 5 && memcmp(p, "false", 5) == 0) return p + 5;
    if (avail >= 4 && memcmp(p, "null", 4) == 0) return p + 4;

    const char* s = p;
    if (p < end && *p == '-') p++;
    const char* digitsStart = p;
    while (p < end && *p >= '0' && *p <= '9') p++;
    if (p == digitsStart) return nullptr;          // no integer part
    if (p < end && *p == '.') {
        p++;
        const char* fracStart = p;
        while (p < end && *p >= '0' && *p <= '9') p++;
        if (p == fracStart) return nullptr;        // trailing dot
    }
    if (p < end && (*p == 'e' || *p == 'E')) return nullptr;  // exponent rejected
    return (p > s) ? p : nullptr;
}

inline const char* scanValue(const char* p, const char* end, uint8_t depth);

/** @brief Scans an object or array. @p p sits on the opening bracket. */
inline const char* scanContainer(const char* p, const char* end, uint8_t depth) {
    const bool isObj = (*p == '{');
    const char close = isObj ? '}' : ']';
    p++;
    p = skipWs(p, end);
    if (p < end && *p == close) return p + 1;   // empty container

    for (;;) {
        if (isObj) {
            p = skipWs(p, end);
            const char* k = scanString(p, end);
            if (!k) return nullptr;
            p = skipWs(k, end);
            if (p >= end || *p != ':') return nullptr;
            p++;
        }
        const char* v = scanValue(p, end, (uint8_t)(depth + 1));
        if (!v) return nullptr;
        p = skipWs(v, end);
        if (p >= end) return nullptr;
        if (*p == ',') { p++; continue; }        // trailing comma rejected below
        if (*p == close) return p + 1;
        return nullptr;
    }
}

/** @brief Scans any value. @return Pointer just past it, or nullptr. */
inline const char* scanValue(const char* p, const char* end, uint8_t depth) {
    if (depth > JSON_READ_MAX_DEPTH) return nullptr;
    p = skipWs(p, end);
    if (p >= end) return nullptr;
    if (*p == '"') return scanString(p, end);
    if (*p == '{' || *p == '[') return scanContainer(p, end, depth);
    return scanBare(p, end);
}

}  // namespace jsonread_detail

// -----------------------------------------------------------------------------
// Validation
// -----------------------------------------------------------------------------

/**
 * @brief Strictly validates a whole document against the accepted subset.
 *
 * Run this once on the uploaded body before any lookup. Rejects: unterminated
 * strings, unknown or wide \\u escapes, exponent numbers, trailing commas,
 * comments, single quotes, over-deep nesting, and trailing garbage after the
 * root value.
 *
 * @return true if the document is well-formed and within the subset.
 */
inline bool jsonValidate(JsonSpan doc) {
    if (!doc.ptr) return false;
    const char* end = doc.end();
    const char* p = jsonread_detail::scanValue(doc.ptr, end, 0);
    if (!p) return false;
    p = jsonread_detail::skipWs(p, end);
    return p == end;   // no trailing garbage
}

// -----------------------------------------------------------------------------
// Navigation
// -----------------------------------------------------------------------------

/**
 * @brief Looks up a member of an object, one level deep.
 *
 * Depth- and string-aware: a brace or a matching key text inside a nested value
 * or inside a quoted string cannot be mistaken for a member of @p obj. This is
 * the difference from a plain strstr(), and the reason a nested document needs
 * a real scanner.
 *
 * @param obj Span covering an object (starting at '{').
 * @param key Member name. Must be plain text — a key containing an escape never
 *            matches, since the library only ever emits ASCII identifiers.
 * @return Span covering the member's value, or an empty span if absent.
 */
inline JsonSpan jsonMember(JsonSpan obj, const char* key) {
    using namespace jsonread_detail;
    if (!obj.ptr || !key) return JsonSpan();

    const char* end = obj.end();
    const char* p = skipWs(obj.ptr, end);
    if (p >= end || *p != '{') return JsonSpan();
    p++;

    const size_t keyLen = strlen(key);

    for (;;) {
        p = skipWs(p, end);
        if (p >= end || *p == '}') return JsonSpan();

        const char* kStart = p;
        const char* kEnd = scanString(p, end);
        if (!kEnd) return JsonSpan();

        // Compare the raw key text between the quotes.
        const size_t rawLen = (size_t)(kEnd - kStart) - 2;
        const bool match = (rawLen == keyLen) &&
                           (memcmp(kStart + 1, key, keyLen) == 0);

        p = skipWs(kEnd, end);
        if (p >= end || *p != ':') return JsonSpan();
        p++;

        const char* vStart = skipWs(p, end);
        const char* vEnd = scanValue(vStart, end, 1);
        if (!vEnd) return JsonSpan();

        if (match) return JsonSpan(vStart, (size_t)(vEnd - vStart));

        p = skipWs(vEnd, end);
        if (p >= end) return JsonSpan();
        if (*p == ',') { p++; continue; }
        return JsonSpan();
    }
}

/**
 * @brief Returns the number of elements in an array span.
 */
inline size_t jsonCount(JsonSpan arr) {
    using namespace jsonread_detail;
    if (!arr.ptr) return 0;

    const char* end = arr.end();
    const char* p = skipWs(arr.ptr, end);
    if (p >= end || *p != '[') return 0;
    p++;
    p = skipWs(p, end);
    if (p < end && *p == ']') return 0;

    size_t n = 0;
    for (;;) {
        const char* v = scanValue(p, end, 1);
        if (!v) return n;
        n++;
        p = skipWs(v, end);
        if (p >= end) return n;
        if (*p == ',') { p++; continue; }
        return n;
    }
}

/**
 * @brief Returns the element at @p idx of an array span.
 */
inline JsonSpan jsonElement(JsonSpan arr, size_t idx) {
    using namespace jsonread_detail;
    if (!arr.ptr) return JsonSpan();

    const char* end = arr.end();
    const char* p = skipWs(arr.ptr, end);
    if (p >= end || *p != '[') return JsonSpan();
    p++;

    for (size_t i = 0;; i++) {
        p = skipWs(p, end);
        if (p >= end || *p == ']') return JsonSpan();

        const char* vStart = p;
        const char* vEnd = scanValue(vStart, end, 1);
        if (!vEnd) return JsonSpan();
        if (i == idx) return JsonSpan(vStart, (size_t)(vEnd - vStart));

        p = skipWs(vEnd, end);
        if (p >= end) return JsonSpan();
        if (*p == ',') { p++; continue; }
        return JsonSpan();
    }
}

// -----------------------------------------------------------------------------
// Typed extraction
// -----------------------------------------------------------------------------

/**
 * @brief Copies a string value into a fixed buffer, decoding escapes.
 *
 * @param v   Span covering a string value (including its quotes).
 * @param buf Destination; always null-terminated when @p len > 0.
 * @param len Size of the destination buffer.
 * @return Decoded length, or JSON_INVALID if @p v is not a string or does not
 *         fit. Note that decoding only ever shrinks the text (\\u00XX collapses
 *         six characters to one byte), so the caller's "buffer one byte wider
 *         than the field" trick still detects an over-long value.
 */
inline int jsonStrCopy(JsonSpan v, char* buf, size_t len) {
    using namespace jsonread_detail;
    if (len == 0) return JSON_INVALID;
    buf[0] = '\0';
    if (!v.ptr || v.len < 2 || v.ptr[0] != '"') return JSON_INVALID;

    // Any rejection must still leave a terminated buffer: the caller may log
    // or compare it, and a half-decoded, unterminated buffer is exactly the
    // class of defect this parser exists to avoid. (Caught by the host test.)
    auto reject = [&]() -> int { buf[0] = '\0'; return JSON_INVALID; };

    const char* p = v.ptr + 1;
    const char* end = v.end() - 1;      // the closing quote
    size_t w = 0;

    while (p < end) {
        char c = *p;
        if (c == '\\') {
            p++;
            if (p >= end) return reject();
            switch (*p) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    if (p + 4 >= end) return reject();
                    if (p[1] != '0' || p[2] != '0') return reject();
                    if (!isHex(p[3]) || !isHex(p[4])) return reject();
                    c = (char)((hexVal(p[3]) << 4) | hexVal(p[4]));
                    p += 4;
                    break;
                }
                default: return reject();
            }
        }
        if (w + 1 >= len) return reject();      // does not fit
        buf[w++] = c;
        p++;
    }
    buf[w] = '\0';
    return (int)w;
}

/**
 * @brief Reads a string member into a fixed buffer. The read-side twin of
 *        postVal(), with the same return convention.
 *
 * @return Copied length, JSON_ABSENT if the key is missing, or JSON_INVALID if
 *         it is present but not a string / does not fit.
 */
inline int jsonVal(JsonSpan obj, const char* key, char* buf, size_t len) {
    JsonSpan v = jsonMember(obj, key);
    if (!v) {
        if (len) buf[0] = '\0';
        return JSON_ABSENT;
    }
    return jsonStrCopy(v, buf, len);
}

/** @brief True if the object has the given member. Twin of postHas(). */
inline bool jsonHas(JsonSpan obj, const char* key) {
    return (bool)jsonMember(obj, key);
}

/**
 * @brief Reads a numeric or boolean member, range-checked against T.
 *
 * Booleans accept both the JSON literals and 0/1, because NetworkProfile emits
 * flags such as "dhcp" through %u rather than as JSON booleans.
 *
 * Out-of-range values are rejected rather than truncated: storing 300 into a
 * uint8_t as 44 is exactly the silent misinterpretation this parser exists to
 * avoid.
 *
 * @return 0 on success, JSON_ABSENT, or JSON_INVALID.
 */
template <typename T>
inline int jsonNum(JsonSpan obj, const char* key, T& out) {
    using namespace jsonread_detail;
    JsonSpan v = jsonMember(obj, key);
    if (!v) return JSON_ABSENT;
    if (v.len == 0) return JSON_INVALID;

    if constexpr (std::is_same_v<T, bool>) {
        if (v.len == 4 && memcmp(v.ptr, "true", 4) == 0)  { out = true;  return 0; }
        if (v.len == 5 && memcmp(v.ptr, "false", 5) == 0) { out = false; return 0; }
        if (v.len == 1 && v.ptr[0] == '1') { out = true;  return 0; }
        if (v.len == 1 && v.ptr[0] == '0') { out = false; return 0; }
        return JSON_INVALID;
    } else {
        // Copy the token out so strtod/strtoll get a terminated string; the
        // token is short by construction (scanBare bounds it).
        char tok[40];
        if (v.len >= sizeof(tok)) return JSON_INVALID;
        memcpy(tok, v.ptr, v.len);
        tok[v.len] = '\0';

        char* endp = nullptr;
        if constexpr (std::is_floating_point_v<T>) {
            const double d = strtod(tok, &endp);
            if (endp != tok + v.len) return JSON_INVALID;
            if (d < (double)std::numeric_limits<T>::lowest() ||
                d > (double)std::numeric_limits<T>::max()) return JSON_INVALID;
            out = (T)d;
            return 0;
        } else {
            const long long n = strtoll(tok, &endp, 10);
            if (endp != tok + v.len) return JSON_INVALID;   // rejects "1.5" for an int
            if (n < (long long)std::numeric_limits<T>::min() ||
                n > (long long)std::numeric_limits<T>::max()) return JSON_INVALID;
            out = (T)n;
            return 0;
        }
    }
}

/**
 * @brief Reads a dotted-quad string member into four octets.
 *
 * Kept free of IPAddress so the parser has no Arduino dependency beyond the
 * stub; jsonIp() below is the convenience wrapper.
 *
 * @return 0 on success, JSON_ABSENT, or JSON_INVALID.
 */
inline int jsonIp4(JsonSpan obj, const char* key, uint8_t out[4]) {
    char s[JSON_READ_IP_STR_SIZE];
    const int n = jsonVal(obj, key, s, sizeof(s));
    if (n < 0) return n;

    unsigned o[4];
    int consumed = 0;
    if (sscanf(s, "%u.%u.%u.%u%n", &o[0], &o[1], &o[2], &o[3], &consumed) != 4) {
        return JSON_INVALID;
    }
    if (consumed != n) return JSON_INVALID;          // trailing junk
    for (int i = 0; i < 4; i++) {
        if (o[i] > 255) return JSON_INVALID;
        out[i] = (uint8_t)o[i];
    }
    return 0;
}

/** @brief Reads a dotted-quad member as an IPAddress. Twin of postIp(). */
inline int jsonIp(JsonSpan obj, const char* key, IPAddress& out) {
    uint8_t o[4];
    const int r = jsonIp4(obj, key, o);
    if (r < 0) return r;
    out = IPAddress(o[0], o[1], o[2], o[3]);
    return 0;
}
