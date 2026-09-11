#pragma once

/**
 * @file SyslogConfigPages.h
 * @brief Page body for the optional syslog configuration component.
 *
 * Internal to SyslogConfigComponent — include that, not this.
 *
 * The form is built client-side from /syslogdata with the shared field
 * builders, so it looks and validates like the built-in pages.
 */

#include "ConfigWebPages.h"

static const char CONFIG_PORTAL_SYSLOG_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Syslog</title><link rel="stylesheet" href="/css">
<script src="/common.js"></script>
<script src="/fields.js"></script>
<script>
// Page-specific rules. Everything is conditional on the switch: settings that
// are not in effect are not worth blocking a save over.
function syslogRules(v) {
  if (!ID('enabled').checked) return;
  var host = v.value('host');
  if (!host)                v.fail('host', 'Required');
  else if (!fvIsHost(host)) v.fail('host', 'Enter a host name or an IP address');
  var port = parseInt(v.value('port'), 10);
  if (!(port >= 1 && port <= 65535)) v.fail('port', 'Between 1 and 65535');
}

// RFC 5424 codes: a smaller number is more severe. The menu is listed from
// the most severe down so the floor reads as "this and everything above it".
var SEVS = [
  {v:3, t:'error — errors only'},
  {v:4, t:'warning — errors and warnings'},
  {v:5, t:'notice'},
  {v:6, t:'info — errors, warnings and milestones'},
  {v:7, t:'debug — everything'}
];
var FACS = [{v:1, t:'user'}];
for (var i = 0; i < 8; i++) FACS.push({v: 16 + i, t: 'local' + i});

function build(o) {
  var h = '';
  h += '<div class="group"><div class="group-title">Server</div>';

  // The switch comes first: everything below it is what it switches.
  h += '<div class="row"><span class="label"><label for="enabled">Enabled</label>'
     + '</span><span class="value"><input type="checkbox" id="enabled" name="enabled"'
     + (o.enabled ? ' checked' : '') + '></span></div>';

  h += '<div id="syslogfields">';
  h += ifRow('', 'Host', 'host', o.host, 'syslog.example.org',
             {host: true, title: 'Syslog server host name or IP address'});
  h += numRow('', 'Port', 'port', o.port, 1, 65535, 1, 'UDP; usually 514');
  h += selectRow('', 'Send', 'sev', o.minSeverity, SEVS,
                 {title: 'Lowest severity forwarded; less severe lines stay on the console'});
  h += selectRow('', 'Facility', 'fac', o.facility, FACS,
                 {title: 'RFC 5424 facility the server files these lines under'});
  h += '</div></div>';

  ID('syslogform').innerHTML = h;
  ID('enabled').addEventListener('change', dim);
  dim();
  attachValidation('syslogform', 'saveBtn', syslogRules);
}

// Stored but not in effect: dimmed, not disabled. A disabled input is not
// submitted, so switching syslog off would erase the settings it was switched
// off with — and the fields stay editable, so a server can be filled in before
// it is switched on.
function dim() {
  var f = ID('syslogfields');
  if (ID('enabled').checked) f.classList.remove('dim');
  else                       f.classList.add('dim');
  validateForm('syslogform', 'saveBtn', syslogRules);
}

function init() { initCommon(); getJSON('/syslogdata', build); }
</script>
</head><body onload="init()">
)HTML"
CONFIG_PORTAL_HEADER
R"HTML(
<div class="content">
  <form method="post" action="/syslog">
    <div id="syslogform"></div>
    <div class="submit"><input type="submit" id="saveBtn" value="Save"></div>
  </form>
</div>
)HTML"
CONFIG_PORTAL_FOOTER
R"HTML(
</body></html>
)HTML";
