# Changelog

All notable changes to this project will be documented in this file.

The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.13.2] - 2026-10-13

### Added

- `pfPost()` takes `opts.button`: the button disabled while the post runs,
  in place of the submit button of `ctx`. For an action button that is not
  a submit button (one inside another form, which Enter must not press),
  answered on a line of its own next to it

### Fixed

- CHANGELOG: the compare links of 0.13.0 and 0.13.1

## [0.13.1] - 2026-10-13

### Fixed

- MQTT page: the base topic's length limit is the profile's
  (`MqttProfile::MAX_TOPIC_LEN`, sent as `topicMax`), checked in the page
  too; the message no longer names a fixed 64

## [0.13.0] - 2026-10-13

### Added

- MQTT page: a Base topic field, the device's namespace on the broker
  (`MqttProfile::baseTopic`, NetworkProfile 0.10.0). Empty keeps the
  application's default, which the field shows as its placeholder
  (`setDefaultBaseTopic()`), so a device follows its hostname until a topic
  is written in. Refused as `bad_topic` at the field; in the backup;
  `Changed::baseTopic` on a save or a restore that changed it, for the
  application to clear the retained messages on the old one

## [0.12.0] - 2026-10-13

### Added

- `sendResult()` takes a `url`: where to try the device after a restart if
  it does not come back at this address. The cover shows it as a link, and
  does not go there by itself: with two interfaces the page may be on the
  other one. `RESULT_URL_LEN`, counted in `resultJsonLen()`

### Changed

- The Network page posts in the page too. A refused field is outlined and
  named below the Save button, with the profile's index (`bad_hostname`,
  `bad_ip`, `bad_mask`, `gateway_off_subnet`, `bad_dns`, `bad_ssid`,
  `bad_password`, `bad_tx_power`, `bad_ntp`, `too_long`). The save is
  staged and checked whole before anything is written, on the same code as
  the restore: a refused interface no longer leaves an earlier one applied.
  With `setRestartOnSave()` the page covers itself until the device is back;
  a save or a restore that sets a static address offers it as a link
- `docs/DESIGN.md` and its boundary figure cover the syslog module and the
  `detail/` headers, which they had left out

## [0.11.0] - 2026-10-13

### Added

- `pfPost(url, body, opts, ctx)`: posts without a form of its own (an
  action with a few fields), answered as `pfSubmit()` is; `pfSubmit()` is
  built on it. Both use an `XMLHttpRequest`, so a file upload shows its
  progress in the answer line. `pfRestarting(text, url)`: where to go once
  the device is back (`restartUrl` in the options)

### Changed

- The Syslog, MQTT and Other pages post in the page too, like the Backup
  page since 0.10.0. A refused Syslog or MQTT field is outlined and named
  below the Save button (`no_host`, `bad_host`, `bad_port`, `bad_severity`,
  `bad_facility`; `password_without_user`), the former text as the
  message; the "Configuration rejected" page, which could not say what was
  wrong, is no longer used by the library. On the Other page a password
  change signs out from the page, a restart and a firmware update cover the
  page until the device is back and then open the Status page, and a
  refused firmware (`fw_mismatch`, `bad_file`) is named below the Update
  button. The factory reset keeps its own page: the credentials are gone
  and the address may be. `CONFIG_PORTAL_SAVED_HTML`,
  `CONFIG_PORTAL_SAVE_FAILED_HTML`, `CONFIG_PORTAL_REBOOT_HTML` and the
  other result pages stay for applications' plain-post routes
- An MQTT save that changes the connection answers with "restart" when the
  application said it restarts (`setRestartOnSave()`), so the page waits
  for it
- A required field left empty is no longer outlined before the user has
  touched it (`:user-invalid` instead of `:invalid` in the stylesheet): the
  Backup page's file inputs were red from the start

## [0.10.0] - 2026-10-13

### Added

- Forms posted with `fetch()`, so the page stays and what the user typed is
  kept: `pfSubmit()` in `/common.js` posts a form (multipart if it has a file
  input) and shows the answer in a `.pmsg` line below its button, a refused
  field outlined where the page maps the code to it; `pfMsg()`, `PF_ERR`.
  An answer that is not JSON is understood too (a 2xx page as success,
  otherwise its text), so handlers can move over one by one
- `sendResult()` / `sendResult_P()`: the answer to such a post,
  `{"result":"ok"}` or the refused code, with an index, a message and a
  restart flag where they apply. Built in the buffer the JSON endpoints
  share (no stack, no new static RAM); a variant takes the caller's buffer
  for longer messages, sized with `resultJsonLen()`. A message that does not
  fit is cut at a character with "…"; the code never is.
  `test/result_harness.cpp`: host tests
- `pfRestarting()`: covers the page while the device restarts, reloads it
  once the device answers again, and says so if it does not come back at
  that address
