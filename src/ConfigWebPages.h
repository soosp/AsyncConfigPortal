#pragma once

#include <Arduino.h>

/**
 * @file ConfigWebPages.h
 * @brief PROGMEM HTML/CSS assets for the AsyncConfigPortal built-in pages.
 *
 * Design: modern but simple. No framework. Tables are replaced by flexbox
 * label/value rows inside card-like groups. The menu is a placeholder filled
 * dynamically from /menu (no flicker), with a <noscript> fallback. Pages fetch
 * their data via REST (/statusdata, /project, …) and populate the DOM.
 *
 * Show/hide uses the `hidden` attribute throughout — the stylesheet restores its
 * effect with [hidden]{display:none!important}, which .row's flex would
 * otherwise beat. JS template insertion uses innerHTML += with
 * <div class="row"> blocks (see the sensor-style pattern).
 */

// -----------------------------------------------------------------------------
// Shared CSS — light theme, CSS variables, system font, responsive.
//
// Replaceable at compile time by defining CONFIG_PORTAL_CSS before this header,
// which also drops the built-in sheet from flash. Both the macro and the sheet
// it names must reach the library's own translation unit, so it needs a forced
// include (PlatformIO: build_flags = -include mycss.h); the library cannot
// include a header it does not know about, and the Arduino IDE has no equivalent.
// The usual route is AsyncConfigPortal::setCss() / setCssExtra() at runtime,
// which works everywhere. See docs/CSS.md.
//
// The pages only ever reference class names, never the stylesheet itself, so a
// replacement that keeps the class contract keeps the pages working. See
// docs/CSS.md for that contract.
// -----------------------------------------------------------------------------
#if !defined(CONFIG_PORTAL_CSS)
static const char CONFIG_PORTAL_CSS_BUILTIN[] PROGMEM = R"CSS(
:root {
  --bg: #eef3f7;
  --fg: #1a2b3c;
  --muted: #5b6b7b;
  --accent: #2c6faa;
  --header-bg: #2c3e50;
  --header-fg: #ffffff;
  --card-bg: #ffffff;
  --border: #d0d7de;
  --ok: #2e7d32;
  --warn: #ef6c00;
  --alarm: #c62828;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  font-family: system-ui, -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  background: var(--bg);
  color: var(--fg);
  line-height: 1.4;
}
.header {
  background: var(--header-bg);
  color: var(--header-fg);
  padding: 0.8em 1em 0.6em;
  text-align: center;
}
.header h1 { margin: 0 0 0.3em; font-size: 1.4em; }
.menu { display: flex; flex-wrap: wrap; justify-content: center; gap: 0.2em 1em; }
.menu a { color: var(--header-fg); text-decoration: none; opacity: 0.9; }
.menu a:hover { opacity: 1; text-decoration: underline; }
.logout { display: block; text-align: center; color: var(--header-fg);
  opacity: 0.75; text-decoration: none; margin-top: 0.4em; font-size: 0.9em; }
