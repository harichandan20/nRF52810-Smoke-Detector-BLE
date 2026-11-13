/*
 * ESP32 gateway: BLE (BEDROOM1 / BEDROOM2) -> decode -> Wi-Fi -> HTTP POST (JSON) + web dashboard
 *
 * Server: 40.81.229.183 port 9000 (uvicorn / HTTP). One HTTP POST is sent per event (JSON body):
 *   {"unique_id":"RSSD..","gsm_status":"..","wifi_status":"..","slave_id":"..",
 *    "message":"..","timestamp":"dd/mm/yyyy hh:mm:ss"}
 *
 * The ESP32 runs TWO Wi-Fi sides at the same time (AP+STA):
 *   1) STATION      : joins the router "Ripple_Airtel_Deco". While it has an IP address the packets
 *                     say "WIFI:CONNECTED", the time comes from NTP, and every event is POSTed.
 *   2) ACCESS POINT : its own Wi-Fi "RSSD-Gateway" / "12345678". Join it with a phone and the
 *                     dashboard opens by itself (captive portal), or open http://192.168.4.1
 *                     (devices on the router can use the router IP printed in the serial log).
 *
 * Each nRF builds its own text packet and notifies it over BLE:
 *   GSM:NOT CONNECTED|WIFI:NOT CONNECTED|SLAVE:BEDROOM1|MSG:SMOKE DETECTED|TIME:hh:mm:ss|DATE:dd/mm/yyyy
 *
 * The gateway also:
 *   - gives each nRF the current time (NTP, or the phone's clock if the router has no internet)
 *   - tells each nRF whether the router Wi-Fi is connected
 *   - tells each nRF when the OTHER board has an alarm (cross alert)
 */
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* ====== SETTINGS ====== */
#define WIFI_SSID       "Ripple_Airtel_Deco"   /* router the gateway joins */
#define WIFI_PASS       "Ripple@123"
#define AP_SSID         "RSSD-Gateway"         /* the gateway's own Wi-Fi for the dashboard */
#define AP_PASS         "12345678"             /* at least 8 characters ("" = open network) */
#define AP_MAX_CONN     4
#define SERVER_HOST     "40.81.229.183"
#define SERVER_PORT     9000
#define SERVER_PATH     "/"             /* no endpoint path: POST goes to the root */
#define TCP_CONNECT_MS  5000            /* max time to open the connection */
#define TCP_SEND_MS     5000            /* max time to send */
#define TCP_REPLY_MS    3000            /* wait for the HTTP reply */
#define SEND_RETRY_MS   5000            /* wait this long after a failed send */
#define TIMEZONE        "IST-5:30"      /* India */
#define TZ_OFFSET_SEC   19800           /* must match TIMEZONE: +5:30 = 19800 s */
#define HEARTBEAT_MS    30000           /* send status even if nothing changed (0 = off) */
#define TIME_RESYNC_MS  600000          /* re-send the time to the nRF every 10 min */
/* ====================== */

#define NUM_BOARDS     2
#define PKT_MAX        128              /* longest nRF packet is 109 characters */
#define OFFLINE_MS     5000
#define ALERT_MASK     0x03             /* smoke OR switch alerts the other board */
#define STATE_MASK     0x0B             /* smoke, switch, remote = a new message */
#define EV_MAX         10               /* events kept for the dashboard */

/* flag bits sent by the nRF (byte 3 of its status packet) */
#define F_SMOKE        0x01
#define F_SWITCH       0x02
#define F_REMOTE       0x08

static const char *TAG = "gateway";
static const char *SLAVE_NAME[NUM_BOARDS] = { "BEDROOM1", "BEDROOM2" };

enum { ST_SEARCH = 0, ST_CONNECTING, ST_DISCOVER, ST_CONNECTED };
enum { SRV_IDLE = 0, SRV_OK, SRV_FAIL };

