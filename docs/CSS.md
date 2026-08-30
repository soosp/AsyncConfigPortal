# Styling the portal

The pages never reference the stylesheet, only class names. So restyling is a
matter of replacing the stylesheet while keeping those class names meaning what
the pages expect — that contract is what this document writes down.

Every page loads the same `/css`, the built-in ones included, so styling is
portal-wide by construction — there is nothing to repeat per page. Two ways in,
both set from `setup()`, before `begin()`:

1. **Supplement it.** `setCssExtra()` appends a snippet of your own after the
   built-in sheet, so your rules win by cascade order and you only write what
   differs. Recolouring is ten lines.
2. **Replace it.** `setCss()` serves your stylesheet instead of the built-in one.

```cpp
static const char MY_CSS_EXTRA[] PROGMEM = R"CSS(
:root { --accent: #7b1fa2; --header-bg: #37474f; }
.group { border-radius: 4px; }
)CSS";

void setup() {
    web.setCssExtra(MY_CSS_EXTRA);   // before web.begin()
    ...
}
```

Both must be PROGMEM strings: they are served straight out of flash.

A `<style>` block inside your own page is a third possibility, but it reaches
only that page — the built-in pages are compiled into the library and cannot take
one. Use it when one page genuinely needs to differ, not for a house style.

If the project also uses `AsyncWiFiPortal`, note that the recovery portal runs in
its own boot, where this server does not exist, so it cannot inherit anything set
here. It takes the same two setters; pass the same string to both and the two
look alike.

Which of the two to pick, and what a replacement takes on, is the next section.

## The tab icon

`/favicon.ico` is registered whether or not there is an icon; with none it
answers 204, which stops the browser retrying. `setFavicon()` fills it in:

```cpp
static const char MY_ICON[] PROGMEM = R"SVG(<svg …</svg>)SVG";

web.setFavicon(MY_ICON);                       // image/svg+xml
web.setFavicon(ICO_BYTES, sizeof(ICO_BYTES),   // or anything else
               "image/x-icon");
```

The content type is what decides how the browser reads the body, not the `.ico`
in the path, so an SVG served here works. SVG is usually the better choice on a
device: a few hundred bytes of path data against several kilobytes for the
equivalent PNG, and it scales to whatever size is asked for. The length-taking
form exists because a binary body ends at its first zero byte if the length is
derived with `strlen_P()`.

A `<link rel="icon">` data URI in a page's head is the alternative, and a worse
one here: the head is per page, so it repeats in every one of them and still
leaves the built-in pages without an icon. The endpoint stores the bytes once,
covers every page, and carries the same ETag as the other static assets, so a
browser fetches it once per firmware version.

## Which one to use

Use the **supplement** for anything short of a complete stylesheet, and a **full
replacement** when you are writing the whole sheet. Handing a complete sheet to
`setCssExtra()` mostly works, but the two are not interchangeable:

- **It is not a clean slate.** Built-in rules you did not override stay in
  effect. A selector you forgot does not fall back to unstyled; it falls back to
  the built-in look, which is a quieter kind of mistake to find.
- **Some built-in rules survive a same-name override**, because they are more
  specific or conditional: `.value input[readonly]`, `.value input:disabled`,
  `.label.required::after`, `body.stale .value`, `[hidden]`, and the
  `@media (max-width: 480px)` block. A plain `.row { ... }` of yours will not beat
  the media query's `.row` on a narrow screen.
- **You pay twice.** Both sheets are sent on every page load, and both are
  compiled in — about 3.8 KB for the built-in one.

The supplement is the safer of the two in one respect, which is worth knowing
before you reach for a replacement: the load-bearing rules cannot be lost.
`[hidden] { display: none !important; }` and `.field-error { display: none; }`
stay in place. Replace the sheet and leave either out, and the pages will show
what they meant to hide and every error message from the start — a silent
functional break, not a visual one. That is why the class contract below is
worth reading in full before choosing replacement.

### Why setters rather than macros

The handler that serves `/css` lives in the library's own translation unit. A
`#define` in a sketch never reaches it, so it would silently do nothing; passing
the macro through build flags instead makes that unit name a symbol it cannot
see, which fails to compile. A pointer set at runtime has neither problem, and
behaves the same in the Arduino IDE and in PlatformIO.

### Getting the built-in sheet out of flash

`setCss()` stops the built-in sheet being *sent*, but it is still compiled in.
Dropping it entirely means defining `CONFIG_PORTAL_CSS` at compile time, which
needs both the macro and the stylesheet it names to reach the library's
translation unit — **PlatformIO only**, through a forced include:

```ini
build_flags =
    -include mycss.h
```

with `mycss.h` next to the sketch, holding both the `PROGMEM` stylesheet and the
`#define CONFIG_PORTAL_CSS`.

The Arduino IDE has no equivalent: its build-options file is copied into the
build folder and handed to the compiler from there, so a relative `-include`
would not resolve against your sketch folder. On the IDE the setters are the only
route. Anyone replacing the sheet outright is likely to be on PlatformIO anyway.

## Variables

Declared on `:root`; everything else in the built-in sheet is expressed in terms
of them, which is why a supplement that redefines them recolours every page,
built-in ones included.