.logout:hover { opacity: 1; text-decoration: underline; }
.content { max-width: 34em; margin: 1.2em auto; padding: 0 1em; }
.group {
  background: var(--card-bg);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 0.8em 1em;
  margin: 1em 0;
}
.group-title {
  font-weight: 600;
  color: var(--accent);
  text-align: center;
  margin-bottom: 0.6em;
}
.row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 1em;
  padding: 0.35em 0;
}
.label { flex: 0 0 45%; color: var(--muted); font-size: 0.92em; }
.value { flex: 1 1 55%; word-break: break-word; }
.value input, .value select {
  width: 100%;
  padding: 0.35em 0.5em;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: #fff;
  font: inherit;
}
.value input:invalid { border-color: var(--alarm); }
.value input[readonly], .value input:disabled { background: #eef1f4; color: var(--muted); }
/* Validation: required marker, invalid state, and per-field error message. */
.value input.invalid, .value select.invalid { border-color: var(--alarm); background: #fff5f5; }
.label.required::after { content: " *"; color: var(--alarm); font-weight: 700; }
.field-error { display: none; color: var(--alarm); font-size: 0.8em; margin-top: 0.2em; }
.hint { color: var(--muted); font-size: 0.8em; margin-top: 0.2em; }
/* The hidden attribute is the show/hide mechanism, but its user-agent rule is
   display:none at the weakest weight, which any display: rule here would beat —
   .row is flex, so a hidden row would still show. This restores it. */
[hidden] { display: none !important; }
/* Settings that are stored but not in effect. Dimmed rather than disabled: a
   disabled input is not submitted, so switching a feature off would silently
   erase the settings it was switched off with. */
.dim { opacity: 0.5; }
.submit { text-align: center; margin: 0.8em 0; }
.submit input {
  padding: 0.5em 1.4em;
  border: none;
  border-radius: 6px;
  background: var(--accent);
  color: #fff;
  font: inherit;
  cursor: pointer;
}
.submit input:disabled { background: #9db4c7; cursor: not-allowed; }
.footer { text-align: center; color: var(--muted); font-size: 0.82em; margin: 1.5em 0 2em; }
.ok { color: var(--ok); } .warn { color: var(--warn); } .alarm { color: var(--alarm); }
.alert-container { position: fixed; inset: 0; display: flex; align-items: center; justify-content: center; }
.alert-backdrop { position: fixed; inset: 0; background: #90a4b4; opacity: 0.7; }
.alert-content { position: relative; background: var(--card-bg); padding: 1.2em 1.6em; border-radius: 10px; border: 2px solid var(--header-bg); }

/* Connection watchdog. The banner states the problem; dimming the values makes
   staleness visible at a glance, which a banner alone does not - the last known
   readings stay legible but clearly are not live. */
.offline { display: none; background: #8a1c1c; color: #fff; text-align: center; padding: 0.6em 1em; font-size: 0.9em; }
body.stale .value, body.stale .card, body.stale #clock { opacity: 0.35; }
@media (max-width: 480px) {
  .row { flex-direction: column; align-items: stretch; gap: 0.15em; }
  .label { flex: none; }
}
)CSS";
#  define CONFIG_PORTAL_CSS CONFIG_PORTAL_CSS_BUILTIN
#endif


// -----------------------------------------------------------------------------
// Result pages for a form save.
//
// The contract: they return to the page the POST came from. A component does not
// name its own path here, and does not need a copy of these pages per page it
// serves — document.referrer is the form's page, which is the only page the user
// can have arrived from. Where that is unavailable (a strict referrer policy),
// the fallback is Status, which is a sensible place to be rather than a dead end.
//
// This is for saves. The Other page's own results — reboot, factory reset,
// firmware mismatch — deliberately go elsewhere: after a restart or a reset,
// "back where you came from" is the wrong answer.
// -----------------------------------------------------------------------------
static const char CONFIG_PORTAL_SAVED_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<link rel="stylesheet" href="/css"><title>Saved</title>
<script>setTimeout(function(){location.replace(document.referrer||'/')},2000)</script>
</head><body>
<div class="alert-container"><div class="alert-backdrop"></div>
<div class="alert-content">Configuration saved.</div></div></body></html>
)HTML";

static const char CONFIG_PORTAL_SAVE_FAILED_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<link rel="stylesheet" href="/css"><title>Not saved</title>
<script>setTimeout(function(){location.replace(document.referrer||'/')},4000)</script>
</head><body>
<div class="alert-container"><div class="alert-backdrop"></div>
<div class="alert-content">Configuration rejected. Check the values and try again.</div></div></body></html>
)HTML";

// -----------------------------------------------------------------------------
// Shared JS snippet reused by pages (menu, project, auth-state helpers).
// Kept as a macro so each page embeds it inside its own <script>.
// -----------------------------------------------------------------------------
// Common JavaScript, served once at /common.js and cached by the browser.
// Pages include it with <script src="/common.js"></script> instead of
// duplicating these helpers. initCommon() wires up the menu, project info, and
// the auth-dependent Logout link visibility.
// -----------------------------------------------------------------------------
static const char CONFIG_PORTAL_COMMON_JS[] PROGMEM = R"JS(
function ID(id){return document.getElementById(id);}
// --- Connection watchdog ----------------------------------------------------
// getJSON reports every outcome, not just success: a page whose device has gone
// away would otherwise keep showing its last values forever - visually
// indistinguishable from a working system. Outcomes are tracked centrally:
// the values are dimmed and a banner appears, so stale data cannot be mistaken
// for live data.
var netFails=0,netDown=false;
// Deliberately neutral: it is the one statement that is true on every page. The
// consequence differs — a status page is showing stale readings, a form page
// cannot save — so pages that want to spell it out override this before
// initCommon(). On status pages the dimming already conveys staleness.
var netOfflineMsg='Device not responding';
function netBanner(){
 var b=ID('offline');
 if(!b){b=document.createElement('div');b.id='offline';b.className='offline';
  document.body.insertBefore(b,document.body.firstChild);}
 b.innerHTML=netOfflineMsg;
 b.style.display=netDown?'block':'none';
 if(netDown)document.body.className+=' stale';
 else document.body.className=document.body.className.replace(/ *\bstale\b/g,'');
 netGateSubmits();
}
// Submitting into a dead device is worse than useless: the form is a plain POST,
// so the browser navigates away, shows its own error page and takes everything
// the user typed with it. Disabling submit keeps the input safely on the page
// until the device answers again.
function netGateSubmits(){
 var b=document.querySelectorAll('input[type=submit],button[type=submit]');
 for(var i=0;i<b.length;i++)b[i].disabled=netDown;
 // Restores the correct state for validated forms, which may want the button
 // disabled for their own reasons.
 if(typeof fvLast!=='undefined'&&fvLast)validateForm(fvLast.c,fvLast.s,fvLast.f);
}
// ok means "the device answered", NOT "the answer was 200". A 401 from a page
// the user is not logged in for, or a 500 from one failing endpoint, both prove
// the device is alive; treating them as connection loss would cry wolf.
function netMark(ok){
 if(ok){netFails=0;if(netDown){netDown=false;netBanner();}return;}
 // Tolerate one miss: a reboot or a single dropped request should not flash the
 // banner. Two consecutive failures means the device is really unreachable.
 if(++netFails>=2&&!netDown){netDown=true;netBanner();}
}
function netIsDown(){return netDown;}
function getJSON(url,cb){var x=new XMLHttpRequest();
x.onreadystatechange=function(){
 if(this.readyState!=4)return;
 // status 0 at readyState 4 means the request never reached anyone.
 netMark(this.status!==0);
 if(this.status==200){
  var o;try{o=JSON.parse(this.responseText);}catch(e){return;}
  cb(o);
 }
};
x.onerror=function(){netMark(false);};
x.ontimeout=function(){netMark(false);};
x.open('GET',url,true);
x.timeout=5000;   // set after open(): older browsers reject it before
x.send();}
function buildMenu(items){var m=ID('menu');if(!m)return;
m.innerHTML=items.map(function(i){return '<a href="'+i.path+'">'+i.label+'</a>';}).join('');
heartbeat();}
function setProject(o){var e;
if(e=ID('project_name'))e.innerHTML=o.project_name;
if(e=ID('project_ver'))e.innerHTML='v'+o.project_ver;
if(e=ID('project_desc'))e.innerHTML=o.project_desc;
if(e=ID('project_year'))e.innerHTML=o.project_year;
if(e=ID('author'))e.innerHTML=o.author;
document.title=o.project_name;}
// Liveness heartbeat.
//
// Pages that poll for data feed the watchdog on their own. Form pages (Network,
// Other) fetch once and then sit idle, so a device that dies while the user is
// filling the form would go unnoticed until submit — by which point a plain form
// POST has navigated away and taken the typed values with it. This slow probe
// closes that gap.
//
// /chkauth is the natural probe: tiny, present on every page, and it already had
// to be called to decide whether to show the Logout link, so one request serves
// both purposes. Its status code answers the auth question; merely having a
// status code answers the liveness question.
function heartbeat(){
 var x=new XMLHttpRequest();
 x.onreadystatechange=function(){
  if(this.readyState!=4)return;
  netMark(this.status!==0);
  var e=ID('logout');
  if(e)e.style.display=(this.status==200)?'block':'none';
 };
 x.onerror=function(){netMark(false);};
 x.ontimeout=function(){netMark(false);};
 x.open('GET','/chkauth',true);
 x.timeout=5000;
 x.send();
}
function initCommon(){getJSON('/menu',buildMenu);getJSON('/project',setProject);
 heartbeat();setInterval(heartbeat,10000);}
)JS";

// Field toolkit: the validation engine and the row builders. Served separately
// at /fields.js so a form page — or a standalone server such as WiFiPortal —
// can pull the field machinery without the runtime above, which is coupled to
// /menu, /project and /chkauth. ID() is repeated here so the toolkit is
// self-contained; a form page that loads both scripts merely redefines it,
// which is harmless.
static const char CONFIG_PORTAL_FIELDS_JS[] PROGMEM = R"JS(
function ID(id){return document.getElementById(id);}

// --- Form validation engine -------------------------------------------------
// IP address check: split into four octets, each 0-255 (numeric range, not a
// regex — clearer, no lookbehind/browser-compat pitfalls). Rejects e.g. 392.x.
function fvIsIp(s){
 var p=s.split('.');
 if(p.length!==4)return false;
 for(var i=0;i<4;i++){
  if(!/^\d{1,3}$/.test(p[i]))return false;
  var n=parseInt(p[i],10);
  if(n<0||n>255)return false;
  if(p[i].length>1&&p[i].charAt(0)==='0')return false; // no leading zeros
 }
 return true;
}
// Host check: accepts an IP, or an FQDN (labels of letters/digits/underscore/
// hyphen, not starting/ending with hyphen; TLD >= 2 letters; total <= 254).
// Uses per-label regex instead of lookbehind so it works on all browsers.
function fvIsHost(s){
 if(s.length>254)return false;
 if(fvIsIp(s))return true;
 var labels=s.replace(/\.$/,'').split('.');
 if(labels.length<1)return false;
 for(var i=0;i<labels.length;i++){
  if(!/^[a-zA-Z0-9]([a-zA-Z0-9_\-]{0,61}[a-zA-Z0-9])?$/.test(labels[i]))return false;
 }
 return /^[a-zA-Z]{2,}$/.test(labels[labels.length-1]);
}
// Element-type-aware value reader (text/number/select -> value, checkbox ->
// checked bool). Keeps extraValidate() simple: v.value(id) returns the right
// thing regardless of element type.
function fvRead(el){
 if(!el)return null;
 if(el.type==='checkbox')return el.checked;
 return el.value;
}
// Marks a field invalid: red outline + message below it (from title or msg).
function fvFail(el,msg){
 if(!el)return;
 el.classList.add('invalid');
 var e=ID(el.id+'_err');
 if(e){e.textContent=msg||'';e.style.display=msg?'block':'none';}
}
function fvClear(el){
 if(!el)return;
 el.classList.remove('invalid');
 var e=ID(el.id+'_err');
 if(e){e.textContent='';e.style.display='none';}
}
// Built-in per-field checks: required, pattern, min/max, and cross-field
// data-gt/lt/eq/neq/match. Returns true if the field passed.
function fvBuiltin(el){
 var t=el.type, val=fvRead(el);
 // required
 if(el.hasAttribute('required')){
  if(t==='checkbox'){ if(!val){fvFail(el,'Required');return false;} }
  else if(val===''||val===null){fvFail(el,'Required');return false;}
 }
 // skip further checks on empty optional fields
 if((val===''||val===null)&&!el.hasAttribute('required')){fvClear(el);return true;}
 // pattern (text-like only)
 if(el.pattern&&t!=='checkbox'){
  var re=new RegExp('^(?:'+el.pattern+')$');
  if(!re.test(val)){fvFail(el,'Expected: '+(el.title||'valid format'));return false;}
 }
 // IP address (data-ip): numeric octet range check.
 if(el.hasAttribute('data-ip')&&val!==''){
  if(!fvIsIp(val)){fvFail(el,'Invalid IP address');return false;}
 }
 // Host (data-host): IP or FQDN.
 if(el.hasAttribute('data-host')&&val!==''){
  if(!fvIsHost(val)){fvFail(el,'Invalid IP or hostname');return false;}
 }
 // numeric min/max
 if(t==='number'&&val!==''){
  var n=parseFloat(val);
  if(el.hasAttribute('min')&&n<parseFloat(el.min)){fvFail(el,'Min '+el.min);return false;}
  if(el.hasAttribute('max')&&n>parseFloat(el.max)){fvFail(el,'Max '+el.max);return false;}
 }
 // cross-field rules via data- attributes (value compared to another field)
 var rules=[['gt','>'],['lt','<'],['gte','>='],['lte','<='],['eq','=='],['neq','!='],['match','===']];
 for(var i=0;i<rules.length;i++){
  var a='data-'+rules[i][0], op=rules[i][1];
  if(el.hasAttribute(a)){
   var other=ID(el.getAttribute(a));
   if(other){
    var x=val, y=fvRead(other), num=(t==='number');
    if(num){x=parseFloat(x);y=parseFloat(y);}
    var ok=(op==='>')?x>y:(op==='<')?x<y:(op==='>=')?x>=y:(op==='<=')?x<=y:
           (op==='==')?x==y:(op==='!=')?x!=y:x===y;
    if(!ok){
     var m=el.getAttribute(a+'-msg')||('Must be '+op+' '+(other.title||el.getAttribute(a)));
     fvFail(el,m);return false;
    }
   }
  }
 }
 fvClear(el);
 return true;
}
// Validates every field in a container, runs the optional page-specific
// extraValidate(v), toggles the submit button, and returns overall validity.
// Pass the submit button id (or null) to enable/disable it.
function validateForm(containerId,submitId,extraFn){
 var c=ID(containerId); if(!c)return false;
 var els=c.querySelectorAll('input,select,textarea');
 var ok=true, failed={};
 for(var i=0;i<els.length;i++){
  if(els[i].type==='submit'||els[i].type==='button')continue;
  if(!fvBuiltin(els[i])){ok=false;failed[els[i].id]=true;}
 }
 // Page-specific complex rules (power-of-two, formulas, etc.). Uses the same
 // fail() styling so custom and built-in errors look identical.
 if(extraFn){
  var v={
   value:function(id){return fvRead(ID(id));},
   fail:function(id,msg){fvFail(ID(id),msg);ok=false;failed[id]=true;},
   get:function(id){return ID(id);}
  };
  extraFn(v);
 }
 var s=submitId?ID(submitId):null;
 // Offline overrides validity: a perfectly valid form still must not be sent
 // to a device that is not there.
 if(s)s.disabled=!ok||(typeof netDown!=='undefined'&&netDown);
 return ok;
}
// Wires onblur validation on every field in a container. Call after building
// the form. extraFn is the optional page-specific validator.
var fvLast=null;   // last form wired up, so netGateSubmits() can re-run it
function attachValidation(containerId,submitId,extraFn){
 var c=ID(containerId); if(!c)return;
 fvLast={c:containerId,s:submitId,f:extraFn};
 var run=function(){validateForm(containerId,submitId,extraFn);};
 var els=c.querySelectorAll('input,select,textarea');
 for(var i=0;i<els.length;i++){
  if(els[i].type==='submit'||els[i].type==='button')continue;
  els[i].addEventListener('blur',run);
  if(els[i].type==='checkbox'||els[i].tagName==='SELECT')
   els[i].addEventListener('change',run);
 }
 run(); // initial state (disables submit if incomplete)
}
// Marks the labels of required fields (adds .required so CSS shows a "*").
// Assumes each .row has a .label and an input; call after building the form.
function markRequired(containerId){
 var c=ID(containerId); if(!c)return;
 var rows=c.querySelectorAll('.row');
 for(var i=0;i<rows.length;i++){
  var inp=rows[i].querySelector('input,select,textarea');
  var lab=rows[i].querySelector('.label');
  if(inp&&lab&&inp.hasAttribute('required'))lab.classList.add('required');
 }
}

// --- Field/row builders (shared) --------------------------------------------
// Build a labelled form row plus its error span, so any page composed with
// addPage() can render fields the validation engine above already understands.
// Kept here rather than in a component because field rendering is a base
// concern: the net module and an app page get the same builders.

// Row with a text input; opt: {type, ip, host, title, req} sets validation
// attrs and an error span. data-ip / data-host trigger the engine's checks.
function ifRow(idx,label,name,val,ph,opt){
 opt=opt||{};
 var id=name+idx;
 var attr='id="'+id+'" name="'+id+'" value="'+(val||'')+'" placeholder="'+(ph||'')+'"';
 if(opt.ip)attr+=' data-ip';
 if(opt.host)attr+=' data-host';
 if(opt.title)attr+=' title="'+opt.title+'"';
 if(opt.req)attr+=' required';
 return '<div class="row"><span class="label">'+label+':</span><span class="value">'+
  '<input type="'+(opt.type||'text')+'" '+attr+'></span></div>'+
  '<span class="field-error" id="'+id+'_err"></span>';
}
// Numeric row. The engine checks min/max on type=number; the step is enforced
// by a page rule, since the engine does not.
function numRow(idx,label,name,val,min,max,step,title){
 var id=name+idx;
 var a='id="'+id+'" name="'+id+'" type="number"';
 a+=' value="'+((val===undefined||val===null)?'':val)+'"';
 a+=' min="'+min+'" max="'+max+'" step="'+step+'"';
 if(title)a+=' title="'+title+'"';
 return '<div class="row"><span class="label">'+label+':</span><span class="value">'+
  '<input '+a+'></span></div>'+
  '<span class="field-error" id="'+id+'_err"></span>';
}
// Row with a <select>. options: an array of plain values, or of {v,t}
// (value/text) when the label differs from the submitted value. opt:
// {title, req, ph}. ph adds a leading <option value=""> placeholder so a
// required select can reject "nothing chosen" — the engine treats an empty
// value as unset, exactly as for a text field. The option whose value equals
// val (compared as strings) is preselected.
function selectRow(idx,label,name,val,options,opt){
 opt=opt||{};
 var id=name+idx;
 var a='id="'+id+'" name="'+id+'"';
 if(opt.title)a+=' title="'+opt.title+'"';
 if(opt.req)a+=' required';
 var o=opt.ph?'<option value="">'+opt.ph+'</option>':'';
 options=options||[];
 for(var i=0;i<options.length;i++){
  var it=options[i];
  var v=(it&&it.v!==undefined)?it.v:it;
  var t=(it&&it.t!==undefined)?it.t:v;
  o+='<option value="'+v+'"'+((''+v)===(''+val)?' selected':'')+'>'+t+'</option>';
 }
 return '<div class="row"><span class="label">'+label+':</span><span class="value">'+
  '<select '+a+'>'+o+'</select></span></div>'+
  '<span class="field-error" id="'+id+'_err"></span>';
}

// --- Static-addressing coherence (mirrors NetworkProfile::checkStatic) -------
// Cross-field checks the per-field engine cannot do: an address that is
// syntactically valid but wrong for its subnet. Shared here so any static
// config form (the network page, a Wi-Fi portal) reuses the same rules. The
// server stays authoritative; this is only for instant feedback.
function fvIp2n(s){
 s=(s||'').replace(/^\s+|\s+$/g,'');
 var p=s.split('.'); if(p.length!==4)return -1;
 var n=0;
 for(var i=0;i<4;i++){ if(!/^\d{1,3}$/.test(p[i]))return -1; var o=+p[i]; if(o>255)return -1; n=n*256+o; }
 return n>>>0;
}
function fvUnicast(n){
 if(((n&0xFF000000)>>>0)===0)return false;          // 0/8
 if(((n&0xFF000000)>>>0)===0x7F000000)return false; // 127/8 loopback
 if(((n&0xF0000000)>>>0)===0xE0000000)return false; // 224/4 multicast
 if(((n&0xF0000000)>>>0)===0xF0000000)return false; // 240/4 reserved
 return true;
}
// An optional address left blank or 0.0.0.0 counts as unset.
function fvUnset(s){ s=(s||'').replace(/^\s+|\s+$/g,''); return s===''||s==='0.0.0.0'; }
// null if the ip/mask/gw/dns block is coherent, else {key,msg} naming the field
// ('ip','mask','gw','dns'+n). dns is an array of strings.
function fvCheckStatic(ip,mask,gw,dns){
 var m=fvIp2n(mask);
 if(m<=0)return {key:'mask',msg:'Not a valid subnet mask'};
 var host=(~m)>>>0;
 if(((host&(host+1))>>>0)!==0)return {key:'mask',msg:'Not a valid subnet mask'};
 var a=fvIp2n(ip);
 if(a<0)return {key:'ip',msg:'Not a valid IP address'};
 if(!fvUnicast(a))return {key:'ip',msg:'Not a usable host address'};
 if(host!==0&&host!==1){ var h=(a&host)>>>0; if(h===0||h===host)return {key:'ip',msg:'This is the network or broadcast address'}; }
 if(!fvUnset(gw)){ var g=fvIp2n(gw); if(g<0)return {key:'gw',msg:'Not a valid IP address'}; if(((g&m)>>>0)!==((a&m)>>>0))return {key:'gw',msg:'Gateway is not on this subnet'}; }
 if(dns){ for(var i2=0;i2<dns.length;i2++){ if(fvUnset(dns[i2]))continue; var d=fvIp2n(dns[i2]); if(d<0||!fvUnicast(d))return {key:'dns'+i2,msg:'Not a usable DNS address'}; } }
 return null;
}
)JS";