/* ---------------- DASHBOARD PAGE (HTML inside the firmware, nothing else to add) ---------------- */
static const char index_html[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>Smoke Alarm Gateway</title>\n"
    "<style>\n"
    ":root{\n"
    "  --ink:#1c2733; --muted:#66768a; --line:#dfe5ec; --soft:#f5f7fa; --white:#fff; --blue:#1d5fa8;\n"
    "  --ok:#157a46; --ok-bg:#e3f5ea;\n"
    "  --warn:#a85d00; --warn-bg:#fdf0d5;\n"
    "  --bad:#c0292b; --bad-bg:#fde7e7;\n"
    "  --off:#66768a; --off-bg:#eceff3;\n"
    "}\n"
    "*{box-sizing:border-box;margin:0}\n"
    "html{-webkit-text-size-adjust:100%}\n"
    "body{font-family:system-ui,-apple-system,\"Segoe UI\",Roboto,Arial,sans-serif;background:var(--white);color:var(--ink);line-height:1.45}\n"
    ".wrap{max-width:960px;margin:0 auto;padding:0 16px 40px}\n"
    "\n"
    "header{border-bottom:1px solid var(--line)}\n"
    "header .in{max-width:960px;margin:0 auto;padding:16px;display:flex;flex-wrap:wrap;gap:10px 20px;align-items:center;justify-content:space-between}\n"
    "h1{font-size:1.3rem;font-weight:700}\n"
    ".sub{font-size:.85rem;color:var(--muted)}\n"
    ".right{display:flex;align-items:center;gap:18px}\n"
    ".live{display:flex;align-items:center;gap:7px;font-size:.85rem;color:var(--muted)}\n"
    ".dot{width:10px;height:10px;border-radius:50%;background:var(--off);flex:none}\n"
    ".dot.on{background:var(--ok)} .dot.bad{background:var(--bad)}\n"
    ".clock{text-align:right;font-variant-numeric:tabular-nums;line-height:1.2}\n"
    ".clock b{display:block;font-size:1.25rem;font-weight:600}\n"
    ".clock span{font-size:.78rem;color:var(--muted)}\n"
    "\n"
    ".banner{margin-top:18px;border-radius:8px;padding:12px 16px;font-weight:600;border:1px solid}\n"
    ".banner.ok{background:var(--ok-bg);color:var(--ok);border-color:#bfe6cf}\n"
    ".banner.bad{background:var(--bad-bg);color:var(--bad);border-color:#f3bcbc;animation:pulse 1.2s ease-in-out infinite}\n"
    ".banner.off{background:var(--off-bg);color:var(--off);border-color:var(--line)}\n"
    "@keyframes pulse{50%{opacity:.7}}\n"
    "@media (prefers-reduced-motion:reduce){.banner.bad{animation:none}}\n"
    "\n"
    "section{margin-top:26px}\n"
    "h2{font-size:1.05rem;font-weight:700;margin-bottom:4px}\n"
    ".note{font-size:.85rem;color:var(--muted);margin-bottom:10px}\n"
    "\n"
    ".box{border:1px solid var(--line);border-radius:10px;overflow:hidden;background:var(--white)}\n"
    ".boxhead{display:flex;flex-wrap:wrap;gap:8px 14px;align-items:center;justify-content:space-between;padding:12px 16px;background:var(--soft);border-bottom:1px solid var(--line)}\n"
    ".boxhead .t{font-weight:700}\n"
    ".boxhead .m{font-size:.85rem;color:var(--muted)}\n"
    "\n"
    "table{width:100%;border-collapse:collapse}\n"
    "th,td{padding:10px 16px;text-align:left;font-size:.92rem;border-bottom:1px solid var(--line);vertical-align:top}\n"
    "tr:last-child th,tr:last-child td{border-bottom:0}\n"
    "th{width:42%;font-weight:500;color:var(--muted)}\n"
    "td{font-weight:600;word-break:break-word}\n"
    "\n"
    ".tag{display:inline-block;border-radius:5px;padding:1px 9px;font-size:.82rem;font-weight:700}\n"
    ".tag.good{background:var(--ok-bg);color:var(--ok)}\n"
    ".tag.warn{background:var(--warn-bg);color:var(--warn)}\n"
    ".tag.alert{background:var(--bad-bg);color:var(--bad)}\n"
    ".tag.neutral{background:var(--off-bg);color:var(--off)}\n"
    "\n"
    ".json{margin:0;padding:12px 16px;background:var(--soft);border-top:1px solid var(--line);font:.8rem/1.5 ui-monospace,Consolas,Menlo,monospace;color:#334155;white-space:pre-wrap;word-break:break-all}\n"
    ".json b{display:block;font:600 .78rem system-ui,sans-serif;color:var(--muted);margin-bottom:4px}\n"
    "\n"
    ".two{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}\n"
    "@media (max-width:640px){.two{grid-template-columns:1fr}}\n"
    ".room{border-top:5px solid var(--c)}\n"
    ".room.ok{--c:var(--ok)} .room.warn{--c:var(--warn)} .room.bad{--c:var(--bad)} .room.off{--c:var(--off)}\n"
    ".room .boxhead{background:var(--white)}\n"
    ".room .big{font-size:1.15rem;font-weight:700;color:var(--c)}\n"
    "\n"
    ".ev{display:flex;gap:14px;padding:9px 16px;border-bottom:1px solid var(--line);font-size:.9rem}\n"
    ".ev:last-child{border-bottom:0}\n"
    ".ev time{color:var(--muted);min-width:80px;font-variant-numeric:tabular-nums}\n"
    ".empty{color:var(--muted);font-size:.9rem;padding:12px 16px}\n"
    ".foot{text-align:center;color:var(--muted);font-size:.8rem;margin-top:24px}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<header>\n"
    "  <div class=\"in\">\n"
    "    <div>\n"
    "      <h1>Smoke Alarm Gateway</h1>\n"
    "      <div class=\"sub\">Gateway <b id=\"dev\">-</b> &nbsp;|&nbsp; Phones connected <b id=\"cli\">-</b></div>\n"
    "    </div>\n"
    "    <div class=\"right\">\n"
    "      <div class=\"live\"><span class=\"dot\" id=\"d-link\"></span><span id=\"t-link\">Connecting...</span></div>\n"
    "      <div class=\"clock\"><b id=\"clk\">--:--:--</b><span id=\"dte\">Waiting for time</span></div>\n"
    "    </div>\n"
    "  </div>\n"
    "</header>\n"
    "\n"
    "<div class=\"wrap\">\n"
    "  <div class=\"banner\" id=\"banner\" style=\"display:none\"></div>\n"
    "\n"
    "  <section>\n"
    "    <h2>Final packet sent to the TCP server</h2>\n"
    "    <div class=\"note\">The newest message the gateway sent to the server. It is sent whenever something changes, and every 30 seconds as a heartbeat.</div>\n"
    "    <div class=\"box\" id=\"packet\"></div>\n"
    "  </section>\n"
    "\n"
    "  <section>\n"
    "    <h2>Bedroom slaves</h2>\n"
    "    <div class=\"note\">Live status of each alarm board.</div>\n"
    "    <div class=\"two\" id=\"rooms\"></div>\n"
    "  </section>\n"
    "\n"
    "  <section>\n"
    "    <h2>Connections</h2>\n"
    "    <div class=\"two\" id=\"conns\"></div>\n"
    "  </section>\n"
    "\n"
    "  <section>\n"
    "    <h2>Recent events</h2>\n"
    "    <div class=\"box\" id=\"events\"></div>\n"
    "  </section>\n"
    "\n"
    "  <div class=\"foot\">Updates every second. Keep this page open to see alarms as they happen.</div>\n"
    "</div>\n"
    "\n"
    "<script>\n"
    "var $ = function (id) { return document.getElementById(id); };\n"
    "function esc(s) {\n"
    "  return String(s).replace(/[&<>\"']/g, function (c) {\n"
    "    return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c];\n"
    "  });\n"
    "}\n"
    "function tag(text, kind) { return '<span class=\"tag ' + kind + '\">' + esc(text) + '</span>'; }\n"
    "function row(k, v) { return '<tr><th scope=\"row\">' + k + '</th><td>' + v + '</td></tr>'; }\n"
    "\n"
    "var VIEW = {\n"
    "  'NORMAL':       {cls:'ok',   title:'All clear'},\n"
    "  'SMOKE':        {cls:'bad',  title:'Smoke detected'},\n"
    "  'SMOKE+SWITCH': {cls:'bad',  title:'Smoke and switch'},\n"
    "  'SWITCH':       {cls:'warn', title:'Switch pressed'},\n"
    "  'OFFLINE':      {cls:'off',  title:'Offline'}\n"
    "};\n"
    "\n"
    "function ago(s) {\n"
    "  if (s < 0) return 'never';\n"
    "  if (s < 2) return 'just now';\n"
    "  if (s < 60) return s + ' seconds ago';\n"
    "  return Math.floor(s / 60) + ' min ago';\n"
    "}\n"
    "\n"
    "/* ---- final packet ---- */\n"
    "function packet(d) {\n"
    "  var p = d.packet, v = d.server;\n"
    "  var res = v.state === 'OK' ? tag('Delivered', 'good') : v.state === 'FAILED' ? tag('Not delivered', 'alert') : tag('Waiting', 'neutral');\n"
    "  var head = '<div class=\"boxhead\"><div><span class=\"t\">To ' + esc(v.host) + ':' + v.port + '</span> &nbsp;' + res +\n"
    "             '</div><div class=\"m\">' + (v.when ? 'Last attempt ' + esc(v.when) + ' | ' : '') + esc(v.msg) + '</div></div>';\n"
    "  if (!p) {\n"
    "    return head + '<div class=\"empty\">' +\n"
    "      (d.router.connected ? 'No packet sent yet. It is sent as soon as an alarm board is connected.'\n"
    "                          : 'Waiting for the router Wi-Fi. Nothing can be sent yet.') + '</div>';\n"
    "  }\n"
    "  return head + '<table>' +\n"
    "    row('1. Unique ID', esc(p.unique_id)) +\n"
    "    row('2. GSM status', esc(p.gsm_status)) +\n"
    "    row('3. Wi-Fi status', p.wifi_status === 'CONNECTED' ? tag('CONNECTED', 'good') : tag(p.wifi_status, 'neutral')) +\n"
    "    row('4. Slave ID', esc(p.slave_id)) +\n"
    "    row('5. Message', esc(p.message)) +\n"
    "    row('6. Timestamp', esc(p.timestamp)) +\n"
    "    '</table><pre class=\"json\"><b>Exact data sent (JSON)</b>' + esc(JSON.stringify(p)) + '</pre>';\n"
    "}\n"
    "\n"
    "/* ---- one vertical table per bedroom ---- */\n"
    "function room(b) {\n"
    "  var v = VIEW[b.status] || VIEW.OFFLINE;\n"
    "  var link = b.online ? tag('Connected', 'good')\n"
    "           : tag(b.link === 'CONNECTED' ? 'No signal' : b.link.charAt(0) + b.link.slice(1).toLowerCase(), 'neutral');\n"
    "  var last = (b.date.charAt(0) === '-') ? 'Not known yet' : esc(b.date + '  ' + b.time);\n"
    "  return '<div class=\"box room ' + v.cls + '\">' +\n"
    "    '<div class=\"boxhead\"><div><div class=\"t\">' + esc(b.name) + '</div><div class=\"m\">Alarm slave</div></div>' +\n"
    "    '<div class=\"big\">' + v.title + '</div></div><table>' +\n"
    "    row('Status', tag(b.status, v.cls === 'ok' ? 'good' : v.cls === 'bad' ? 'alert' : v.cls === 'warn' ? 'warn' : 'neutral')) +\n"
    "    row('Message', b.message ? esc(b.message) : 'None') +\n"
    "    row('Smoke', b.smoke ? tag('Detected', 'alert') : tag('None', 'good')) +\n"
    "    row('Switch', b.switch ? tag('Pressed', 'alert') : tag('Not pressed', 'good')) +\n"
    "    row('Remote trigger', b.remote ? tag('Active', 'alert') : tag('Off', 'neutral')) +\n"
    "    row('Link to gateway', link) +\n"
    "    row('Wi-Fi', b.wifi === 'CONNECTED' ? tag('Connected', 'good') : tag('Not connected', 'neutral')) +\n"
    "    row('GSM (SMS)', b.gsm === 'CONNECTED' ? tag('Connected', 'good') : tag('Not connected', 'neutral')) +\n"
    "    row('Last message time', last) +\n"
    "    row('Last heard', ago(b.age)) +\n"
    "    '</table></div>';\n"
    "}\n"
    "\n"
    "/* ---- router + server ---- */\n"
    "function conns(d) {\n"
    "  var r = d.router, v = d.server;\n"
    "  var vt = v.state === 'OK' ? tag('Delivering', 'good')\n"
    "         : v.state === 'FAILED' ? tag('Problem', 'alert')\n"
    "         : tag(r.connected ? 'Waiting for first event' : 'Waiting for Wi-Fi', 'neutral');\n"
    "  return '<div class=\"box\"><div class=\"boxhead\"><span class=\"t\">Router Wi-Fi</span></div><table>' +\n"
    "    row('Network', esc(r.ssid)) +\n"
    "    row('Status', r.connected ? tag('Connected', 'good') : tag('Not connected', 'alert')) +\n"
    "    row('Gateway address', r.ip ? esc(r.ip) : 'None yet') +\n"
    "    row('Packets show', r.connected ? 'WIFI:CONNECTED' : 'WIFI:NOT CONNECTED') +\n"
    "    '</table></div>' +\n"
    "    '<div class=\"box\"><div class=\"boxhead\"><span class=\"t\">TCP server</span></div><table>' +\n"
    "    row('Address', esc(v.host) + ':' + v.port) +\n"
    "    row('Status', vt) +\n"
    "    row('Last result', esc(v.msg)) +\n"
    "    row('Last attempt', v.when ? esc(v.when) : 'None yet') +\n"
    "    row('Delivered / failed', v.sent + ' / ' + v.failed) +\n"
    "    '</table></div>';\n"
    "}\n"
    "\n"
    "var lastSync = 0;\n"
    "function syncTime() {\n"
    "  var now = Date.now();\n"
    "  if (now - lastSync < 10000) return;\n"
    "  lastSync = now;\n"
    "  fetch('/api/time?t=' + Math.floor(now / 1000)).catch(function () {});\n"
    "}\n"
    "\n"
    "function render(d) {\n"
    "  $('d-link').className = 'dot on';\n"
    "  $('t-link').textContent = 'Live';\n"
    "  $('dev').textContent = d.device;\n"
    "  $('cli').textContent = d.clients;\n"
    "  if (d.now) {\n"
    "    var p = d.now.split(' ');\n"
    "    $('clk').textContent = p[1];\n"
    "    $('dte').textContent = p[0];\n"
    "  }\n"
    "  if (!d.synced) syncTime();   /* phone clock is used only until the gateway has its own time */\n"
    "\n"
    "  var alarms = [], offline = 0;\n"
    "  d.boards.forEach(function (b) {\n"
    "    if (b.smoke || b.switch) alarms.push(b.name + ' (' + (b.smoke ? 'smoke' : 'switch') + ')');\n"
    "    if (!b.online) offline++;\n"
    "  });\n"
    "  var bn = $('banner');\n"
    "  bn.style.display = 'block';\n"
    "  if (alarms.length) {\n"
    "    bn.className = 'banner bad';\n"
    "    bn.textContent = 'ALERT: ' + alarms.join(' and ');\n"
    "  } else if (offline === d.boards.length) {\n"
    "    bn.className = 'banner off';\n"
    "    bn.textContent = 'No alarm boards are connected yet. Make sure they are powered on.';\n"
    "  } else if (offline) {\n"
    "    bn.className = 'banner off';\n"
    "    bn.textContent = offline + ' of ' + d.boards.length + ' alarm boards offline. The others are all clear.';\n"
    "  } else {\n"
    "    bn.className = 'banner ok';\n"
    "    bn.textContent = 'All rooms are safe.';\n"
    "  }\n"
    "\n"
    "  $('packet').innerHTML = packet(d);\n"
    "  $('rooms').innerHTML = d.boards.map(room).join('');\n"
    "  $('conns').innerHTML = conns(d);\n"
    "  $('events').innerHTML = d.events.length\n"
    "    ? d.events.map(function (e) {\n"
    "        return '<div class=\"ev\"><time>' + esc(e.t) + '</time><span>' + esc(e.m) + '</span></div>';\n"
    "      }).join('')\n"
    "    : '<div class=\"empty\">No events yet.</div>';\n"
    "}\n"
    "\n"
    "function tick() {\n"
    "  var ctl = new AbortController();\n"
    "  var to = setTimeout(function () { ctl.abort(); }, 4000);\n"
    "  fetch('/api/status', {signal: ctl.signal, cache: 'no-store'})\n"
    "    .then(function (r) { return r.json(); })\n"
    "    .then(function (d) { clearTimeout(to); render(d); })\n"
    "    .catch(function () {\n"
    "      clearTimeout(to);\n"
    "      $('d-link').className = 'dot bad';\n"
    "      $('t-link').textContent = 'Lost connection to gateway. Rejoin its Wi-Fi.';\n"
    "    });\n"
    "}\n"
    "\n"
    "syncTime();\n"
    "tick();\n"
    "setInterval(tick, 1000);\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

/* nRF service characteristics (UUID bytes are little-endian) */
static const ble_uuid128_t status_uuid = BLE_UUID128_INIT(
    0xf0,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0x79,0x56,0x34,0x12);
static const ble_uuid128_t alarm_uuid = BLE_UUID128_INIT(
    0xf0,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0x7a,0x56,0x34,0x12);
static const ble_uuid128_t pkt_uuid = BLE_UUID128_INIT(
    0xf0,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0x7b,0x56,0x34,0x12);
static const ble_uuid128_t time_uuid = BLE_UUID128_INIT(
    0xf0,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0x7c,0x56,0x34,0x12);
static const ble_uuid128_t wifi_uuid = BLE_UUID128_INIT(
    0xf0,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0x7d,0x56,0x34,0x12);

typedef struct {
    int      stage;
    bool     ready, got_data, have_pkt;
    uint16_t conn;
    uint16_t h_status, h_alarm, h_pkt, h_time, h_wifi;
    uint8_t  flags;
    int64_t  last_seen_ms;
    char     pkt[PKT_MAX];          /* latest packet text from the nRF */
} node_t;

/* the packet split into its items */
typedef struct {
    char gsm[24];
    char wifi[24];
    char slave[16];
    char msg[64];
    char time[16];
    char date[16];
} decoded_t;

typedef struct {
    char ts[20];
    char text[48];
} event_t;

static node_t  nodes[NUM_BOARDS];
static SemaphoreHandle_t data_lock;
static SemaphoreHandle_t log_lock;          /* protects the event list and the server status */
static uint8_t own_addr_type;
static volatile bool connecting;
static volatile bool got_ip = false;        /* the router Wi-Fi is connected and has given an IP */
static volatile bool ntp_synced = false;    /* clock was set by NTP */
static volatile uint32_t time_gen = 0;      /* +1 every time the clock changes */
static ble_addr_t pending_addr;
static int pending_board;
static char dev_id[24];                     /* "RSSD6c14e4" */
static char sta_ip[24] = "";                /* IP given by the router */
static esp_timer_handle_t retry_timer;

static event_t ev[EV_MAX];
static int ev_head = 0, ev_count = 0;

/* result of the last HTTP POST, shown on the dashboard */
static int  srv_state = SRV_IDLE;
static char srv_msg[64] = "Nothing sent yet";
static char srv_when[20] = "";
static unsigned srv_sent = 0, srv_failed = 0;
static char last_body[320] = "";          /* the newest JSON packet sent (or tried) */

static void start_scan(void);
static int  gap_event(struct ble_gap_event *event, void *arg);

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static bool time_ok(void)   { return time(NULL) > 1700000000; }
static const char *wifi_text(void) { return got_ip ? "CONNECTED" : "NOT CONNECTED"; }

static int find_by_conn(uint16_t conn)
{
    for (int i = 0; i < NUM_BOARDS; i++)
        if (nodes[i].stage >= ST_DISCOVER && nodes[i].conn == conn) return i;
    return -1;
}

/* ---------------- PACKET DECODING ---------------- */
static void field_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void decoded_defaults(decoded_t *d, int board)
{
    field_copy(d->gsm,   sizeof(d->gsm),   "NOT CONNECTED");
    field_copy(d->wifi,  sizeof(d->wifi),  wifi_text());
    field_copy(d->slave, sizeof(d->slave), SLAVE_NAME[board]);
    field_copy(d->msg,   sizeof(d->msg),   "");
    field_copy(d->time,  sizeof(d->time),  "--:--:--");
    field_copy(d->date,  sizeof(d->date),  "--/--/----");
}

/* If the nRF has no time yet, use the gateway's own clock (NTP or phone) */
static void fix_timestamp(decoded_t *d)
{
    if (d->time[0] != '-' || !time_ok()) return;

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(d->time, sizeof(d->time), "%02u:%02u:%02u",
             (unsigned)(tm.tm_hour % 100), (unsigned)(tm.tm_min % 100),
             (unsigned)(tm.tm_sec % 100));
    snprintf(d->date, sizeof(d->date), "%02u/%02u/%04u",
             (unsigned)(tm.tm_mday % 100), (unsigned)((tm.tm_mon + 1) % 100),
             (unsigned)((tm.tm_year + 1900) % 10000));
}

/* GSM:..|WIFI:..|SLAVE:..|MSG:..|TIME:hh:mm:ss|DATE:dd/mm/yyyy  ->  decoded_t */
static void decode_packet(const char *pkt, int board, decoded_t *d)
{
    char tmp[PKT_MAX];
    char *save = NULL;

    decoded_defaults(d, board);
    field_copy(tmp, sizeof(tmp), pkt);

    for (char *tok = strtok_r(tmp, "|", &save); tok != NULL; tok = strtok_r(NULL, "|", &save)) {
        char *colon = strchr(tok, ':');          /* split at the FIRST ':' only */
        if (colon == NULL) continue;
        *colon = '\0';
        const char *key = tok;
        const char *val = colon + 1;
        while (*val == ' ') val++;

        if      (strcmp(key, "GSM")   == 0) field_copy(d->gsm,   sizeof(d->gsm),   val);
        else if (strcmp(key, "SLAVE") == 0) field_copy(d->slave, sizeof(d->slave), val);
        else if (strcmp(key, "MSG")   == 0) field_copy(d->msg,   sizeof(d->msg),   val);
        else if (strcmp(key, "TIME")  == 0) field_copy(d->time,  sizeof(d->time),  val);
        else if (strcmp(key, "DATE")  == 0) field_copy(d->date,  sizeof(d->date),  val);
        /* "WIFI" from the nRF is ignored: the gateway's own live state is used */
    }

    field_copy(d->wifi, sizeof(d->wifi), wifi_text());
    fix_timestamp(d);
}

/* made by the gateway itself, because an offline nRF cannot send anything */
static void decode_offline(int board, decoded_t *d)
{
    decoded_defaults(d, board);
    field_copy(d->msg, sizeof(d->msg), "BOARD OFFLINE");
    fix_timestamp(d);
}

/* NORMAL, SMOKE, SWITCH, SMOKE+SWITCH or OFFLINE */
static const char *state_text(const node_t *n, int64_t now, bool *online_out)
{
    bool online = (n->stage == ST_CONNECTED && n->got_data &&
                   (now - n->last_seen_ms) < OFFLINE_MS);
    bool smoke = online && (n->flags & F_SMOKE);
    bool sw    = online && (n->flags & F_SWITCH);
    *online_out = online;

    if (!online)          return "OFFLINE";
    if (smoke && sw)      return "SMOKE+SWITCH";
    if (smoke)            return "SMOKE";
    if (sw)               return "SWITCH";
    return "NORMAL";
}

/* Message for the server, easy to read for anyone. Both bedrooms are always listed:
 *   BEDROOM1 SMOKE BEDROOM2 NORMAL
 *   BEDROOM1 NORMAL BEDROOM2 SWITCH
 *   BEDROOM1 SMOKE+SWITCH BEDROOM2 OFFLINE */
static void fill_message(decoded_t *d, const node_t *snap, int64_t now)
{
    char msg[sizeof(d->msg)] = "";
    size_t used = 0;

    for (int i = 0; i < NUM_BOARDS; i++) {
        bool online;
        const char *st = state_text(&snap[i], now, &online);
        int w = snprintf(msg + used, sizeof(msg) - used, "%s%s %s",
                         used ? " " : "", SLAVE_NAME[i], st);
        if (w > 0 && (size_t)w < sizeof(msg) - used) used += (size_t)w;
    }
    field_copy(d->msg, sizeof(d->msg), msg);
}

/* ---------------- EVENT HISTORY + SERVER STATUS (shown on the dashboard) ---------------- */
static void stamp(char *ts, size_t cap)
{
    if (time_ok()) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(ts, cap, "%02d:%02d:%02d", tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100);
    } else {
        snprintf(ts, cap, "up %llds", (long long)(now_ms() / 1000));
    }
}

