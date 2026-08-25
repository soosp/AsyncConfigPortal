#pragma once

/**
 * @file upload_claim.h
 * @brief Ownership of a buffer for the duration of one upload.
 *
 * Internal implementation detail — include the component, not this.
 *
 * A restore arrives in chunks across several callbacks, so the buffer holding it
 * has to belong to one request from the first chunk until the last. That is more
 * than a busy flag: the claim has an owner, and an upload that stops mid-way
 * would otherwise hold the buffer for good, so an idle claim can be taken over.
 * The flag itself is never released in that case — the buffer does not become
 * free, it changes hands.
 *
 * The flag is passed in rather than owned here, because a component may guard
 * one buffer that serves more than one route: NetConfigComponent builds
 * /netdata and receives /netrestore into the same memory, and a build in
 * progress must not be trampled by an upload starting.
 */

#include <Arduino.h>
#include <atomic>

class UploadClaim {
public:
    /**
     * @param buf     Buffer the upload is collected into.
     * @param cap     Capacity of @p buf.
     * @param busy    Flag guarding that buffer; shared with its other users.
     * @param idleMs  How long a silent claim is respected before it may be
     *                taken over.
     */
    UploadClaim(char* buf, size_t cap, std::atomic_flag& busy, uint32_t idleMs)
        : _buf(buf), _cap(cap), _busy(busy), _idleMs(idleMs) {}

    /**
     * @brief Takes the buffer for @p req, or confirms it is already theirs.
     * @return false when another request holds it and is still active.
     */
    bool claim(AsyncWebServerRequest* req) {
        const uint32_t now = millis();

        if (_owner == req) {                 // ours already: keep it alive
            _seen = now;
            return true;
        }
        if (_owner) {
            if ((now - _seen) < _idleMs) return false;   // held and still active
        } else {
            if (_busy.test_and_set(std::memory_order_acquire)) return false;
        }
        _owner = req;
        _seen  = now;
        _len   = 0;
        _full  = false;
        return true;
    }

    /** @brief Gives the buffer back, if @p req is the one holding it. */
    void release(AsyncWebServerRequest* req) {
        if (_owner != req) return;
        _owner = nullptr;
        _len   = 0;
        _full  = false;
        _busy.clear(std::memory_order_release);
    }

    /**
     * @brief Appends one chunk, claiming the buffer on the first.
     *
     * Over-long input is rejected rather than truncated: a cut document would
     * parse as far as it got and could apply half a configuration.
     *
     * @return false if the chunk was not ours to take; the caller swallows it.
     */
    bool collect(AsyncWebServerRequest* req, size_t index,
                 const uint8_t* data, size_t len) {
        if (index == 0 && !claim(req)) return false;
        if (_owner != req) return false;
        _seen = millis();

        if (_full) return true;
        if (_len + len > _cap) { _full = true; return true; }
        memcpy(_buf + _len, data, len);
        _len += len;
        return true;
    }

    bool  owns(AsyncWebServerRequest* req) const { return _owner == req; }
    bool  overflowed() const { return _full; }    ///< input exceeded the buffer
    char* data()       const { return _buf; }
    size_t size()      const { return _len; }

private:
    char*                  _buf;
    size_t                 _cap;
    std::atomic_flag&      _busy;
    uint32_t               _idleMs;
    AsyncWebServerRequest* _owner = nullptr;
    uint32_t               _seen  = 0;
    size_t                 _len   = 0;
    bool                   _full  = false;
};