// -----------------------------------------------------------------------------
// Common page header (menu placeholder + noscript fallback) and footer.
// The Logout link is injected into the menu by buildMenu() and shown only when
// /chkauth reports an authenticated session.
// -----------------------------------------------------------------------------
#define CONFIG_PORTAL_HEADER \
"<div class=\"header\"><h1><span id=\"project_name\">&nbsp;</span></h1>" \
"<div class=\"menu\" id=\"menu\"><noscript><a href=\"/\">Status</a></noscript></div>" \
"<a href=\"/logout\" id=\"logout\" class=\"logout\" style=\"display:none\">Logout</a></div>"

#define CONFIG_PORTAL_FOOTER \
"<div class=\"footer\"><span id=\"project_desc\">Project</span> " \
"<span id=\"project_ver\"></span><br>&copy; <span id=\"project_year\"></span>, " \
"<span id=\"author\"></span></div>"

// -----------------------------------------------------------------------------
// Status page — generic system info (overridable by subclasses).
// -----------------------------------------------------------------------------
static const char CONFIG_PORTAL_STATUS_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="stylesheet" href="/css">
<script src="/common.js"></script>
</head><body onload="init()">
<script>
function fmtUptime(s){var d=Math.floor(s/86400);s%=86400;var h=Math.floor(s/3600);
s%=3600;var m=Math.floor(s/60);return d+'d '+h+'h '+m+'m';}
// Fills a row, or hides it when the value is absent: not every chip reports
// every metric (no die-temperature sensor or minimum-heap tracking on ESP8266),
// and a missing reading must not be shown as a zero.
function setRow(id,v){var e=ID(id);if(v===null){var r=e.closest('.row');if(r)r.style.display='none';return;}
  e.innerHTML=v;}
