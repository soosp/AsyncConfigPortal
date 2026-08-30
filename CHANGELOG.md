# Changelog

All notable changes to this project will be documented in this file.

The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.4.2] - 2026-08-30

### Changed

- `/menu` and `/project` are validated with the same `ETag` as the static
  assets. Both are fixed once `begin()` has run — the menu is assembled from the
  pages registered during setup, the project block is compile-time constants —
  so the firmware version identifies them exactly.

  It is not the size of either reply that costs. Every page load fetches both,
  and on ESP8266 a concurrent request costs a request object, a response object
  and a chunk buffer whatever it returns. With the stylesheet and scripts
  already cached, these were two of the four requests a page still made; they
  are now two 304s.

  Measured need: with a TLS MQTT session holding about 6.5 kB, a device with
  ~19.7 kB free after setup has ~13 kB for the web, against a page-load peak
  near 12 kB. It fits until a second page is opened before the first has let
  go, and then a 32-byte allocation fails.

## [0.4.1] - 2026-08-30

### Fixed

- A conditional request carrying `Cache-Control: no-cache` — or `Pragma:
  no-cache`, which Firefox also sends — is answered with the resource rather
  than a 304. RFC 9111 makes that request directive binding on the origin
  server, not only on caches in between, and ignoring it meant a hard reload
  did nothing after a page had changed without a version bump. An ordinary
  reload still revalidates to a 304, so the saving is unaffected.

## [0.4.0] - 2026-08-30

### Added

- `setFavicon()` supplies the icon served at `/favicon.ico`, which until now
  always answered 204. Two forms: a NUL-terminated PROGMEM string, defaulting to
  `image/svg+xml`, and a pointer with an explicit length and content type for
  anything binary — a PNG or an ICO ends at its first zero byte if the length is
  derived with `strlen_P()`.

  A route rather than a data URI in each page's head, because the head is per
  page: a data URI repeats in every one of them and still leaves the built-in
  pages without an icon, which a project cannot reach into. The endpoint carries
  the same ETag as the other static assets, so a browser fetches it once per
  firmware version.
- `sendProgmem()` overload taking an explicit length, for bodies that are not
  NUL-terminated text. The existing form now delegates to it.

## [0.3.0] - 2026-08-30

### Added

- Static assets and pages carry an `ETag` derived from `FIRMWARE_VERSION`, with
  `Cache-Control: no-cache`, and answer a matching `If-None-Match` with a bare
  304.

  This is a memory measure before it is a bandwidth one. A page is not one
  request: the browser fetches the HTML, the stylesheet, one or two scripts, the
  menu, the project block and whatever data the page polls — six or seven at
  once, each with its own request object, response object and chunk buffer. On
  an ESP8266 that peak was measured at about 12 kB for a plain status page,
  which is most of what remains once Wi-Fi, the async server and an MQTT session
  have taken theirs, and it is why a portal that loads at boot stops loading
  later. Letting the browser keep the unchanging assets turns seven requests
  into three or four.

  The version is the right validator for a device: assets change when the
  firmware changes and at no other time. It also closes the trap where an OTA
  update leaves the browser showing the previous version's page, with a plain
  reload not enough to shift it. `no-cache` rather than a `max-age` because the
  browser must still ask — the answer is a 20-byte 304 instead of a 4 kB body,
  and nothing stale survives an update.

  Authentication is checked before the conditional request, so a 304 cannot
  short-circuit it.

## [0.2.1] - 2026-08-29

### Fixed

- The deferred restart on ESP8266 now runs from the cont context rather than
  from the Ticker callback. `ESP.restart()` in the SYS context does not stop the
  cont task, so the SDK began dismantling the network stack while `loop()` was
  still using it; an application polling its network layer every pass reached a
  freed `netif` and the reboot ended in a LoadProhibited exception. The Ticker
  still provides the flush delay, but now hands the call to
  `schedule_function()`.
- Mark `run_tests.sh` executable

## [0.2.0] - 2026-08-28

### Added

- `MqttConfigComponent::attach()` takes a `withHaDiscovery` flag, symmetric with
  `withBackup`, which adds a Home Assistant discovery switch to the MQTT page.
  Off by default. `Changed` gains a matching `haDiscovery` bit, because turning
  discovery off is not a no-op for the application: the retained discovery
  messages have to be cleared or the entities linger in Home Assistant. The
  field is only read from a submission when the row is offered — an absent
  checkbox means unticked, so reading it unconditionally would quietly clear a
  flag the project had set for itself.
