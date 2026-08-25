# AsyncConfigPortal

A reusable, modular web-configuration server for ESP32 and ESP8266 (Arduino).
Provides HTTP
digest auth, a dynamic menu, a Status page, an Other page (OTA with
firmware-marker validation, restart, factory reset, password change), a Backup
page that components register sections on, and a composition API for
project-specific pages. Ships an **optional, opt-in** network-config module that
adds no mandatory dependency.

See [`docs/DESIGN.md`](docs/DESIGN.md) for the architecture, the design decisions
behind the boundary, and the pitfalls worth knowing before you extend it, and
[`docs/CSS.md`](docs/CSS.md) if you want the pages to look like yours.

## Dependencies

Both manifests are shipped, and they name the same libraries differently:
`library.json` (PlatformIO) takes an `owner/name`, `library.properties` (Arduino
Library Manager) takes the library's own `name=` field. Both are listed, because
installing the wrong fork is the likeliest way to have this not build.

|PlatformIO|Arduino Library Manager|Notes|
|---|---|---|
|`ESP32Async/AsyncTCP`|`Async TCP`|ESP32 only|
|`ESP32Async/ESPAsyncTCP`|`ESP Async TCP`|ESP8266 only|
|`ESP32Async/ESPAsyncWebServer`|`ESP Async WebServer`|the async HTTP server, both platforms|
|`soosp/HttpDigestAuth`|`HttpDigestAuth`|credential storage + digest auth|

Pin the ESP32Async forks. Several incompatible async-TCP libraries sit in the
Library Manager under names a glance cannot tell apart — `Async TCP`,
`ESP Async TCP`, `ESP AsyncTCP` — and the web server declares no `depends=` of
its own, so nothing installs the right one for you. This library's
`library.properties` names both TCP layers for that reason; the one your board
does not use is installed but never compiled, since nothing includes its header.

The optional net module additionally needs the **NetworkProfile** family — but
only if you use it (see below).

## Minimal usage

```cpp
#include <AsyncConfigPortal.h>
#include <HttpDigestAuth.h>

AsyncConfigPortal web(80);
HttpDigestAuth    auth;

void setup() {
    // bring up ETH / Wi-Fi first, then:
    if (!auth.restore()) { auth.setCredentials("admin", "esp32", "admin"); auth.save(); }
    web.setProject("MyProject", "1.0.0", "desc", "2026", "author");
    web.setLogger([](AsyncConfigPortal::LogLevel lvl, const char* tag, const char* msg){
        // tag names the subsystem ("ota", ...) and can be passed straight to
        // ESP_LOGx, which accepts a runtime tag — so the log stays filterable
        // per subsystem with esp_log_level_set().
        ESP_LOGI(tag, "%s", msg);   // map lvl to ESP_LOGE/W/I/D as you like
    });
    web.begin(auth);
}
```

The pages carry no inline styling: they use the shared class names, so your page
looks like the built-in ones for free. A house style is one call —
`setCssExtra(MY_CSS)` appends your snippet to the stylesheet every page loads,
the built-in ones included, so ten redefined colour variables restyle the whole
portal; `setCss()` replaces the sheet outright if you want that far.
[`docs/CSS.md`](docs/CSS.md) documents both, and the class contract.

Saving is answered with the shared result pages,
`CONFIG_PORTAL_SAVED_HTML` and `CONFIG_PORTAL_SAVE_FAILED_HTML`: both return to
the page the POST came from, so a component serves them as they are and does not
name its own path. (The Other page's own results — reboot, factory reset — go
elsewhere on purpose: after a restart, "back where you came from" is the wrong
answer.)

Two things worth knowing when you compose a page:

- `CONFIG_PORTAL_HEADER` / `CONFIG_PORTAL_FOOTER` are fragments. The footer ends
  after the project block and does **not** close `</body>` or `</html>` — your
  page adds those, which is what lets you put your own scripts after it.
- `addResetHandler(what, fn)` registers one factory-reset target. Without any
  handler the button still works: the server erases what it owns (the stored
  credentials) and restarts. It never silently does nothing — but it also never
  guesses at your application's storage, so register a handler for each namespace
  you want cleared.

See `examples/Minimal` for a complete sketch. Extend the server with your own
pages via `addPage()`, `addJsonEndpoint()`, `addPostHandler()`, `addBackupSection()`
and `addResetHandler()`. A form page can build validated fields with the shared
builders served at `/fields.js` — `ifRow` (text), `numRow` (number) and
`selectRow` (dropdown) — the same ones the built-in pages use. `examples/CustomPage`
is a complete, hardware-free walkthrough of composing your own page this way.