static void push_event(const char *text)
{
    char ts[32];
    stamp(ts, sizeof(ts));

    xSemaphoreTake(log_lock, portMAX_DELAY);
    field_copy(ev[ev_head].ts, sizeof(ev[ev_head].ts), ts);
    field_copy(ev[ev_head].text, sizeof(ev[ev_head].text), text);
    ev_head = (ev_head + 1) % EV_MAX;
    if (ev_count < EV_MAX) ev_count++;
    xSemaphoreGive(log_lock);
    ESP_LOGI(TAG, "event: %s", text);
}

static void srv_update(int state, const char *msg, const char *body)
{
    char ts[32];
    stamp(ts, sizeof(ts));

    xSemaphoreTake(log_lock, portMAX_DELAY);
    srv_state = state;
    if (state == SRV_OK) srv_sent++; else srv_failed++;
    field_copy(srv_msg, sizeof(srv_msg), msg);
    field_copy(srv_when, sizeof(srv_when), ts);
    field_copy(last_body, sizeof(last_body), body);
    xSemaphoreGive(log_lock);
}

/* watches both boards and writes an event whenever something changes */
static void monitor_task(void *param)
{
    uint8_t last_state[NUM_BOARDS];
    int8_t  last_online[NUM_BOARDS];
    memset(last_state, 0xFF, sizeof(last_state));
    memset(last_online, -1, sizeof(last_online));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));

        node_t snap[NUM_BOARDS];
        xSemaphoreTake(data_lock, portMAX_DELAY);
        memcpy(snap, nodes, sizeof(snap));
        xSemaphoreGive(data_lock);
        int64_t now = now_ms();

        for (int i = 0; i < NUM_BOARDS; i++) {
            if (!snap[i].got_data) continue;

            bool online;
            const char *st = state_text(&snap[i], now, &online);
            uint8_t s = snap[i].flags & STATE_MASK;

            if (s != last_state[i] || (int8_t)online != last_online[i]) {
                char m[48];
                snprintf(m, sizeof(m), "%s: %s%s", SLAVE_NAME[i], st,
                         (online && (snap[i].flags & F_REMOTE)) ? " (remote)" : "");
                push_event(m);
                last_state[i] = s;
                last_online[i] = online;
            }
        }
    }
}