- `MqttConfigComponent` requires **NetworkProfile 0.8.0 or newer** and says so
  with an `#error` rather than failing deep inside a header. This library
  declares no dependency on the profile family on purpose — the network and MQTT
  modules are opt-in, and a manifest entry would force the profiles on every
  project that only wants the portal — so `library.json` cannot express the
  constraint and the check is a compile-time guard on
  `NETWORK_PROFILE_VERSION`.

## [0.1.0] - 2026-08-26

First release. The API may still change before 1.0.0.

The optional net and MQTT modules require NetworkProfile >= 0.7.0; the network
status page additionally requires NetworkManager >= 0.3.0. A build that includes
none of them has neither dependency.

### Added

- Web configuration portal for ESP32 and ESP8266, built on ESPAsyncWebServer:
  HTTP digest authentication, a menu assembled from the registered pages, and a
  Status page reporting uptime, heap (including a fragmentation indicator), chip,
  clock and reset reason.
- Built-in Other page: OTA firmware update, restart, factory reset, and password
  change. The OTA path validates an embedded `FirmwareMarker` and rejects an
  image built for a different project or board rather than bricking the device.
- Backup page assembled from per-component sections, with restore validated as a
  whole document before anything is written, so a mangled file changes nothing.
- Composition API for project-specific content — `addPage()`,
  `addJsonEndpoint()`, `addPostHandler()`, `addBackupSection()`,
  `addResetHandler()`, `addUploadHandler()`, and for content that lives in flash
  `sendProgmem()`, `sendProgmemLine()` and `beginStreamed()` — plus shared
  client-side field builders (`ifRow`, `numRow`, `selectRow`) served at
  `/fields.js`, the same ones the built-in pages use.
- `NetConfigComponent`: an optional, opt-in network-configuration page for the
  NetworkProfile family — addressing, DNS, hostname, interface priority, Wi-Fi
  credentials and transmit power. Including it is what pulls in the dependency;
  a build that never includes it has none.

  `setNtpProfile()` adds a Time group for the NTP servers, edited once from a
  device-level `NtpProfile` rather than repeated per interface; not calling it
  leaves the group off the page. The servers are saved and restored with the
  rest of the page, which is what the backup file means to whoever downloads it:
  the settings this page shows. `NetChangeSet::ntp` reports that they changed,
  separately from the interface flags, since the manager applies them on the
  next connect whichever interface that is.
- `MqttConfigComponent`: an optional, opt-in page for MQTT broker settings —
  address, port, TLS and credentials, stored in an `MqttProfile`. It owns no MQTT
  client: it saves and reports what changed, and the application decides what to
  do about it, since the client is the application's.
- `NetStatusComponent`: an optional, opt-in read-only page showing the live link
  — interface, addressing, DNS, NTP, and on Wi-Fi the associated network and
  signal — read from NetworkManager rather than from the stored configuration,
  because the two can differ and that difference is the point. Open without a
  login by default. `addSection()` lets the application add its own rows, so the
  page can report a service it knows nothing about.
- `setCss()` / `setCssExtra()` restyle the whole portal, the built-in pages
  included; `docs/CSS.md` documents the class contract. Runtime rather than
  compile-time, because the handler that serves `/css` is in the library's own
  translation unit, where a sketch's `#define` never arrives.
- Configurable logging through `setLogger()`, with a per-subsystem tag that maps
  directly onto `ESP_LOGx`.
- Three fixed-size registries, each overridable before including the header:
  `CONFIG_PORTAL_MAX_PAGES` (16), `CONFIG_PORTAL_MAX_BACKUP_SECTIONS` (4) and
  `CONFIG_PORTAL_MAX_RESET_HANDLERS` (4). They are small on purpose — the arrays
  are members of the server object — and each `add*()` returns false rather than
  overwriting when its registry is full.
- Examples: `Minimal`, `CustomPage` (composing your own page, hardware-free),
  `NetConfig` (two interfaces with priority-based failover) and
  `EthConfigButton` (reaching a wired device that has no address anyone knows:
  an unconfigured one settles on a fixed address when no DHCP answers, and a
  deployed one moves to it on a button press).

[Unreleased]: https://github.com/soosp/AsyncConfigPortal/compare/0.4.2...HEAD
[0.4.2]: https://github.com/soosp/AsyncConfigPortal/compare/0.4.1...0.4.2
[0.4.1]: https://github.com/soosp/AsyncConfigPortal/compare/0.4.0...0.4.1
[0.4.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.3.0...0.4.0
[0.3.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.2.1...0.3.0
[0.2.1]: https://github.com/soosp/AsyncConfigPortal/compare/0.2.0...0.2.1
[0.2.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.1.0...0.2.0
[0.1.0]: https://github.com/soosp/AsyncConfigPortal/releases/tag/0.1.0