|Variable|Default|Used for|
|---|---|---|
|`--bg`|`#eef3f7`|page background|
|`--fg`|`#1a2b3c`|body text|
|`--muted`|`#5b6b7b`|field labels, footer, disabled input text|
|`--accent`|`#2c6faa`|group titles, submit button|
|`--header-bg`|`#2c3e50`|header bar, dialog border|
|`--header-fg`|`#ffffff`|header text and menu links|
|`--card-bg`|`#ffffff`|group and dialog background|
|`--border`|`#d0d7de`|group and input borders|
|`--ok`|`#2e7d32`|`.ok` state text|
|`--warn`|`#ef6c00`|`.warn` state text — a value that works but has no margin|
|`--alarm`|`#c62828`|`.alarm` text, invalid fields, required marker|

A dark theme is a matter of redefining these ten in a supplement:

```cpp
static const char MY_CSS_EXTRA[] PROGMEM = R"CSS(
:root { --bg:#12181f; --fg:#e6edf3; --card-bg:#1b232c; --border:#30363d;
        --muted:#9aa7b2; --header-bg:#0d1117; --accent:#58a6ff; }
)CSS";
web.setCssExtra(MY_CSS_EXTRA);   // in setup()
```

## Class contract

These names are what the built-in pages, the field builders in `/fields.js` and
the connection watchdog in `/common.js` actually emit. A replacement stylesheet
must style them; it may add anything else it likes.

### Page frame

|Class|Element|Notes|
|---|---|---|
|`.header`|header bar|holds the title and menu|
|`.menu`, `.menu a`|navigation|built from `/menu` at runtime|
|`.logout`|logout link|shown only while authenticated|
|`.content`|main column|width limit lives here|
|`.footer`|project line|filled from `/project`|

### Content blocks

|Class|Element|Notes|
|---|---|---|
|`.group`|a card|the visual unit pages are built from|
|`.group-title`|its heading|the card's own title row|
|`.row`|one label + value pair|becomes a column below 480 px|
|`.label`|left half of a row|the field name|
|`.value`|right half|also wraps inputs and selects|

A page is a stack of `.group` blocks, each a stack of `.row`s. That is the whole
layout system; the built-in pages use nothing else.

### Forms and validation

|Class|Applied by|Meaning|
|---|---|---|
|`.label.required`|field builder|renders the `*` marker via `::after`|
|`.invalid`|validation engine|on the input or select that failed|
|`.field-error`|field builder|the per-field message; **hidden by default**, the engine sets `display`|
|`.hint`|page|explanatory line under a field, in muted text|
|`.submit`|page|wrapper around the submit control|

`.field-error` being `display:none` in the sheet is load-bearing: the engine
shows and hides it, so a replacement must keep it hidden by default or every
error message will be visible from the start.

### Showing and hiding

Structure is shown and hidden with the **`hidden` attribute**, not a class:
`el.hidden = true`. It is semantic, needs no class of your own, and both
libraries use it, so a page behaves the same wherever it came from.

The one rule that makes it work must be in the sheet:

```css
[hidden] { display: none !important; }
```

The user agent's own `[hidden]` rule sits at the weakest weight, so any
`display:` rule in the sheet beats it — `.row` is `display:flex`, which means a
hidden row would otherwise still be visible. This is the one place `!important`
is justified, and a replacement sheet that drops it will show everything the
pages meant to hide.

`style.display` is used for a different job: state a script switches at
runtime —
the offline banner, the Logout link, a modal, a field's error message. Those are
not "hide part of the structure", so they are left as they are.

### States

|Class|Applied by|Meaning|
|---|---|---|
|`.ok` / `.warn` / `.alarm`|page|good / marginal / bad value colouring|
|`.dim`|page|settings that are stored but not currently in effect|
|`.offline`|watchdog|the "device not responding" banner; **hidden by default**|
|`body.stale`|watchdog|dims `.value`, `.card` and `#clock` while offline|
|`.alert-container`, `.alert-backdrop`, `.alert-content`|page|modal dialog parts|

`.dim` is how a page shows settings that are switched off. The alternative —
`disabled` on the inputs — looks the same and behaves differently: a disabled
input is not submitted, so saving a form with a feature switched off would erase
the settings it was switched off with. Dimming keeps them editable and submitted.

The offline pair carries meaning, not just decoration. The banner states the
problem; the dimming is what makes staleness visible at a glance — a banner alone
is easy to scan past, while greyed-out readings stay legible but obviously are
not live. A replacement sheet that drops `body.stale` will show stale values as
if they were current.

## Practical notes

- **Keep it small.** The sheet is served on every page load and lives in flash
  alongside the pages. The built-in one is about 3 KB; a supplement adds its own
  size on top, which is why supplementing beats copying.
- **Cascade order is the mechanism.** The supplement is appended, so equal-weight
  rules later in the file win. You do not need `!important`, and you do not need
  to match the original selectors exactly — only to be at least as specific.
- **The header and footer macros are fragments.** `CONFIG_PORTAL_FOOTER` does not
  close `</body>` or `</html>`; pages supply those.
- **Responsive behaviour is one media query.** Below 480 px `.row` switches to a
  column so labels sit above values. Dropping it makes narrow phones unusable.
- **`/fields.js` and `/common.js` are separate.** Restyling does not require
  touching them; conversely, replacing the stylesheet does not change any
  behaviour, only appearance.