/* ---------------- BLE: receive values ---------------- */
static void process_value(uint16_t conn, uint16_t handle, struct os_mbuf *om)
{
    if (om == NULL) return;
    uint16_t len = OS_MBUF_PKTLEN(om);
    uint8_t  b[7] = {0};

    xSemaphoreTake(data_lock, portMAX_DELAY);
    int i = find_by_conn(conn);
    if (i >= 0) {
        node_t *n = &nodes[i];

        if (handle == n->h_status && len == 7) {
            os_mbuf_copydata(om, 0, 7, b);
            n->flags = b[3];
            n->got_data = true;
            n->last_seen_ms = now_ms();
        }
        else if (handle == n->h_pkt && len > 0 && len < PKT_MAX) {
            os_mbuf_copydata(om, 0, len, n->pkt);
            for (int k = 0; k < len; k++)
                if (n->pkt[k] < 32 || n->pkt[k] > 126) n->pkt[k] = '?';
            n->pkt[len] = '\0';
            n->have_pkt = true;
        }
    }
    xSemaphoreGive(data_lock);
}

/* Setup chain: 1 = subscribe status, 2 = subscribe packet, 3 = read status, 4 = read packet */
static int step_cb(uint16_t conn, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    int step = (int)(intptr_t)arg;
    uint16_t h_status = 0, h_pkt = 0;
    int i;

    if (error->status == BLE_HS_EDONE) return 0;

    if (error->status != 0) {
        ESP_LOGW(TAG, "setup step %d failed: %d", step, error->status);
        if (step <= 2) { ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM); return 0; }
    }

    if (step >= 3 && attr != NULL && error->status == 0)
        process_value(conn, attr->handle, attr->om);

    xSemaphoreTake(data_lock, portMAX_DELAY);
    i = find_by_conn(conn);
    if (i >= 0) { h_status = nodes[i].h_status; h_pkt = nodes[i].h_pkt; }
    xSemaphoreGive(data_lock);
    if (i < 0) return 0;

    uint8_t on[2] = { 0x01, 0x00 };

    if (step == 1) {
        ble_gattc_write_flat(conn, h_pkt + 1, on, 2, step_cb, (void *)2);
    } else if (step == 2) {
        ble_gattc_read(conn, h_status, step_cb, (void *)3);
    } else if (step == 3) {
        ble_gattc_read(conn, h_pkt, step_cb, (void *)4);
    } else if (step == 4) {
        xSemaphoreTake(data_lock, portMAX_DELAY);
        i = find_by_conn(conn);
        if (i >= 0) { nodes[i].ready = true; nodes[i].stage = ST_CONNECTED; }
        xSemaphoreGive(data_lock);
        if (i >= 0) ESP_LOGI(TAG, "%s ready", SLAVE_NAME[i]);
    }
    return 0;
}