- `NetConfigComponent::setRestartOnSave()`,
  `MqttConfigComponent::setRestartOnSave()`: the application tells the
  component that it restarts after a change, so a restore can say so
- `.pmsg` in the stylesheet (`docs/CSS.md`)

### Changed

- Backup page: a restore is posted with `fetch()` and answered below its
  Restore button instead of with a separate page. The "Configuration saved"
  page was the wrong text for a restore, an error came as a bare text page,
  and a restore that restarts the device sent the browser back to a page
  that was gone. A restore that restarts the device now covers the page until
  it is back, then reloads it. The download is unchanged
- The Network, MQTT and Syslog restores answer with `sendResult()`: `ok`, or
  `abandoned` (409), `too_large` (413), `no_file`, `not_a_backup`,
  `need_password`, `refused` (400), `busy`, `storage_error` (500), with the
  former text as the message. Applications with a Backup section of their own
  may keep answering with `CONFIG_PORTAL_SAVED_HTML` or plain text; the page
  shows those too

### Fixed

- `CONFIG_PORTAL_JSON_ENDPOINT_LEN` was documented as a stack buffer in the
  request handler. It is one static buffer: raising it costs static RAM, not
  stack

## [0.9.2] - 2026-10-13

### Fixed

- `NetConfigComponent` restore: the NTP servers of a backup were checked only
  after the network profiles had been written, so an invalid NTP server
  refused the restore with the profiles already changed; and `onSaved()` ran
  before the NTP servers were applied, with their change missing from the
  set. They are now checked with the profiles before anything is written,
  applied last, and marked in the change set `onSaved()` receives
- The documentation of `NetConfigComponent::wifiCredentialGate()` and
  `loadFromJson()` still described the earlier restore form with a single
  password field, where an empty password meant an open network. The form has
  a "Requires password" box and a password field, as on `/net`: unticked means
  open, a filled field is the password, and a ticked box with an empty field
  keeps the stored password (refused for a new SSID). The behaviour is
  unchanged

## [0.9.1] - 2026-10-13

### Fixed

- The form builders of `/fields.js` (`ifRow()`, `numRow()`, `selectRow()`)
  put values into the markup unescaped. A stored value with a quote in it
  (an SSID such as `My "Home"`, an MQTT user name) was cut at the quote, so
  saving the page stored the truncated value, and markup in a value was
  rendered as markup. Values, placeholders, titles and option texts are now
  escaped (`fvEsc()`); labels stay as given, since they come from the page

## [0.9.0] - 2026-10-12

### Security

- Cross-site request forgery is refused. Digest credentials stored in a
  browser are added to any request to the device, also to a form another site
  submits, so such a page could restart the device, change its settings or
  upload firmware. Every state-changing request (not GET, HEAD or OPTIONS) is
  now checked by a server-wide middleware before its handler runs: its
  `Origin`, or without one its `Referer`, must name the `Host` it was sent
  to, or the answer is 403. Uploads (`addUploadHandler()`, `/update`) are
  checked chunk by chunk too, since the server hands over upload data before
  the middleware runs; a refused firmware upload writes nothing

### Added

- `setOriginCheck()` and `OriginCheck`: `Strict` (the default: a request
  with neither header is refused), `Lenient` (let through) or `Off`
- `test/origin_harness.cpp`: host tests for the check

### Changed

- `EthConfigButton` example: the Network page could save the configuration
  address. The page and the adapter shared one Ethernet profile, so in config
  mode the page showed `CONFIG_IP` as a static address, and a save that
  changed only the hostname, or left the DHCP box as shown, stored it for good.
  The page now edits its own profile (`savedProfile`), loaded from the same
  namespace, and the configuration address goes only into the adapter's
  (`runtimeProfile`)

## [0.8.0] - 2026-10-07

### Added

- The Net status page shows the device's own MAC address, from the `"mac"` field
  NetworkManager now reports. Placed above the IP, because it is the address
  that does not change: a DHCP lease moves, and the MAC is how the device is
  found again afterwards.

  Distinct from the BSSID already shown under Connection, which identifies the
  access point rather than the device. Needs NetworkManager with `"mac"` in
  `statusToJson()`; the row is simply absent against an older one.

## [0.7.0] - 2026-10-04

### Added

- `AsyncConfigPortal::systemStats(SystemStats&)`: the Status page's system
  figures as a public, static call. An application's own logging — a periodic
  health line, a boot report — then gives the same numbers as the page from the
  same calls, instead of a second computation that can drift from it. Fields a
  platform cannot provide are flagged (`hasHeapMin`, `hasTemp`) rather than
  zeroed.

### Changed

- `statusJson()` is built on `systemStats()`. The `/statusdata` document is
  unchanged, key for key.

## [0.6.0] - 2026-10-01

### Added