function setData(o){
  ID('uptime').innerHTML=fmtUptime(o.uptime_s);
  ID('heap_free').innerHTML=(o.heap_free/1024).toFixed(1)+' KB';
  setRow('heap_min',o.heap_min===undefined?null:(o.heap_min/1024).toFixed(1)+' KB');
  ID('heap_max').innerHTML=(o.heap_max_alloc/1024).toFixed(1)+' KB';
  ID('heap_frag').innerHTML=o.heap_frag_pct+'%';
  ID('chip').innerHTML=o.chip+' ('+o.cores+' core)';
  ID('cpu_mhz').innerHTML=o.cpu_mhz+' MHz';
  setRow('temp',o.temp===undefined?null:o.temp+' °C');
  ID('flash').innerHTML=(o.flash_size/1048576).toFixed(0)+' MB';
  ID('reset').innerHTML=o.reset_reason;
}
function init(){initCommon();getJSON('/statusdata',setData);
setInterval(function(){getJSON('/statusdata',setData);},5000);}
</script>
)HTML"
CONFIG_PORTAL_HEADER
R"HTML(
<div class="content">
  <div class="group">
    <div class="group-title">System</div>
    <div class="row"><span class="label">Uptime:</span><span class="value" id="uptime">-</span></div>
    <div class="row"><span class="label">Chip:</span><span class="value" id="chip">-</span></div>
    <div class="row"><span class="label">Clock:</span><span class="value" id="cpu_mhz">-</span></div>
    <div class="row"><span class="label">Temperature:</span><span class="value" id="temp">-</span></div>
    <div class="row"><span class="label">Flash:</span><span class="value" id="flash">-</span></div>
    <div class="row"><span class="label">Free heap:</span><span class="value" id="heap_free">-</span></div>
    <div class="row"><span class="label">Min heap:</span><span class="value" id="heap_min">-</span></div>
    <div class="row"><span class="label" title="Largest block that can be allocated in one piece. This is the number that decides whether a page can be built.">Max alloc:</span><span class="value" id="heap_max">-</span></div>
    <div class="row"><span class="label" title="100 x (1 - max alloc / free heap). Compare it with itself under similar load: releasing memory raises free heap without enlarging the largest block, so the figure goes up while the heap gets healthier.">Fragmentation:</span><span class="value" id="heap_frag">-</span></div>
    <div class="row"><span class="label">Last reset:</span><span class="value" id="reset">-</span></div>
  </div>