static int chr_disc_cb(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr != NULL) {
        xSemaphoreTake(data_lock, portMAX_DELAY);
        int i = find_by_conn(conn);
        if (i >= 0) {
            if (ble_uuid_cmp(&chr->uuid.u, &status_uuid.u) == 0)     nodes[i].h_status = chr->val_handle;
            else if (ble_uuid_cmp(&chr->uuid.u, &alarm_uuid.u) == 0) nodes[i].h_alarm  = chr->val_handle;
            else if (ble_uuid_cmp(&chr->uuid.u, &pkt_uuid.u) == 0)   nodes[i].h_pkt    = chr->val_handle;
            else if (ble_uuid_cmp(&chr->uuid.u, &time_uuid.u) == 0)  nodes[i].h_time   = chr->val_handle;
            else if (ble_uuid_cmp(&chr->uuid.u, &wifi_uuid.u) == 0)  nodes[i].h_wifi   = chr->val_handle;
        }
        xSemaphoreGive(data_lock);
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        uint16_t h_status = 0, h_alarm = 0, h_pkt = 0, h_time = 0;
        xSemaphoreTake(data_lock, portMAX_DELAY);
        int i = find_by_conn(conn);
        if (i >= 0) {
            h_status = nodes[i].h_status; h_alarm = nodes[i].h_alarm;
            h_pkt = nodes[i].h_pkt;       h_time  = nodes[i].h_time;
        }
        xSemaphoreGive(data_lock);

        /* h_wifi is optional, so an nRF running older firmware still works */
        if (h_status && h_alarm && h_pkt && h_time) {
            uint8_t on[2] = { 0x01, 0x00 };      /* the CCC sits right after the value handle */
            int rc = ble_gattc_write_flat(conn, h_status + 1, on, 2, step_cb, (void *)1);
            ESP_LOGI(TAG, "subscribing, rc=%d", rc);
        } else {
            ESP_LOGW(TAG, "service incomplete, disconnecting (flash the NEW nRF code)");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        start_scan();                            /* look for the other board */
    } else if (error->status != 0) {
        ESP_LOGW(TAG, "characteristic discovery failed: %d", error->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

/* After the MTU exchange (needed for the ~109 byte packet) start discovery */
static int mtu_cb(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t mtu, void *arg)
{
    ESP_LOGI(TAG, "MTU exchange: status=%d mtu=%u", error->status, mtu);
    ble_gattc_disc_all_chrs(conn, 1, 0xFFFF, chr_disc_cb, NULL);
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        if (connecting) return 0;
        if (event->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND) return 0;

        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0)
            return 0;

        /* manufacturer data from the board: FF FF 'S' <board number> <flags> <score> */
        if (f.mfg_data == NULL || f.mfg_data_len < 4) return 0;
        if (f.mfg_data[0] != 0xFF || f.mfg_data[1] != 0xFF || f.mfg_data[2] != 'S') return 0;

        int board = f.mfg_data[3];
        if (board < 1 || board > NUM_BOARDS) return 0;

        xSemaphoreTake(data_lock, portMAX_DELAY);
        bool wanted = (nodes[board - 1].stage == ST_SEARCH);
        if (wanted) nodes[board - 1].stage = ST_CONNECTING;
        xSemaphoreGive(data_lock);
        if (!wanted) return 0;

        ble_gap_disc_cancel();
        pending_addr = event->disc.addr;
        pending_board = board - 1;
        connecting = true;
        ESP_LOGI(TAG, "Found %s, connecting...", SLAVE_NAME[pending_board]);

        int rc = ble_gap_connect(own_addr_type, &pending_addr, 10000, NULL, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "connect start failed: %d", rc);
            connecting = false;
            xSemaphoreTake(data_lock, portMAX_DELAY);
            nodes[pending_board].stage = ST_SEARCH;
            xSemaphoreGive(data_lock);
            start_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT: {
        connecting = false;
        int b = pending_board;
        if (event->connect.status == 0) {
            uint16_t conn = event->connect.conn_handle;
            xSemaphoreTake(data_lock, portMAX_DELAY);
            memset(&nodes[b], 0, sizeof(node_t));
            nodes[b].stage = ST_DISCOVER;
            nodes[b].conn = conn;
            nodes[b].last_seen_ms = now_ms();
            xSemaphoreGive(data_lock);

            ESP_LOGI(TAG, "%s connected, negotiating MTU...", SLAVE_NAME[b]);
            if (ble_gattc_exchange_mtu(conn, mtu_cb, NULL) != 0)
                ble_gattc_disc_all_chrs(conn, 1, 0xFFFF, chr_disc_cb, NULL);
        } else {
            ESP_LOGW(TAG, "connect to %s failed: %d", SLAVE_NAME[b], event->connect.status);
            xSemaphoreTake(data_lock, portMAX_DELAY);
            nodes[b].stage = ST_SEARCH;
            xSemaphoreGive(data_lock);
            start_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t conn = event->disconnect.conn.conn_handle;
        xSemaphoreTake(data_lock, portMAX_DELAY);
        int i = find_by_conn(conn);
        if (i >= 0) {
            nodes[i].stage = ST_SEARCH;
            nodes[i].ready = false;
        }
        xSemaphoreGive(data_lock);
        if (i >= 0) ESP_LOGW(TAG, "%s disconnected, searching again", SLAVE_NAME[i]);
        start_scan();
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
        process_value(event->notify_rx.conn_handle,
                      event->notify_rx.attr_handle, event->notify_rx.om);
        return 0;

    default:
        return 0;
    }
}

static void start_scan(void)
{
    if (connecting) return;

    bool need = false;
    xSemaphoreTake(data_lock, portMAX_DELAY);
    for (int i = 0; i < NUM_BOARDS; i++) if (nodes[i].stage == ST_SEARCH) need = true;
    xSemaphoreGive(data_lock);
    if (!need) return;

    struct ble_gap_disc_params dp;
    memset(&dp, 0, sizeof(dp));
    dp.passive = 1;
    dp.filter_duplicates = 0;
    dp.itvl = 160;
    dp.window = 80;

    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &dp, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
        ESP_LOGE(TAG, "scan start failed: %d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    ESP_LOGI(TAG, "Looking for BEDROOM1 and BEDROOM2...");
    start_scan();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------- CONTROL TASK: cross-alert + Wi-Fi status + time sync + keep scanning ------------- */
static void control_task(void *param)
{
    uint8_t  last_val[NUM_BOARDS];
    uint8_t  last_wifi[NUM_BOARDS];
    int64_t  last_write[NUM_BOARDS];
    int64_t  last_wifi_wr[NUM_BOARDS];
    int64_t  last_time_wr[NUM_BOARDS];
    uint32_t last_gen[NUM_BOARDS];
    memset(last_val, 0xFF, sizeof(last_val));
    memset(last_wifi, 0xFF, sizeof(last_wifi));
    memset(last_write, 0, sizeof(last_write));
    memset(last_wifi_wr, 0, sizeof(last_wifi_wr));
    memset(last_time_wr, 0, sizeof(last_time_wr));
    memset(last_gen, 0, sizeof(last_gen));
    int cycle = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));
        cycle++;

        bool     ready[NUM_BOARDS];
        uint16_t conn[NUM_BOARDS], h_alarm[NUM_BOARDS], h_time[NUM_BOARDS], h_wifi[NUM_BOARDS];
        uint8_t  other[NUM_BOARDS];
        int64_t  now = now_ms();
        uint8_t  wv = got_ip ? 1 : 0;
        uint32_t gen = time_gen;

        xSemaphoreTake(data_lock, portMAX_DELAY);
        for (int i = 0; i < NUM_BOARDS; i++) {
            ready[i] = nodes[i].ready && nodes[i].stage == ST_CONNECTED;
            conn[i] = nodes[i].conn;
            h_alarm[i] = nodes[i].h_alarm;
            h_time[i] = nodes[i].h_time;
            h_wifi[i] = nodes[i].h_wifi;
            other[i] = 0;
            for (int j = 0; j < NUM_BOARDS; j++) {
                if (j == i) continue;
                if (nodes[j].stage == ST_CONNECTED && nodes[j].got_data &&
                    (now - nodes[j].last_seen_ms) < OFFLINE_MS &&
                    (nodes[j].flags & ALERT_MASK))
                    other[i] = 1;
            }
        }
        xSemaphoreGive(data_lock);

        for (int i = 0; i < NUM_BOARDS; i++) {
            if (!ready[i]) {
                last_val[i] = 0xFF; last_wifi[i] = 0xFF;
                last_time_wr[i] = 0; last_wifi_wr[i] = 0;
                continue;
            }

            bool wrote = false;                  /* only ONE BLE write per board per cycle */

            /* cross-alert: tell this board if the OTHER board has an alarm */
            if (other[i] != last_val[i] || (now - last_write[i]) >= 1000) {
                int rc = ble_gattc_write_flat(conn[i], h_alarm[i], &other[i], 1, NULL, NULL);
                if (rc == 0) { last_val[i] = other[i]; last_write[i] = now; wrote = true; }
            }

            /* Wi-Fi status: 1 = router connected, 0 = not connected (on change + every 5 s) */
            if (!wrote && h_wifi[i] != 0 &&
                (wv != last_wifi[i] || (now - last_wifi_wr[i]) >= 5000)) {
                bool chg = (wv != last_wifi[i]);
                if (ble_gattc_write_flat(conn[i], h_wifi[i], &wv, 1, NULL, NULL) == 0) {
                    last_wifi[i] = wv; last_wifi_wr[i] = now; wrote = true;
                    if (chg)
                        ESP_LOGI(TAG, "%s: Wi-Fi status sent (%s)", SLAVE_NAME[i], wv ? "CONNECTED" : "NOT CONNECTED");
                }
            }

            /* time sync: local time in seconds since 1970, 4 bytes little-endian */
            if (!wrote && time_ok() &&
                (last_time_wr[i] == 0 || (now - last_time_wr[i]) >= TIME_RESYNC_MS ||
                 last_gen[i] != gen)) {
                uint32_t t = (uint32_t)time(NULL) + TZ_OFFSET_SEC;
                uint8_t  b[4] = { t & 0xFF, (t >> 8) & 0xFF, (t >> 16) & 0xFF, (t >> 24) & 0xFF };
                if (ble_gattc_write_flat(conn[i], h_time[i], b, 4, NULL, NULL) == 0) {
                    last_time_wr[i] = now;
                    last_gen[i] = gen;
                    ESP_LOGI(TAG, "%s: time sent", SLAVE_NAME[i]);
                }
            }
        }

        if (cycle % 8 == 0 && !connecting && !ble_gap_disc_active())
            start_scan();
    }
}

/* ---------------- HTTP POST over TCP ---------------- */
typedef struct {
    char buf[200];                  /* first part of the server's reply (status line + headers) */
    int  len;
} tcp_resp_t;

/* keep the JSON valid: the nRF text never has quotes or backslashes, but be safe */
static void clean_field(char *s)
{
    for (; *s; s++)
        if (*s == '"' || *s == '\\') *s = '\'';
}

static void clean_decoded(decoded_t *d)
{
    clean_field(d->gsm);  clean_field(d->wifi); clean_field(d->slave);
    clean_field(d->msg);  clean_field(d->time); clean_field(d->date);
}

/* the items as one JSON object (no trailing newline needed for HTTP) */
static int build_body(char *out, size_t cap, const decoded_t *src)
{
    decoded_t d = *src;
    clean_decoded(&d);

    return snprintf(out, cap,
        "{\"unique_id\":\"%s\",\"gsm_status\":\"%s\",\"wifi_status\":\"%s\","
        "\"slave_id\":\"%s\",\"message\":\"%s\",\"timestamp\":\"%s %s\"}",
        dev_id, d.gsm, d.wifi, d.slave, d.msg, d.date, d.time);
}

/* wrap the JSON in a complete HTTP/1.1 POST request */
static int build_http(char *out, size_t cap, const char *json, int json_len)
{
    char host[40];
    if (SERVER_PORT == 80) snprintf(host, sizeof(host), "%s", SERVER_HOST);
    else                   snprintf(host, sizeof(host), "%s:%d", SERVER_HOST, SERVER_PORT);

    return snprintf(out, cap,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%.*s",
        SERVER_PATH, host, json_len, json_len, json);
}

/* HTTP status code from the reply (200, 201, 422 ...), 0 when there is no valid reply */
static int http_code(const tcp_resp_t *resp)
{
    if (resp->len < 12) return 0;
    if (strncmp(resp->buf, "HTTP/1.", 7) != 0) return 0;
    if (!isdigit((unsigned char)resp->buf[9])) return 0;
    return atoi(resp->buf + 9);
}

/* open TCP connection, send the request, read the reply, close.
 * Returns true when the whole request was sent (check http_code() for the server's answer).
 * On failure err holds a short reason for the dashboard. */
static bool tcp_send(const char *body, int len, tcp_resp_t *resp, char *err, size_t errcap)
{
    resp->len = 0;
    resp->buf[0] = '\0';
    err[0] = '\0';

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_HOST, &dest.sin_addr) != 1) {
        snprintf(err, errcap, "bad server IP");
        ESP_LOGE(TAG, "bad server IP");
        return false;
    }

    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) {
        snprintf(err, errcap, "no free socket (errno %d)", errno);
        ESP_LOGW(TAG, "socket() failed, errno %d", errno);
        return false;
    }

    /* connect with a timeout (non-blocking connect + select) */
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);

    int rc = connect(s, (struct sockaddr *)&dest, sizeof(dest));
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(s, &wset);
        struct timeval tv = { TCP_CONNECT_MS / 1000, (TCP_CONNECT_MS % 1000) * 1000 };
        rc = select(s + 1, NULL, &wset, NULL, &tv);
        if (rc <= 0) {
            snprintf(err, errcap, "server not reachable (timeout)");
            ESP_LOGW(TAG, "TCP connect timeout to %s:%d", SERVER_HOST, SERVER_PORT);
            close(s);
            return false;
        }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (soerr != 0) {
            snprintf(err, errcap, "connection refused (errno %d)", soerr);
            ESP_LOGW(TAG, "TCP connect failed, errno %d", soerr);
            close(s);
            return false;
        }
    } else if (rc < 0) {
        snprintf(err, errcap, "connect failed (errno %d)", errno);
        ESP_LOGW(TAG, "TCP connect failed, errno %d", errno);
        close(s);
        return false;
    }
    fcntl(s, F_SETFL, fl);                       /* back to blocking */

    struct timeval snd = { TCP_SEND_MS / 1000, (TCP_SEND_MS % 1000) * 1000 };
    struct timeval rcv = { TCP_REPLY_MS / 1000, (TCP_REPLY_MS % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));

    int sent = 0;
    while (sent < len) {
        int n = send(s, body + sent, len - sent, 0);
        if (n <= 0) {
            snprintf(err, errcap, "send failed (errno %d)", errno);
            ESP_LOGW(TAG, "TCP send failed, errno %d", errno);
            close(s);
            return false;
        }
        sent += n;
    }

    /* read the reply (first part is enough: status line + headers) */
    int n = recv(s, resp->buf, sizeof(resp->buf) - 1, 0);
    if (n > 0) {
        resp->len = n;
        resp->buf[n] = '\0';
        for (int k = 0; k < n; k++)
            if ((unsigned char)resp->buf[k] < 32 && resp->buf[k] != '\n') resp->buf[k] = ' ';
    }

    shutdown(s, SHUT_RDWR);
    close(s);
    return true;
}