## Platform differences

The same code runs on both platforms; where the hardware differs, so does the
page. Nothing here needs configuring — it is what you will see:

- **Status page.** ESP8266 shows no *Temperature* and no *Min heap* row: it has
  neither a die-temperature sensor nor minimum-heap tracking. The rows are hidden
  rather than filled with zeros, because a reading that does not exist is not the
  same fact as a reading of zero.
- **Wi-Fi TX power** (net module). A drop-down of the radio's discrete levels on
  ESP32; a bounded number input on ESP8266, whose output power is a continuous
  range.
- **Storage.** `Preferences` is part of the core on ESP32, backed by the NVS
  flash partition. On ESP8266 the `vshymanskyy/Preferences` library provides the
  same API over the internal flash filesystem, storing each entry as a file under
  `/nvs/`; **LittleFS is the default driver, so the build needs a filesystem in
  its flash layout**. Its `begin()` takes no `partition_label`, and keys are
  limited to 15 characters.
- **Read the Status page's *Max alloc*, not its *Fragmentation*.** The largest
  block that can be allocated in one piece is what decides whether a page is
  built; the percentage is `100 x (1 - max alloc / free heap)` and only compares
  with itself under similar load. Freeing memory raises free heap without
  enlarging the largest block, so the figure rises while the heap improves — a
  reading of 45% at rest can be healthier than 13% just after a page load.
