#pragma once

/**
 * @file result_json.h
 * @brief The answer to a form posted with fetch(): {"result":...} with an
 *        optional index, message, restart flag and the device's new address.
 *        Pure string logic, host-testable.
 *
 * Internal implementation detail — use AsyncConfigPortal::sendResult().
 *
 * The result code is a machine name the page can tie to a field, so it is
 * written whole or not at all. The message is text for the user: if it does
 * not fit, it is cut at a character boundary and ends in "…", and the answer
 * stays valid JSON. The bound the callers size their buffers by
 * (AsyncConfigPortal::resultJsonLen()) counts every message byte twice, which
 * covers everything but control characters (\u00XX, six bytes); those are
 * what the cut is for.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace acp_detail {

/// The longest answer, with every part empty and the index at its widest.
static constexpr char RESULT_JSON_FRAME[] =
    "{\"result\":\"\",\"index\":-32768,\"message\":\"\",\"restart\":true,\"url\":\"\"}";

/// One byte of a string that may live in flash (ESP8266: PROGMEM).
inline unsigned char resultByte(const char* p, bool inFlash) {
#if defined(pgm_read_byte)
    if (inFlash) return (unsigned char)pgm_read_byte(p);
#else
    (void)inFlash;
#endif
    return (unsigned char)*p;
}

/// Bytes in the UTF-8 sequence that starts with @p lead (1 for a stray byte).
inline size_t resultSeqLen(unsigned char lead) {
    if (lead >= 0xF0 && lead <= 0xF7) return 4;
    if (lead >= 0xE0) return lead <= 0xEF ? 3 : 1;
    if (lead >= 0xC0) return 2;
    return 1;
}

/// The escaped form of one byte; @p out holds at least 7 bytes.
inline size_t resultEscape(unsigned char c, char* out) {
    switch (c) {
        case '"':  out[0] = '\\'; out[1] = '"';  return 2;
        case '\\': out[0] = '\\'; out[1] = '\\'; return 2;
        case '\n': out[0] = '\\'; out[1] = 'n';  return 2;
        case '\r': out[0] = '\\'; out[1] = 'r';  return 2;
        case '\t': out[0] = '\\'; out[1] = 't';  return 2;
        default: break;
    }
    if (c < 0x20) {
        snprintf(out, 7, "\\u%04x", (unsigned)c);
        return 6;
    }
    out[0] = (char)c;
    return 1;
}

/// Escaped length of the sequence of @p n bytes at @p p.
inline size_t resultSeqEscLen(const char* p, size_t n, bool inFlash) {
    char e[7];
    size_t len = 0;
    for (size_t i = 0; i < n; i++) len += resultEscape(resultByte(p + i, inFlash), e);
    return len;
}

/// Appends @p s escaped; false (and nothing more written) if it does not fit.
inline bool resultCatEsc(char* buf, size_t size, size_t& pos, const char* s) {
    char e[7];
    size_t need = 0;
    for (const char* p = s; *p; p++) need += resultEscape((unsigned char)*p, e);
    if (pos + need >= size) return false;
    for (const char* p = s; *p; p++) {
        const size_t n = resultEscape((unsigned char)*p, e);
        memcpy(buf + pos, e, n);
        pos += n;
    }
    buf[pos] = '\0';
    return true;
}

/// Appends @p s as is; false if it does not fit.
inline bool resultCat(char* buf, size_t size, size_t& pos, const char* s) {
    const size_t n = strlen(s);
    if (pos + n >= size) return false;
    memcpy(buf + pos, s, n + 1);
    pos += n;
    return true;
}

/**
 * @brief Writes the answer into @p buf.
 *
 * @param result  Machine name ("ok", "bad_port", ...); required.
 * @param index   The item the answer refers to, or -1 to leave it out.
 * @param message Text for the user, or null / empty to leave it out.
 * @param inFlash @p message is in flash (PROGMEM) on ESP8266.
 * @param restart The device restarts to apply what was saved.
 * @param url     Where the device will answer after the restart, if it is
 *                not this address (its new static address); null for none.
 *                Written whole or, with the message, not at all.
 * @return Length written; 0 if the frame and the whole code do not fit.
 */