/* Show every message item by item on the serial monitor */
static void print_block(const char *what, const decoded_t *d, const char *body,
                        const char *result, const tcp_resp_t *resp)
{
    printf("\n=========== %s ===========\n", what);
    printf(" 1. Unique ID   : %s\n", dev_id);
    printf(" 2. GSM status  : %s\n", d->gsm);
    printf(" 3. WIFI status : %s\n", d->wifi);
    printf(" 4. Slave ID    : %s\n", d->slave);
    printf(" 5. Message     : %s\n", d->msg);
    printf(" 6. Timestamp   : %s  %s\n", d->date, d->time);
    printf(" POST to        : http://%s:%d%s\n", SERVER_HOST, SERVER_PORT, SERVER_PATH);
    printf(" Body           : %s\n", body);
    printf(" Server reply   : %s\n", resp->len > 0 ? resp->buf : "(none)");
    printf(" Result         : %s\n", result);
    printf("==========================================\n");
}

static void tcp_task(void *param)
{
    char        body[640];
    char        http[900];
    uint8_t     last_state[NUM_BOARDS];
    int8_t      last_online[NUM_BOARDS];
    int64_t     last_send[NUM_BOARDS];
    int64_t     retry_at = 0;
    tcp_resp_t  resp;
    bool        was_up = false;

    memset(last_state, 0xFF, sizeof(last_state));
    memset(last_online, -1, sizeof(last_online));
    memset(last_send, 0, sizeof(last_send));
    resp.len = 0;
    resp.buf[0] = '\0';

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));

        if (!got_ip) {
            was_up = false;
            continue;
        }
        if (!was_up) {                          /* router just connected: send everything again */
            was_up = true;
            memset(last_state, 0xFF, sizeof(last_state));
            memset(last_online, -1, sizeof(last_online));
        }

        int64_t now = now_ms();
        if (now < retry_at) continue;

        for (int i = 0; i < NUM_BOARDS; i++) {
            node_t snap[NUM_BOARDS];                 /* both boards, so each message shows both bedrooms */
            xSemaphoreTake(data_lock, portMAX_DELAY);
            memcpy(snap, nodes, sizeof(snap));
            xSemaphoreGive(data_lock);
            node_t n = snap[i];

            if (!n.got_data) continue;

            bool online  = (n.stage == ST_CONNECTED && (now - n.last_seen_ms) < OFFLINE_MS);
            uint8_t st   = n.flags & STATE_MASK;
            bool changed = (st != last_state[i]) || ((int8_t)online != last_online[i]);
            bool beat    = HEARTBEAT_MS && (now - last_send[i]) >= HEARTBEAT_MS;

            if (!changed && !beat) continue;

            decoded_t d;
            if (online) {
                if (!n.have_pkt) continue;               /* wait for the first packet */
                decode_packet(n.pkt, i, &d);
            } else {
                decode_offline(i, &d);
            }
            fill_message(&d, snap, now);                 /* e.g. BEDROOM1 SMOKE BEDROOM2 NORMAL */

            int len = build_body(body, sizeof(body), &d);
            if (len <= 0 || len >= (int)sizeof(body)) continue;

            int hlen = build_http(http, sizeof(http), body, len);
            if (hlen <= 0 || hlen >= (int)sizeof(http)) continue;

            char err[40];
            bool sent = tcp_send(http, hlen, &resp, err, sizeof(err));
            int  code = sent ? http_code(&resp) : 0;
            bool ok   = (code >= 200 && code < 300);     /* only a 2xx reply counts as success */
            bool rejected = (code >= 400 && code < 500); /* server answered "no": retrying cannot help */

            char result[64];
            if (ok)            snprintf(result, sizeof(result), "Delivered (HTTP %d)", code);
            else if (rejected) snprintf(result, sizeof(result), "Server rejected data (HTTP %d)", code);
            else if (!sent)    snprintf(result, sizeof(result), "%s", err);
            else if (code)     snprintf(result, sizeof(result), "Server error (HTTP %d)", code);
            else               snprintf(result, sizeof(result), "Sent, but no valid reply");

            print_block(changed ? "EVENT" : "HEARTBEAT", &d, body,
                        ok ? "SENT OK (server accepted)" : result, &resp);
            srv_update(ok ? SRV_OK : SRV_FAIL, result, body);

            if (ok || rejected) {
                last_state[i]  = st;
                last_online[i] = online;
                last_send[i]   = now;
            }
            if (!ok) {
                retry_at = now_ms() + SEND_RETRY_MS;     /* do not hammer the server */
                if (!rejected) break;
            }
        }
    }
}