</div>
)HTML"
CONFIG_PORTAL_FOOTER
R"HTML(
</body></html>
)HTML";

// -----------------------------------------------------------------------------
// Other page — password change, firmware update, backup, restart.
// -----------------------------------------------------------------------------
static const char CONFIG_PORTAL_OTHER_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="stylesheet" href="/css">
<script src="/common.js"></script>
</head><body onload="init()">
<script>
function setData(o){ID('user').value=o.user;checkAuth();}
function checkAuth(){var s=ID('authSubmit');
var ok=ID('user').value!=''&&ID('pw').value.length>=5&&ID('pw').value==ID('pwchk').value;
s.disabled=!ok;}
function onUpdate(){ID('update-alert').style.display='flex';}
function init(){initCommon();getJSON('/authdata',setData);
ID('updateForm').addEventListener('submit',onUpdate);}
</script>
)HTML"
CONFIG_PORTAL_HEADER
R"HTML(
<div class="content">
  <form method="post" action="/misc" id="authForm">
    <div class="group">
      <div class="group-title">Authentication</div>
      <div class="row"><span class="label">Username:</span><span class="value">
        <input type="text" id="user" name="user" pattern="[0-9A-Za-z]{1,20}" oninput="checkAuth()" required></span></div>
      <div class="row"><span class="label">Password:</span><span class="value">
        <input type="password" id="pw" name="pw" pattern=".{5,20}" oninput="checkAuth()" required></span></div>
      <div class="row"><span class="label">Confirm:</span><span class="value">
        <input type="password" id="pwchk" pattern=".{5,20}" oninput="checkAuth()" required></span></div>
    </div>
    <div class="submit"><input type="submit" id="authSubmit" value="Set password" disabled></div>
  </form>
  <form method="post" enctype="multipart/form-data" action="/update" id="updateForm">
    <div class="group">
      <div class="group-title">Firmware update</div>
      <div class="row"><span class="label">File:</span><span class="value">
        <input type="file" id="updateFile" name="updateFile" required></span></div>
    </div>
    <div class="submit"><input type="submit" value="Update"></div>
  </form>
  <form method="post" action="/factoryreset" id="resetForm">
    <div class="group">
      <div class="group-title">Factory reset</div>
      <div class="row"><span class="label">Warning:</span><span class="value">
        Erases the configuration and the login credentials. On a device reached
        over Wi-Fi this makes it unreachable until it is set up again.</span></div>
      <div class="row"><span class="label">Confirm:</span><span class="value">
        <input type="checkbox" id="fc" onchange="ID('resetSubmit').disabled=!this.checked" required></span></div>
    </div>
    <div class="submit"><input type="submit" id="resetSubmit" value="Factory reset" disabled></div>
  </form>
  <form method="post" action="/restart" id="restartForm">
    <div class="group">
      <div class="group-title">Restart</div>
      <div class="row"><span class="label">Confirm:</span><span class="value">
        <input type="checkbox" id="rc" onchange="ID('restartSubmit').disabled=!this.checked" required></span></div>
    </div>
    <div class="submit"><input type="submit" id="restartSubmit" value="Restart" disabled></div>
  </form>
