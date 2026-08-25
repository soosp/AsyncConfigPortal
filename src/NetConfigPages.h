#pragma once

#include <Arduino.h>
#include "ConfigWebPages.h"   // base pages: CONFIG_PORTAL_HEADER / _FOOTER, /css, common.js

/**
 * @file NetConfigPages.h
 * @brief PROGMEM HTML for the optional NetConfigComponent (Network + Saved pages).
 *
 * Split out of ConfigWebPages.h so the base library carries no network-specific
 * markup. Included by NetConfigComponent.h; pulls in ConfigWebPages.h for the
 * shared header/footer macros the Network page composes with.
 */

// -----------------------------------------------------------------------------
// Network configuration page (used by NetConfigComponent). Builds one card per
// registered interface from /netdata; hides Wi-Fi-only fields for ETH profiles.
// -----------------------------------------------------------------------------
static const char CONFIG_PORTAL_NET_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="stylesheet" href="/css"><script src="/common.js"></script><script src="/fields.js"></script>
</head><body onload="init()">
<script>
var TYPE_WIFI=1, TYPE_ETH=2;
// ifRow() / numRow() / selectRow() now live in CONFIG_PORTAL_COMMON_JS (shared
// field/row builders, loaded on every page via the header). This page just
// calls them.
// Count how many dnsN keys the profile JSON carries (driven by the firmware's
// DNS_SERVER_COUNT), so the page shows exactly that many DNS fields.
function dnsCount(cfg){var n=0;while(('dns'+n) in cfg)n++;return n;}
// Same idea for NTP servers: the firmware's NtpProfile::SERVER_COUNT decides how many
// keys appear, and zero means NTP configuration is compiled out entirely.
// Retained so the priority rule below can iterate the rendered interfaces.
var netList=[];
// Normalises a priority for comparison: blank submits as 0 (the firmware runs
// atoi), and "01" is the same priority as "1".
function prioKey(s){
 s=(s||'').replace(/^\s+|\s+$/g,'');
 if(s==='')s='0';
 var n=Number(s);
 return isNaN(n)?s:String(n);
}
// Page rule: two interfaces must not share a priority, or the failover order is
// ambiguous. Runs through the engine's extraFn hook so the error styling and the
// submit gating are identical to the built-in checks.
function checkPrio(v){
 var seen={};
 for(var k=0;k<netList.length;k++){
  var i=netList[k].idx;
  if(!v.get('prio'+i))continue;   // field omitted (single interface)
  var key=prioKey(v.value('prio'+i));
  if(seen['#'+key])v.fail('prio'+i,'Priority '+key+' is already used by another interface');
  else seen['#'+key]=true;
 }
}
// Cross-field static-addressing coherence, per interface, skipped under DHCP.
// The arithmetic lives in fvCheckStatic() (shared); this maps its result back to
// the right field id (ip3, gw3, dns1_3, ...) for this page's naming.
function checkCoherence(v){
 for(var k=0;k<netList.length;k++){
  var i=netList[k].idx;
  var dh=v.get('dhcp'+i);
  if(dh&&dh.checked)continue;      // DHCP: static fields unused
  if(!v.get('ip'+i))continue;      // interface has no static fields
  var dns=[], dc=netList[k].dnscnt||0;
  for(var d=0;d<dc;d++)dns.push(v.value('dns'+d+'_'+i));
  var r=fvCheckStatic(v.value('ip'+i),v.value('mask'+i),v.value('gw'+i),dns);
  if(r){ var fid=(r.key.indexOf('dns')===0)?(r.key+'_'+i):(r.key+i); v.fail(fid,r.msg); }
 }
}
function netRules(v){checkPrio(v);checkCoherence(v);}
function buildNet(list){
 netList=list;
 var h='';
 // Device-level hostname: one global field (written to every profile on save),
 // read from the first profile. Not per-interface.
 var host0=(list.length&&list[0].cfg.host)?list[0].cfg.host:'';
 h+='<div class="group"><div class="group-title">Device</div>'+
    ifRow('','Hostname','host',host0,'device',{title:'Device hostname'})+
    '</div>';
 for(var k=0;k<list.length;k++){
  var e=list[k], c=e.cfg, i=e.idx;
  var title=(e.type==TYPE_WIFI)?'Wi-Fi':'Ethernet';
  h+='<div class="group"><div class="group-title">'+title+'</div>';
  if(e.type==TYPE_WIFI){
   // The SSID comes from "cfg", where the firmware escapes it. The entry no
   // longer carries a second, unescaped copy.
   h+=ifRow(i,'SSID','ssid',c.ssid,'',{title:'Network name',req:true});
   // Two controls, two facts: the checkbox says whether the network needs a
   // password at all, the field says what it is. One always-submitted field
   // could not express "clear it" and "leave it alone" at the same time.
   h+='<div class="row"><span class="label">Requires password:</span><span class="value">'+
      '<input type="checkbox" id="pwreq'+i+'" name="pwreq'+i+'" value="1"'+
      (e.secured?' checked':'')+' onchange="togglePw('+i+')"'+
      ' title="Untick for an open network with no authentication"></span></div>';
   h+=ifRow(i,'Password','pass','','',{type:'password',
      title:'Leave empty to keep the stored password'});
   // TX power is a fixed set of levels on ESP32; offer the actual values from
   // the firmware (txlevels) rather than a free number that could miss a level.
   // ESP32 sends a discrete level set (txlevels) -> dropdown; ESP8266 sends a
   // continuous range (txrange) -> bounded number input.
   if(e.txlevels)
     h+=selectRow(i,'TX power (dBm)','txpwr',c.txpwr,e.txlevels,
                {title:'Transmit power. Lower it to cut consumption, or to work '+
                 'around boards where antenna routing disturbs the oscillator.'});
   else if(e.txrange)
     h+=numRow(i,'TX power (dBm)','txpwr',c.txpwr,e.txrange.min,e.txrange.max,e.txrange.step,
                'Transmit power. Lower it to cut consumption, or to work '+
                'around boards where antenna routing disturbs the oscillator.');
  }
  h+='<div class="row"><span class="label">DHCP:</span><span class="value">'+
     '<input type="checkbox" id="dhcp'+i+'" name="dhcp'+i+'" value="1"'+(c.dhcp?' checked':'')+
     ' onchange="toggleStatic('+i+')"></span></div>';
  h+='<div id="static'+i+'">';
  h+=ifRow(i,'IP','ip',c.ip,'192.168.1.50',{ip:true,title:'IPv4 address',req:true});
  h+=ifRow(i,'Mask','mask',c.mask,'255.255.255.0',{ip:true,title:'Subnet mask',req:true});
  h+=ifRow(i,'Gateway','gw',c.gw,'192.168.1.1',{ip:true,title:'Gateway address (leave empty for an isolated segment)'});
  // DNS fields: as many slots as the firmware has, none required.
  //
  // Uses the count reported with the entry rather than the keys present in the
  // profile JSON: with DHCP enabled the profile omits its dnsN keys entirely, so
  // counting them produced no rows at all, and un-ticking DHCP had nothing to
  // reveal — the fields only turned up after a reboot.
  var dc=(e.dnscnt!==undefined)?e.dnscnt:dnsCount(c);
  for(var d=0;d<dc;d++){
   var lbl=(dc>1)?('DNS '+(d+1)):'DNS';
   h+=ifRow(i,lbl,'dns'+d+'_',c['dns'+d],'8.8.8.8',{ip:true,title:'DNS server address'});
  }
  h+='</div>';
  // Priority only means something when there is another interface to order
  // against. With a single interface the field is omitted entirely; it is then
  // absent from the POST and the firmware leaves the stored value untouched.
  if(list.length>1)
   h+=ifRow(i,'Priority','prio',c.prio,'0',{title:'Interface priority (0=highest)'});
  h+='</div>';
 }
 ID('netforms').innerHTML=h;
 for(var k=0;k<list.length;k++){ toggleStatic(list[k].idx); togglePw(list[k].idx); }
 markRequired('netforms');
 attachValidation('netforms','saveBtn',netRules);
}
// Unticking "requires password" means an open network: the field is cleared and
// disabled. The firmware reads the *checkbox's* absence as the clear-it signal,
// because a disabled input submits nothing at all — so the field alone could
// never say "clear it".
//
// Called once per interface after the form is built, exactly like toggleStatic:
// the initial state is produced by the same function that handles later changes,
// never rendered by hand. Two places describing one state eventually disagree.
function togglePw(i){
 var cb=ID('pwreq'+i),p=ID('pass'+i);
 if(!cb||!p)return;
 p.disabled=!cb.checked;
 if(!cb.checked)p.value='';
 p.placeholder=cb.checked?'(unchanged)':'';
 validateForm('netforms','saveBtn',netRules);
}
// DHCP on -> static fields hidden AND not required (so they don't block submit).
function toggleStatic(i){
 var d=ID('dhcp'+i),s=ID('static'+i);
 if(!d||!s)return;
 s.hidden=d.checked;
 var req=['ip','mask'];
 for(var k=0;k<req.length;k++){var el=ID(req[k]+i);
  if(el){if(d.checked)el.removeAttribute('required');else el.setAttribute('required','');}}
 validateForm('netforms','saveBtn',netRules);
}
// One Time group for the device, not one per interface: the servers are a
// device-level setting, and repeating them per profile only invited the two
// copies to drift.
function buildTime(ntp){
  var el=ID('nettime'); if(!ntp){el.innerHTML='';return;}
  var n=0; while(('ntp'+n) in ntp) n++;
  var h='<div class="group"><div class="group-title">Time<\/div>';
  for(var t=0;t<n;t++){
    // Placeholders follow the public pool's own naming (0.pool.ntp.org,
    // 1.pool.ntp.org, ...), which is what a user is most likely to enter.
    var ph=(n>1)?(t+'.pool.ntp.org'):'pool.ntp.org';
    h+=ifRow('', (n>1)?('NTP server '+(t+1)):'NTP server',
             'ntp'+t, ntp['ntp'+t], ph, {host:true});
  }
  ID('nettime').innerHTML=h+'<\/div>';
}
function setData(doc){buildNet(doc.profiles);buildTime(doc.ntp);}
// This page is a form, so stale readings are not the issue — being unable to
// save is. Set before initCommon() creates the banner.
netOfflineMsg='Device not responding \u2014 changes cannot be saved';
function init(){initCommon();getJSON('/netdata',setData);}
</script>
)HTML"
CONFIG_PORTAL_HEADER
R"HTML(
<div class="content">
  <form method="post" action="/net">
    <div id="netforms"></div>
    <div id="nettime"></div>
    <div class="submit"><input type="submit" id="saveBtn" value="Save"></div>
  </form>
</div>
)HTML"
CONFIG_PORTAL_FOOTER
R"HTML(
</body></html>
)HTML";

// Shown when a submitted configuration was rejected. Deliberately does not