/* ---------------- STATUS AS JSON (read by the dashboard) ---------------- */
static int build_status_json(char *out, size_t cap)
{
    node_t snap[NUM_BOARDS];
    xSemaphoreTake(data_lock, portMAX_DELAY);
    memcpy(snap, nodes, sizeof(snap));
    xSemaphoreGive(data_lock);
    int64_t now = now_ms();

    size_t w = 0;
#define ADD(...) do { if (w < cap) { int r_ = snprintf(out + w, cap - w, __VA_ARGS__); \
                                     if (r_ > 0) w += (size_t)r_; } } while (0)

    wifi_sta_list_t sl;
    int clients = 0;
    if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) clients = sl.num;

    char nowbuf[40] = "";
    if (time_ok()) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(nowbuf, sizeof(nowbuf), "%02d/%02d/%04d %02d:%02d:%02d",
                 tm.tm_mday % 100, (tm.tm_mon + 1) % 100, (tm.tm_year + 1900) % 10000,
                 tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100);
    }

    /* copy the server status once, so the numbers belong together */
    xSemaphoreTake(log_lock, portMAX_DELAY);
    int sstate = srv_state;
    unsigned ssent = srv_sent, sfail = srv_failed;
    char smsg[sizeof(srv_msg)], swhen[sizeof(srv_when)], sbody[sizeof(last_body)];
    field_copy(smsg, sizeof(smsg), srv_msg);
    field_copy(swhen, sizeof(swhen), srv_when);
    field_copy(sbody, sizeof(sbody), last_body);
    xSemaphoreGive(log_lock);
    clean_field(smsg);

    ADD("{\"device\":\"%s\",\"ap_ssid\":\"%s\",\"clients\":%d,"
        "\"synced\":%s,\"now\":\"%s\",\"uptime\":%lld,"
        "\"router\":{\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"%s\"},"
        "\"server\":{\"host\":\"%s\",\"port\":%d,\"state\":\"%s\",\"msg\":\"%s\","
        "\"sent\":%u,\"failed\":%u,\"when\":\"%s\"},\"packet\":%s,\"boards\":[",
        dev_id, AP_SSID, clients, time_ok() ? "true" : "false", nowbuf, (long long)(now / 1000),
        WIFI_SSID, got_ip ? "true" : "false", got_ip ? sta_ip : "",
        SERVER_HOST, SERVER_PORT,
        sstate == SRV_OK ? "OK" : (sstate == SRV_FAIL ? "FAILED" : "IDLE"),
        smsg, ssent, sfail, swhen, sbody[0] ? sbody : "null");

    static const char *LINK[] = { "SEARCHING", "CONNECTING", "SETTING UP", "CONNECTED" };

    for (int i = 0; i < NUM_BOARDS; i++) {
        const node_t *n = &snap[i];
        bool online;
        const char *st = state_text(n, now, &online);

        decoded_t d;
        if (online && n->have_pkt) decode_packet(n->pkt, i, &d);
        else if (online)           decoded_defaults(&d, i);
        else                       decode_offline(i, &d);
        clean_decoded(&d);

        long age = n->got_data ? (long)((now - n->last_seen_ms) / 1000) : -1;

        ADD("%s{\"name\":\"%s\",\"online\":%s,\"status\":\"%s\",\"smoke\":%s,\"switch\":%s,"
            "\"remote\":%s,\"link\":\"%s\",\"gsm\":\"%s\",\"wifi\":\"%s\",\"message\":\"%s\","
            "\"time\":\"%s\",\"date\":\"%s\",\"age\":%ld}",
            i ? "," : "", d.slave, online ? "true" : "false", st,
            (online && (n->flags & F_SMOKE)) ? "true" : "false",
            (online && (n->flags & F_SWITCH)) ? "true" : "false",
            (online && (n->flags & F_REMOTE)) ? "true" : "false",
            LINK[n->stage & 3], d.gsm, d.wifi, d.msg, d.time, d.date, age);
    }

    ADD("],\"events\":[");
    xSemaphoreTake(log_lock, portMAX_DELAY);
    for (int k = 0; k < ev_count; k++) {             /* newest first */
        int idx = (ev_head - 1 - k + EV_MAX * 2) % EV_MAX;
        ADD("%s{\"t\":\"%s\",\"m\":\"%s\"}", k ? "," : "", ev[idx].ts, ev[idx].text);
    }
    xSemaphoreGive(log_lock);
    ADD("]}");
