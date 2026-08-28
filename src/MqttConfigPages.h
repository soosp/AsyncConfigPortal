#pragma once

/**
 * @file MqttConfigPages.h
 * @brief Page body for the optional MQTT configuration component.
 *
 * Internal to MqttConfigComponent — include that, not this.
 *
 * The form is built client-side from /mqttdata with the shared field builders,
 * so it looks and validates like the built-in pages without restating any markup.
 */

#include "ConfigWebPages.h"

static const char CONFIG_PORTAL_MQTT_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MQTT</title><link rel="stylesheet" href="/css">
<script src="/common.js"></script>
<script src="/fields.js"></script>
<script>
// Page-specific rules, run by the shared engine after the built-in ones. It
// hands in an accessor object; failures are reported through it so custom and
// built-in errors look and behave the same.
//
// Everything is conditional on the switch: settings that are not in effect are
// not worth blocking a save over, and the user may well be filling them in
// before switching MQTT on.
function mqttRules(v) {
  if (!ID('enabled').checked) return;
  var host = v.value('host');
  if (!host)             v.fail('host', 'Required');
  else if (!fvIsHost(host)) v.fail('host', 'Enter a host name or an IP address');
  var port = parseInt(v.value('port'), 10);
  if (!(port >= 1 && port <= 65535)) v.fail('port', 'Between 1 and 65535');
  if (ID('auth').checked && !v.value('user')) v.fail('user', 'Required');
}

function build(o) {
  var h = '';
  h += '<div class="group"><div class="group-title">Broker</div>';

  // The switch comes first: everything below it is what it switches.
  h += '<div class="row"><span class="label"><label for="enabled">Enabled</label>'
     + '</span><span class="value"><input type="checkbox" id="enabled" name="enabled"'
     + (o.enabled ? ' checked' : '') + '></span></div>';

  h += '<div id="mqttfields">';
  h += ifRow('', 'Host', 'host', o.host, 'broker.example.org',
             {host: true, title: 'Broker host name or IP address'});
  h += numRow('', 'Port', 'port', o.port, 1, 65535, 1,
              'Usually 1883, or 8883 with TLS');
  h += '<div class="row"><span class="label"><label for="tls">TLS</label></span>'
     + '<span class="value"><input type="checkbox" id="tls" name="tls"'
     + (o.tls ? ' checked' : '') + '></span></div>'
     + '<div class="hint">Changing this takes effect after a restart: the client '
     + 'is built once, for one kind of connection.</div>';
  // Authentication is a switch of its own rather than "leave the username
  // empty": the two credential fields disappear with it, so the page says at a
  // glance whether the broker is being connected to anonymously.
  h += '<div class="row"><span class="label"><label for="auth">Requires authentication'
     + '</label></span><span class="value"><input type="checkbox" id="auth" name="auth"'
     + (o.user ? ' checked' : '') + '></span></div>';

  h += '<div id="authfields">';
  h += ifRow('', 'Username', 'user', o.user, '',
             {title: 'Username the broker expects'});
  // Never sent to the page, so never sent back unless retyped.
  h += ifRow('', 'Password', 'pass', '', o.hasPassword ? '(unchanged)' : '',
             {type: 'password', title: 'Leave empty to keep the stored password'});
  h += '</div>';

  // Home Assistant discovery, only when the project asked for the row. It sits
  // inside mqttfields so it dims with the rest when MQTT is switched off: it is
  // a layer on top of the connection, not an alternative to it.
  if (o.withHaDiscovery) {
    h += '<div class="row"><span class="label"><label for="hadisc">'
       + 'Home Assistant discovery</label></span>'
       + '<span class="value"><input type="checkbox" id="hadisc" name="hadisc"'
       + (o.haDiscovery ? ' checked' : '') + '></span></div>'
       + '<div class="hint">Publishes retained discovery messages so Home '
       + 'Assistant creates the entities by itself. Switching it off clears '
       + 'them again.</div>';
  }

  h += '</div></div>';

  ID('mqttform').innerHTML = h;
  ID('enabled').addEventListener('change', dim);
  ID('auth').addEventListener('change', dim);
  dim();
  attachValidation('mqttform', 'saveBtn', mqttRules);
}

// Stored but not in effect: dimmed, not disabled. A disabled input is not
// submitted, so switching MQTT off would erase the settings it was switched off
// with — and the fields stay editable, so a broker can be filled in before it is
// switched on.
function dim() {
  var f = ID('mqttfields');
  if (ID('enabled').checked) f.classList.remove('dim');
  else                       f.classList.add('dim');

  // Hidden, not merely dimmed: an anonymous connection has no username and no
  // password, so there is nothing there to keep. The firmware reads the
  // checkbox's absence as the clear-it signal — a hidden field still submits,
  // but its value is no longer what the user means.
  ID('authfields').hidden = !ID('auth').checked;

  validateForm('mqttform', 'saveBtn', mqttRules);
}

function init() { initCommon(); getJSON('/mqttdata', build); }
</script>
</head><body onload="init()">
)HTML"
CONFIG_PORTAL_HEADER
R"HTML(
<div class="content">
  <form method="post" action="/mqtt">
    <div id="mqttform"></div>
    <div class="submit"><input type="submit" id="saveBtn" value="Save"></div>
  </form>
</div>
)HTML"
CONFIG_PORTAL_FOOTER
R"HTML(
</body></html>
)HTML";
