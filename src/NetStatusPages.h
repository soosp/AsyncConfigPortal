#pragma once

/**
 * @file NetStatusPages.h
 * @brief Page body for the optional Network status component.
 *
 * Internal to NetStatusComponent — include that, not this. Kept separate for the
 * same reason as NetConfigPages.h: a page this size is easier to read on its own
 * than embedded in the component that registers it.
 *
 * The page renders whatever NetworkManager::statusToJson() produced. Fields that
 * interface does not have are simply absent from the document, and a row whose
 * value is absent is removed rather than shown empty — an Ethernet link has no
 * SSID, and an empty "SSID:" row would suggest it lost one.
 */

#include "ConfigWebPages.h"   // base pages: CONFIG_PORTAL_HEADER / _FOOTER, /css, common.js

static const char CONFIG_PORTAL_NETSTATUS_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Network status</title><link rel="stylesheet" href="/css">
<script src="/common.js"></script>
<script>
// Signal quality bands. -67 dBm is the usual floor for traffic that does not
// tolerate loss; below about -77 the link is close enough to the noise floor to
// stall under load. Override the two numbers to taste.
var RSSI_GOOD = -67, RSSI_FAIR = -77;

// Adds a row to a group, or nothing at all when the value is absent. Building
// rows instead of hiding pre-written ones keeps the list of servers open-ended:
// the document decides how many there are.
function row(g, label, value, cls) {
  if (value === undefined || value === null || value === '') return;
  var d = document.createElement('div'); d.className = 'row';
  var l = document.createElement('span'); l.className = 'label'; l.textContent = label + ':';
  var v = document.createElement('span'); v.className = 'value' + (cls ? ' ' + cls : '');
  v.textContent = value;
  d.appendChild(l); d.appendChild(v); g.appendChild(d);
}

function group(title) {
  var g = document.createElement('div'); g.className = 'group';
  var h = document.createElement('div'); h.className = 'group-title'; h.textContent = title;
  g.appendChild(h); return g;
}

function rssiClass(r) { return r >= RSSI_GOOD ? 'ok' : (r >= RSSI_FAIR ? 'warn' : 'alarm'); }

function render(doc) {
  var root = ID('netstatus'); root.innerHTML = '';
  var o = doc.net || {};

  var c = group('Connection');
  row(c, 'Interface', o.interface ? o.interface.toUpperCase() : undefined);
  if (o.link) {
    row(c, 'SSID', o.link.ssid);
    if (o.link.rssi !== undefined)
      row(c, 'Signal', o.link.rssi + ' dBm', rssiClass(o.link.rssi));
    row(c, 'BSSID', o.link.bssid);
  }
  root.appendChild(c);

  var a = group('Addressing');
  row(a, 'IP address', o.ip);
  row(a, 'Subnet mask', o.mask);
  row(a, 'Gateway', o.gw);
  if (o.dns) for (var i = 0; i < o.dns.length; i++)
    row(a, o.dns.length > 1 ? 'DNS ' + (i + 1) : 'DNS', o.dns[i]);
  root.appendChild(a);

  if (o.ntp) {
    var t = group('Time');
    row(t, 'Synchronised', o.ntp.synced ? 'yes' : 'no', o.ntp.synced ? 'ok' : 'warn');
    var s = o.ntp.servers || [];
    for (var j = 0; j < s.length; j++) {
      // One row per server. A server may be configured by name and resolved to
      // an address, or given as an address with no name at all — so the address
      // goes in brackets after the name when there is both, and stands alone
      // when there is not. A second row with an empty label would read as a
      // missing field rather than as a continuation, so both go on one line.
      var v = s[j].name
                ? (s[j].ip ? s[j].name + ' (' + s[j].ip + ')' : s[j].name)
                : s[j].ip;
      row(t, s.length > 1 ? 'Server ' + (j + 1) : 'Server', v);
    }
    root.appendChild(t);
  }

  // Application sections, rendered exactly as handed over: the page knows
  // nothing about MQTT or anything else, only that a section has a title and
  // some label/value pairs.
  var secs = doc.sections || [];
  for (var s = 0; s < secs.length; s++) {
    var g = group(secs[s].title);
    var f = secs[s].fields || {};
    for (var k in f) if (f.hasOwnProperty(k)) row(g, k, f[k]);
    root.appendChild(g);
  }
}

function init() {
  initCommon();
  getJSON('/netstatusdata', render);
  setInterval(function () { getJSON('/netstatusdata', render); }, 5000);
}
</script>
</head><body onload="init()">
)HTML"
CONFIG_PORTAL_HEADER
R"HTML(
<div class="content">
  <div id="netstatus"></div>
</div>
)HTML"
CONFIG_PORTAL_FOOTER
R"HTML(
</body></html>
)HTML";