#undef ADD

    if (w >= cap) w = cap - 1;
    return (int)w;
}

/* ---------------- WEB SERVER ---------------- */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html, sizeof(index_html) - 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char *buf = malloc(6144);
    if (buf == NULL) return httpd_resp_send_500(req);
    int len = build_status_json(buf, 6144);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, buf, len);
    free(buf);
    return r;
}

/* the page sends the phone's clock:  /api/time?t=<seconds since 1970, UTC>
 * It is only used while NTP has not set the clock (router without internet). */
static esp_err_t time_handler(httpd_req_t *req)
{
    char q[64], v[16];
    if (!ntp_synced &&
        httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "t", v, sizeof(v)) == ESP_OK) {
        long long t = atoll(v);
        if (t > 1700000000LL) {
            long long cur = (long long)time(NULL);
            if (!time_ok() || llabs(cur - t) > 3) {
                struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                time_gen++;                       /* makes control_task re-send the time to the nRFs */
                ESP_LOGI(TAG, "clock set from browser");
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* any other address (connectivity checks, random sites) -> go to the dashboard */
static esp_err_t redirect_handler(httpd_req_t *req)
{
    char host[48] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));

    /* opened by IP (192.168.x.x): stay on that IP. Opened by a name that our DNS
     * hijacked (connectivity check): send to the access-point address. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", isdigit((unsigned char)host[0]) ? "/" : "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.max_open_sockets = 4;           /* keep sockets free for BLE-side TCP client + DNS */
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server start failed");
        return;
    }

    /* order matters: the first match wins, the catch-all goes last */
    httpd_uri_t u_root   = { .uri = "/",           .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t u_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    httpd_uri_t u_time   = { .uri = "/api/time",   .method = HTTP_GET, .handler = time_handler };
    httpd_uri_t u_any    = { .uri = "/*",          .method = HTTP_GET, .handler = redirect_handler };
    httpd_register_uri_handler(srv, &u_root);
    httpd_register_uri_handler(srv, &u_status);
    httpd_register_uri_handler(srv, &u_time);
    httpd_register_uri_handler(srv, &u_any);
    ESP_LOGI(TAG, "Dashboard ready: join Wi-Fi \"%s\" and open http://192.168.4.1", AP_SSID);
}

/* ---------------- CAPTIVE PORTAL DNS (access-point side): every name -> 192.168.4.1 ---------------- */
static void dns_task(void *param)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(53);
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(s);
        vTaskDelete(NULL);
        return;
    }

    static const uint8_t answer[16] = {
        0xC0, 0x0C,                 /* name = pointer to the question */
        0x00, 0x01, 0x00, 0x01,     /* type A, class IN */
        0x00, 0x00, 0x00, 0x3C,     /* TTL 60 s */
        0x00, 0x04,                 /* 4 bytes follow */
        192, 168, 4, 1
    };
    uint8_t buf[300];

    while (1) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - sizeof(answer), 0, (struct sockaddr *)&from, &fl);
        if (n < 17) continue;

        int q = 12;                                   /* skip the question name */
        while (q < n && buf[q] != 0) q += buf[q] + 1;
        if (q + 5 > n) continue;
        int qend = q + 5;                             /* zero byte + type(2) + class(2) */
        uint16_t qtype = (uint16_t)((buf[q + 1] << 8) | buf[q + 2]);

        buf[2] = 0x81; buf[3] = 0x80;                 /* response, recursion available */
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0;    /* answers: only for A records */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;

        int len = qend;
        if (qtype == 1) { memcpy(buf + len, answer, sizeof(answer)); len += sizeof(answer); }
        sendto(s, buf, len, 0, (struct sockaddr *)&from, fl);
    }
}

/* ---------------- WIFI (station to the router + own access point) + NTP time ---------------- */
static void reconnect_cb(void *arg) { esp_wifi_connect(); }

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        const char *why = "other";
        switch (e->reason) {
        case 201:                    why = "SSID NOT FOUND (wrong name, out of range, or 5 GHz only)"; break;
        case 202: case 15: case 204: why = "AUTH/HANDSHAKE FAILED (wrong password or security mode)"; break;
        case 205:                    why = "connection failed (router refused)"; break;
        case 2:                      why = "auth expired"; break;
        default: break;
        }
        bool was = got_ip;
        got_ip = false;
        sta_ip[0] = '\0';
        if (was) push_event("Router Wi-Fi lost");
        ESP_LOGW(TAG, "Wi-Fi NOT CONNECTED to \"%s\": reason=%d (%s), retrying...",
                 WIFI_SSID, e->reason, why);
        esp_timer_stop(retry_timer);                      /* retry in 3 s, keeps the access point calm */
        esp_timer_start_once(retry_timer, 3000000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        got_ip = true;
        push_event("Router Wi-Fi connected");
        ESP_LOGI(TAG, "Wi-Fi CONNECTED to \"%s\", IP %s  (dashboard: http://%s/)", WIFI_SSID, sta_ip, sta_ip);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "device joined " AP_SSID ": " MACSTR, MAC2STR(e->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "device left " AP_SSID ": " MACSTR, MAC2STR(e->mac));
    }
}

static void ntp_synced_cb(struct timeval *tv)
{
    ntp_synced = true;
    time_gen++;                         /* re-send the corrected time to the nRFs */
    ESP_LOGI(TAG, "clock set by NTP");
}

static void time_start(void)
{
    setenv("TZ", TIMEZONE, 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_set_time_sync_notification_cb(ntp_synced_cb);
    esp_sntp_init();
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();      /* IP 192.168.4.1 */
    esp_netif_create_default_wifi_sta();

    const esp_timer_create_args_t targs = { .callback = reconnect_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &retry_timer));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, NULL));

    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    strncpy((char *)sta.sta.ssid, WIFI_SSID, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, WIFI_PASS, sizeof(sta.sta.password) - 1);

    wifi_config_t apc;
    memset(&apc, 0, sizeof(apc));
    strncpy((char *)apc.ap.ssid, AP_SSID, sizeof(apc.ap.ssid) - 1);
    apc.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    apc.ap.channel = 1;                          /* follows the router's channel once connected */
    apc.ap.max_connection = AP_MAX_CONN;
    if (strlen(AP_PASS) >= 8) {
        strncpy((char *)apc.ap.password, AP_PASS, sizeof(apc.ap.password) - 1);
        apc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        apc.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));

    /* tell phones on the access point to use us as their DNS server (captive portal) */
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(ap, &ip);
    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    esp_netif_dhcps_stop(ap);
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer_dns = 0x02;                                   /* OFFER_DNS */
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));
    esp_netif_dhcps_start(ap);

    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------------- MAIN ---------------- */
void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    data_lock = xSemaphoreCreateMutex();
    log_lock  = xSemaphoreCreateMutex();
    esp_log_level_set("NimBLE", ESP_LOG_WARN);   /* hide the per-write BLE spam */

    /* unique device ID = "RSSD" + last 3 bytes of the ESP32 MAC, lowercase hex
     * MAC 78:42:1C:6C:14:E4  ->  RSSD6c14e4 */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(dev_id, sizeof(dev_id), "RSSD%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device ID: %s", dev_id);

    wifi_start();
    time_start();
    web_start();
    xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_hs_cfg.sync_cb = on_sync;

    xTaskCreate(control_task, "alert_ctl", 4096, NULL, 5, NULL);
    xTaskCreate(monitor_task, "monitor", 4096, NULL, 3, NULL);
    xTaskCreate(tcp_task, "tcp_send", 10240, NULL, 4, NULL);
    nimble_port_freertos_init(host_task);
}