inline size_t buildResultJson(char* buf, size_t size, const char* result, int16_t index,
                              const char* message, bool inFlash, bool restart,
                              const char* url = nullptr) {
    if (!buf || size == 0) return 0;
    buf[0] = '\0';
    if (!result) result = "";
    // The tail, after the message: the restart flag and the URL, which is
    // short and never cut, so the message is what gives way
    char tail[96];
    size_t tailLen = 0;
    tail[0] = '\0';
    if (restart) tailLen += (size_t)snprintf(tail, sizeof(tail), ",\"restart\":true");
    if (url && url[0]) {
        size_t p = tailLen;
        if (resultCat(tail, sizeof(tail), p, ",\"url\":\"") &&
            resultCatEsc(tail, sizeof(tail), p, url) &&
            resultCat(tail, sizeof(tail), p, "\"")) {
            tailLen = p;
        } else {
            tail[tailLen] = '\0';                 // too long for the tail: left out
        }
    }
    if (tailLen + 1 < sizeof(tail)) { tail[tailLen++] = '}'; tail[tailLen] = '\0'; }
    size_t pos = 0;
    if (!resultCat(buf, size, pos, "{\"result\":\"") ||
        !resultCatEsc(buf, size, pos, result) ||
        !resultCat(buf, size, pos, "\"")) {
        buf[0] = '\0';
        return 0;
    }
    if (index >= 0) {
        char n[16];
        snprintf(n, sizeof(n), ",\"index\":%d", (int)index);
        if (!resultCat(buf, size, pos, n)) { buf[0] = '\0'; return 0; }
    }
    if (pos + tailLen >= size) { buf[0] = '\0'; return 0; }

    static const char MSG_HEAD[] = ",\"message\":\"";
    static const char ELLIPSIS[] = "\xE2\x80\xA6";
    const size_t headLen = sizeof(MSG_HEAD) - 1;
    const size_t ellLen  = sizeof(ELLIPSIS) - 1;
    // Room for at least the ellipsis, or the message is left out
    if (message && resultByte(message, inFlash) != 0 &&
        pos + headLen + ellLen + 1 + tailLen < size) {
        resultCat(buf, size, pos, MSG_HEAD);
        // What is left for the text: before the closing quote, the tail and NUL
        const size_t room = size - 1 - pos - 1 - tailLen;
        size_t whole = 0;
        for (const char* p = message; resultByte(p, inFlash); ) {
            const size_t n = resultSeqLen(resultByte(p, inFlash));
            size_t k = 0;
            while (k < n && resultByte(p + k, inFlash)) k++;
            whole += resultSeqEscLen(p, k, inFlash);
            p += k;
        }
        const size_t limit = whole <= room ? room : room - ellLen;
        size_t used = 0;
        const char* p = message;
        while (resultByte(p, inFlash)) {
            const size_t n = resultSeqLen(resultByte(p, inFlash));
            size_t k = 0;
            while (k < n && resultByte(p + k, inFlash)) k++;
            const size_t e = resultSeqEscLen(p, k, inFlash);
            if (used + e > limit) break;
            char esc[7];
            for (size_t i = 0; i < k; i++) {
                const size_t m = resultEscape(resultByte(p + i, inFlash), esc);
                memcpy(buf + pos, esc, m);
                pos += m;
            }
            used += e;
            p += k;
        }
        if (whole > room) {
            memcpy(buf + pos, ELLIPSIS, ellLen);
            pos += ellLen;
        }
        buf[pos++] = '"';
        buf[pos] = '\0';
    }
    resultCat(buf, size, pos, tail);   // room kept above
    return pos;
}

} // namespace acp_detail