- **TLS on ESP8266 is tight.** A BearSSL client reserves about 6 KB the moment it
  is created and its connection buffers are ~22 KB by default, against roughly
  40 KB of heap in total — so a device also serving this portal will not fit both
  unless all three of these hold: the server agrees to MFLN (`setBufferSizes(512,
  512)` after `probeMaxFragmentLength()`), `HOST_FQDN_LEN` is lowered (64 is
  ample in practice, and it shrinks the portal's largest document), and the build
  uses the basic SSL cipher set — Arduino IDE: *Tools > SSL Support > Basic SSL
  ciphers*; PlatformIO: `build_flags = -D BEARSSL_SSL_BASIC`.

  Worth knowing because the failure mode is unkind: with TLS up and too little
  left, pages come up blank, and the setting that caused it can no longer be
  changed from the portal. An application offering TLS on ESP8266 should measure
  the largest free block once the connection stands and back out if it is too
  small — `examples/NetConfig` shows the check. ESP32 has none of these limits.
- **RAM.** ESP8266 has far less of it, and the portal is built to fit: pages
  stream straight out of flash and large JSON documents are streamed from their
  buffer rather than copied. Raising `NET_CONFIG_MAX_PROFILES` or
  `NTP_PROFILE_SERVER_COUNT` (1 to 3) grows the largest buffer, so raise them
  only
  as far as the device actually needs.

## Optional modules

Three ready-made pages ship with the library. Each is **header-only** and
**opt-in**: including it is what pulls in the library it needs, and a build that
never includes it has that dependency at all.

|Include|Page|Pulls in|
|---|---|---|
|`<NetConfigComponent.h>`|Network — addressing, DNS, hostname, Wi-Fi, and the NTP servers|NetworkProfile (`NtpProfile` for the Time group, which needs `NTP_PROFILE_ENABLED`)|
|`<MqttConfigComponent.h>`|MQTT — broker, port, TLS, credentials|NetworkProfile (`MqttProfile`)|
|`<NetStatusComponent.h>`|Net status — the live link, read-only|NetworkManager|

They share the same shape, so knowing one is knowing all three:

```cpp
component.attach(web, order, label /*, component-specific */);   // returns bool
```

`attach()` returns false if a registration failed — the page registry is finite,
and a component that could not register would otherwise look attached.

Three registries have a fixed size, each overridable before including the
header. They are small on purpose: the arrays are members of the server object,
so an unused slot is RAM spent on a page nobody registered.

|Macro|Default|Bounds|
|---|---|---|
|`CONFIG_PORTAL_MAX_PAGES`|16|every built-in page, module page and project page takes one|
|`CONFIG_PORTAL_MAX_BACKUP_SECTIONS`|4|`addBackupSection()`|
|`CONFIG_PORTAL_MAX_RESET_HANDLERS`|4|`addResetHandler()`|

Each `add*()` returns false rather than overwriting when its registry is full,
so a build that outgrows one says so instead of losing a page quietly.

### Network and MQTT

Both edit a stored profile and take an optional Backup section of their own:

```cpp
netConfig.addProfile(ethProfile, "eth");    // one or more

// The NTP servers are edited in a Time group on the same page, from a
// device-level NtpProfile — one clock, one set of servers, rather than a copy in
// every interface. Not calling this leaves the group off the page.
//
// One profile, which is the ordinary case. NetworkProfile also allows one per
// interface (NetworkAdapter::setNtpProfile), for a wired LAN with an internal
// time server a fallback cannot reach; this page does not edit those.
netConfig.setNtpProfile(ntpProfile, "ntp");

netConfig.attach(web, AsyncConfigPortal::MENU_NET, "Network", /*withBackup=*/true);

mqtt.setProfile(mqttProfile, "mqtt");       // exactly one
mqtt.onSaved([](const MqttConfigComponent::Changed& c) {
    if (c.connection) ESP.restart();        // the client is built from these
});
mqtt.attach(web, AsyncConfigPortal::MENU_NET - 1, "MQTT", /*withBackup=*/true);
```

Neither owns a client or a connection: they save, and report what changed. What
to do about it is the application's call, since the client belongs to it.

### Net status

Read-only, and open by default — checking a signal level is the sort of thing one
does standing next to the device. Pass `AuthLevel::Required` where the SSID,
addressing and BSSID should not be readable without a login.

It shows what NetworkManager knows. Anything else is the application's state, so
the page takes sections rather than learning about each service:

```cpp
netStatus.addSection("MQTT", [](char* buf, size_t len) {
    if (!enabled) return false;             // section omitted entirely
    return snprintf(buf, len, "{\"Broker\":\"%s\",\"State\":\"%s\"}",
                    host, mqtt.connected() ? "connected" : "offline") < (int)len;
});
netStatus.attach(web);
```

### Writing your own instead

`NetConfigComponent` ships in the library as a ready, opt-in managed
network-config page. It is **header-only** and pulls in the NetworkProfile family
**only when you include it**:

```cpp
#include <NetConfigComponent.h>   // opt in
```

and add the NetworkProfile library to *your* `lib_deps`. If you never include it,
there is no NetworkProfile dependency. If you want your own page instead, write
it against the composition API — that is what these three are built on.

> ⚠️ **Two constraints keep this dependency-free** — see DESIGN.md:
>
> 1. Keep `NetConfigComponent` header-only.
> 2. Use `lib_ldf_mode = chain` (the PlatformIO default). `deep`/`deep+` scan all
>    headers and would force-resolve `NetworkProfile.h` even when unused.

## Setting build macros

The library's tunables are macros read by library sources, not by your sketch.
Under **PlatformIO** put them in `build_flags` — one mechanism, both platforms:

```ini
build_flags =
    -DNET_CONFIG_MAX_PROFILES=2
    -DNTP_PROFILE_SERVER_COUNT=3
```

Under the **Arduino IDE** a `#define` in the sketch will *not* reach them:
library sources are separate translation units. Each core has its own file for
this, and they are not interchangeable:

|Core|File in the sketch folder|Contents|
|---|---|---|
|ESP32|`build_opt.h`|compiler flags, one per line, **no comments**|
|ESP8266|`<sketch>.ino.globals.h`|a `/* @create-file:build.opt@ ... */` block holding the flags|

```txt
// build_opt.h (ESP32)
-DNET_CONFIG_MAX_PROFILES=2
-DNTP_PROFILE_SERVER_COUNT=3
```

The ESP32 examples ship a `build_opt.h`; the accompanying sketch documents what
each flag is for. The ESP8266 counterpart is **not** shipped, because the IDE
hides an example folder that contains a `.ino.globals.h` — write one yourself
when building an example for ESP8266.

Because the two cores read different files, this is also how you give the same
sketch **different flags per platform** under the IDE: each core picks up its own
file and ignores the other's.

> ⚠️ **After the first build, editing the file has no effect on its own.** The
> Arduino IDE does not notice the change and reuses the previous build objects
> and cached core. Force a full rebuild: close the IDE, or change any board menu
> option (the upload method, say) — either invalidates the cache.

Either way the IDE reads the file only from the sketch folder.

## Firmware-marker build flags (OTA)

The OTA path validates an embedded firmware identity so a wrong image is
rejected. Set these per build (defaults exist for a first run):

```ini
build_flags =
    -DFIRMWARE_PROJECT='"my-project"'   ; <= 23 chars
    -DFIRMWARE_BOARD='"my-board"'       ; <= 23 chars
    -DFIRMWARE_VERSION=100              ; major * 10000 + minor * 100 + patch
```

Reference the embedded marker once in your app (e.g. log `g_fwMarker`) so the
linker keeps it.