- `MQTT_CONFIG_TLS` (default 1) controls whether the MQTT page offers TLS at
  all. With it off the row is gone, the port hint stops mentioning 8883, and a
  submitted `tls` parameter is ignored — a stored flag is left as it is rather
  than cleared, so a project that later builds TLS back in finds it intact.

  A compile-time macro rather than an `attach()` parameter like
  `withHaDiscovery`, and the difference is not stylistic: that flag hides a form
  row, this one has to agree with whether the application instantiated a secure
  client at all. A runtime bool could disagree with the build, leaving the page
  offering TLS the firmware cannot open and a user believing a connection is
  encrypted when it is not.

  Worth considering on ESP8266 for a reason larger than the existing warning
  suggests. Constructing a `WiFiClientSecure` calls `stack_thunk_add_ref()`,
  which reserves BearSSL's call stack from the heap at construction, not at
  connect — so a global instance costs about 6.8 kB from boot whether or not a
  secured connection is ever opened, and it lands mid-heap, halving the largest
  free block. Measured on one firmware by removing a secure client it never
  used: boot heap 25.0 kB to 31.8 kB, largest block at MQTT connect 11.8 kB to
  24.8 kB.

## [0.5.2] - 2026-09-20

### Fixed

- `sendProgmemLine()` no longer calls `send_P()`, which on ESP8266 forwards to
  the deprecated `beginResponse_P()` — and that overload's deprecation attribute
  has no platform guard, so the correct ESP8266 call warned from inside
  ESPAsyncWebServer's own header.

  The pointer-and-length overload of `beginResponse()` replaces it on both
  platforms: it builds an `AsyncProgmemResponse`, which fills its buffer with
  `memcpy_P` and so reads flash correctly on ESP8266 and ordinary memory on
  ESP32. The platform split in this function is gone.

  Not the `char*` overload, which without a template callback builds an
  `AsyncBasicResponse` that byte-reads the pointer and would fault on a flash
  address.

## [0.5.1] - 2026-09-12

### Added

- `setCacheTag(uint32_t)` and the constexpr `cacheTagOf(const char*)`: the
  ETag the portal validates its assets and the `/menu` document against can
  now be a build stamp rather than `FIRMWARE_VERSION`. During development the
  version does not move between builds, so a page added to the menu stayed
  invisible to a browser holding the old one — and a hard reload does not
  reach a script-fetched `/menu`. `cacheTagOf(__DATE__ " " __TIME__)` from
  the application's own translation unit changes exactly when its pages can.

## [0.5.0] - 2026-09-12

### Added

- `SyslogConfigComponent` (+ `SyslogConfigPages.h`): an opt-in page editing a
  `SyslogProfile` — server, port, severity floor, facility — with the same
  `setProfile`/`onSaved`/`attach` shape as the MQTT page, a factory-reset hook
  for its namespace, and an optional Backup section (no secrets, so the
  restore needs no extra controls). Requires NetworkProfile 0.9.0.

## [0.4.3] - 2026-09-08

### Fixed

- In the case of multi-core processors, the word "core" should be written in the
  plural form.

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

[Unreleased]: https://github.com/soosp/AsyncConfigPortal/compare/0.13.2...HEAD
[0.13.2]: https://github.com/soosp/AsyncConfigPortal/compare/0.13.1...0.13.2
[0.13.1]: https://github.com/soosp/AsyncConfigPortal/compare/0.13.0...0.13.1
[0.13.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.12.0...0.13.0
[0.12.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.11.0...0.12.0
[0.11.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.10.0...0.11.0
[0.10.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.9.2...0.10.0
[0.9.2]: https://github.com/soosp/AsyncConfigPortal/compare/0.9.1...0.9.2
[0.9.1]: https://github.com/soosp/AsyncConfigPortal/compare/0.9.0...0.9.1
[0.9.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.8.0...0.9.0
[0.8.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.7.0...0.8.0
[0.7.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.6.0...0.7.0
[0.6.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.5.2...0.6.0
[0.5.2]: https://github.com/soosp/AsyncConfigPortal/compare/0.5.1...0.5.2
[0.5.1]: https://github.com/soosp/AsyncConfigPortal/compare/0.5.0...0.5.1
[0.5.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.4.3...0.5.0
[0.4.3]: https://github.com/soosp/AsyncConfigPortal/compare/0.4.2...0.4.3
[0.4.2]: https://github.com/soosp/AsyncConfigPortal/compare/0.4.1...0.4.2
[0.4.1]: https://github.com/soosp/AsyncConfigPortal/compare/0.4.0...0.4.1
[0.4.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.3.0...0.4.0
[0.3.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.2.1...0.3.0
[0.2.1]: https://github.com/soosp/AsyncConfigPortal/compare/0.2.0...0.2.1
[0.2.0]: https://github.com/soosp/AsyncConfigPortal/compare/0.1.0...0.2.0
[0.1.0]: https://github.com/soosp/AsyncConfigPortal/releases/tag/0.1.0