</div>
<div class="alert-container" id="update-alert" style="display:none">
  <div class="alert-backdrop"></div><div class="alert-content">Update in progress. Please wait...</div>
</div>
)HTML"
CONFIG_PORTAL_FOOTER
R"HTML(
</body></html>
)HTML";

// Backup page — one section per registered component.
//
// Deliberately shows the sections even when there is only one: the point of the
// page is that a user can see everything that offers a backup, so that saving
// three of four is a visible omission rather than a silent one.
static const char CONFIG_PORTAL_BACKUP_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="stylesheet" href="/css">
<script src="/common.js"></script>
</head><body onload="init()">
<script>
function esc(s){return String(s).replace(/[&<>"]/g,function(c){
 return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];});}
// Inputs are built here from descriptors, never injected as markup by the
// component, so a section cannot reshape a built-in page.
function field(f){
 var id=esc(f.name),lbl=esc(f.label||f.name),h='';
 if(f.type=='checkbox'){
  h='<input type="checkbox" id="'+id+'" name="'+id+'" value="1"'+
    (f.checked?' checked':'')+'>';
 }else if(f.type=='password'){
  h='<input type="password" id="'+id+'" name="'+id+'">';
 }else return '';
 return '<div class="row"><span class="label">'+lbl+':</span>'+
        '<span class="value">'+h+'</span></div>';
}
// One form per action, matching the Other page: a group only where there are
// fields to put in it, and the submit button outside it.
function section(s){
 var t=esc(s.title);
 var h='<form method="post" action="'+esc(s.backup)+'">'+
   '<div class="submit"><input type="submit" value="Download '+t+'"></div></form>';
 h+='<form method="post" action="'+esc(s.restore)+'" enctype="multipart/form-data">'+
   '<div class="group"><div class="group-title">Restore '+t+'</div>'+
   '<div class="row"><span class="label">File:</span><span class="value">'+
   '<input type="file" name="file" required></span></div>';
 var fs=s.fields||[];
 for(var i=0;i<fs.length;i++) h+=field(fs[i]);
 h+='</div><div class="submit"><input type="submit" value="Restore"></div></form>';
 return h;
}
function setData(list){
 var h='';
 for(var k=0;k<list.length;k++) h+=section(list[k]);
 ID('sections').innerHTML=h;
}
function init(){initCommon();getJSON('/backupdata',setData);}
</script>
)HTML"
CONFIG_PORTAL_HEADER
R"HTML(
<div class="content">
  <p style="text-align:center">Secrets are not included in a backup. Where a
  component needs one in order to restore, its section asks for it below.</p>
  <div id="sections"></div>
