# Changelog

All notable changes to this project will be documented in this file.

The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/soosp/AsyncConfigPortal/compare/0.2.1...HEAD
[0.2.1]: https://github.com/soosp/AsyncConfigPortal/compare/0.2.0...0.2.1
[0.2.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.1.0...0.2.0
[0.1.0]: https://github.com/soosp/AsyncConfigPortal/releases/tag/0.1.0