</div>
)HTML"
CONFIG_PORTAL_FOOTER
R"HTML(
</body></html>
)HTML";

// -----------------------------------------------------------------------------
// Small utility pages.
// -----------------------------------------------------------------------------
// Shown once, immediately after a factory reset — the only moment at which the
// user can still be told what happened, since the next thing the device does is
// reboot into a state where the old credentials no longer work.
static const char CONFIG_PORTAL_FACTORYRESET_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="stylesheet" href="/css">
</head><body>
<div class="content">
  <div class="group">
    <div class="group-title">Factory reset</div>
    <div class="row"><span class="value">The stored configuration and the login
    credentials have been erased, and the device is restarting with its built-in
    defaults.</span></div>
    <div class="row"><span class="value">Sign in again with the default
    credentials from the project&#39;s documentation. If the device used a static
    address it will now ask for one over DHCP, so its address has probably
    changed.</span></div>
  </div>
</div>
</body></html>
)HTML";

// Waits for the device instead of guessing how long it takes. A fixed refresh
// has to be set for the slowest case and is then wrong for every other one — 15
// seconds of staring at a device that came back in five. Probing /chkauth is the
// same trick the liveness watchdog uses: it is tiny, present on every build, and
// merely having a status code answers the question. Anything but 0 means the
// server is answering, 401 included.
//
// The first probe is delayed past the deferred restart, or it would find the
// device still up and send the browser to a page about to disappear.
static const char CONFIG_PORTAL_REBOOT_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<link rel="stylesheet" href="/css"><title>Rebooting</title>
<script>
var t0 = Date.now(), GIVE_UP = 60000;
function again() {
  if (Date.now() - t0 > GIVE_UP) {
    document.getElementById('m').innerHTML =
      'Still not answering. It may have come back on a different address \u2014 ' +
      '<a href="/">try again</a>.';
    return;
  }
  setTimeout(probe, 1000);
}
function probe() {
  var x = new XMLHttpRequest();
  x.onreadystatechange = function () {
    if (this.readyState != 4) return;
    if (this.status !== 0) { location.replace('/'); return; }
    again();
  };
  x.onerror = again; x.ontimeout = again;
  x.open('GET', '/chkauth', true); x.timeout = 2000; x.send();
}
setTimeout(probe, 2000);
</script>
</head><body>
<div class="alert-container"><div class="alert-backdrop"></div>
<div class="alert-content" id="m">Rebooting, please wait...</div></div></body></html>
)HTML";

static const char CONFIG_PORTAL_PWCHANGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><link rel="stylesheet" href="/css">
<title>Saved</title></head><body>
<script>var x=new XMLHttpRequest();x.open('GET','/authdata',true,'x','x');x.send();
setTimeout(function(){location.href='/';},600);</script>
<div class="alert-container"><div class="alert-backdrop"></div>
<div class="alert-content">Saving password...</div></div></body></html>
)HTML";

static const char CONFIG_PORTAL_LOGOUT_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><link rel="stylesheet" href="/css">
<title>Logout</title></head><body>
<script>var x=new XMLHttpRequest();x.open('GET','/authdata',true,'x','x');x.send();
setTimeout(function(){location.href='/';},500);</script>
<div class="alert-container"><div class="alert-backdrop"></div>
<div class="alert-content">Logging out...</div></div></body></html>
)HTML";

static const char CONFIG_PORTAL_FILEERROR_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<meta http-equiv="refresh" content="3; url='/misc'"><link rel="stylesheet" href="/css">
<title>Error</title></head><body>
<div class="alert-container"><div class="alert-backdrop"></div>
<div class="alert-content">Malformed or incomplete file.</div></div></body></html>
)HTML";

// Shown when an uploaded image's identity marker doesn't match this device
// (wrong project or wrong board), or no marker was found. The working firmware
// is left untouched.
static const char CONFIG_PORTAL_FWMISMATCH_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<meta http-equiv="refresh" content="4; url='/misc'"><link rel="stylesheet" href="/css">
<title>Wrong firmware</title></head><body>
<div class="alert-container"><div class="alert-backdrop"></div>
<div class="alert-content">This firmware is not for this device.<br>
Update rejected; nothing was changed.</div></div></body></html>
)HTML";

static const char CONFIG_PORTAL_NOTFOUND_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<meta http-equiv="refresh" content="3; url='/'"><link rel="stylesheet" href="/css">
<title>Not found</title></head><body>
<p style="text-align:center">Invalid URL. <a href="/">Return to start</a>.</p>
</body></html>
)HTML";
