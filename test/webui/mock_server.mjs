// Fake device for working on web/index.html without hardware:
//   node test/webui/mock_server.mjs [port]      then open http://localhost:8080/
// Serves the page and imitates the firmware's endpoints in src/WebUI.cpp: the same JSON keys,
// validation messages, status codes, Host and Origin checks and log numbering. Like the
// firmware's log ring the log survives the mock's restarts and keeps its numbering.
// MOCK_SCENARIO=latched starts with the thermal latch set; =idle is a device that has just
// started: no client so far, no WiFi drop and no temperature reading yet.
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';

const port = Number(process.argv[2]) || 8080;
const page = new URL('../../web/index.html', import.meta.url);
const scenario = process.env.MOCK_SCENARIO || 'streaming';
const idle = scenario === 'idle';
let boot = Date.now(), uptimeBase = idle ? 0 : 86400 * 2, restartReason = idle ? 'power on' : 'scheduled reboot';

const defaults = { rate: 32000, buffer: 1024, gain: 3, hpf: true, hpf_hz: 300, agc: false,
  tx: 19.5, ps: false, cpu: 80, led: 1, oh: true, oh_limit: 80, sched: false, sched_h: 24, name: 'atomecho' };
let cfg = { ...defaults };
let server = scenario !== 'latched', latched = scenario === 'latched', streaming = scenario === 'streaming';
let sent = 0, clips = 0, downUntil = 0;
// A restart: no answers for ms, then uptime counts from zero and the reason is the note given.
function restart(ms, reason) { downUntil = Date.now() + ms; boot = downUntil; uptimeBase = 0; restartReason = reason; }
let listenRes = null, listenerIp = '';   // the one live listener, as on the device
let ohNote = latched ? 'Thermal protection stopped the stream on 2026-10-07 at 14:03 UTC: 81.5 °C for 3 minutes (limit 80 °C).' : '';
// The name mDNS answers to. For tests, the mock pretends another device on the network already
// holds "taken", so a device named taken answers as taken-2; any other name is its own mDNS name.
const mdnsName = () => (cfg.name === 'taken' ? 'taken-2' : cfg.name);

const LOG_CAP = 39;
const logs = [];   // every line since the mock started; the firmware keeps only the last LOG_CAP
const log = (line) => logs.push('[' + new Date().toISOString().slice(11, 19) + 'Z] ' + line);
log('Loaded settings: Rate=32000, Gain=3.0, Buffer=1024');
log('Frame pool: 45 x 2068 B (1440 ms of audio), 23 in IRAM, free heap 130 -> 80 KB');
if (streaming) log('STREAMING STARTED for 192.168.1.20');

const NOT_AN_IMAGE = 'not a firmware image; upload firmware.bin (the other .bin files of a release are for flashing over USB)';
const ON_PROBATION = 'the running firmware is not confirmed yet; wait until the log says "Firmware update confirmed" (about a minute after start, with WiFi), then upload again';
const UNKNOWN_HOST = 'unknown host name: open the device by its IP address or name.local';
const RATES = [16000, 22050, 24000, 32000, 44100, 48000];
const TX = [-1, 2, 5, 7, 8.5, 11, 13, 15, 17, 18.5, 19, 19.5];
const snapTx = (d) => TX.reduce((best, s) => (Math.abs(s - d) < Math.abs(best - d) ? s : best), TX[0]);

function state() {
  const up = Math.floor((Date.now() - boot) / 1000);
  const frameMs = cfg.buffer * 1000 / cfg.rate;
  const live = streaming && server;
  if (live) sent += Math.round(1000 / frameMs);
  const peak = 15 + Math.random() * 40;
  if (Math.random() < 0.05) clips++;
  return {
    fw: '4.1.0',
    cfg: { ...cfg, tx: snapTx(cfg.tx) },
    live: {
      uptime: up + uptimeBase, restart_reason: restartReason, ip: '192.168.1.50', mdns: mdnsName(), ssid: 'Garden "AP"',
      rssi: -63, bssid: '3C:84:6A:12:34:56', channel: 6,
      wifi_drops: idle ? 0 : 1, wifi_last_drop: idle ? null : 'BEACON_TIMEOUT', wifi_last_drop_s: idle ? null : 7380 + up,
      ps_suspended: false, heap_kb: 82, heap_min_kb: 71, cpu_mhz: cfg.cpu,
      temp_c: idle ? null : 51.2 + Math.random(), temp_max_c: idle ? null : 57.9,
      server, streaming: live, client: live ? '192.168.1.20' : '', session_s: live ? up : 0,
      end_reason: live || idle ? '' : 'client closed connection', connects: idle ? 0 : 4, plays: idle ? 0 : 3, listener: listenerIp,
      rate: cfg.rate, frame_ms: Math.round(frameMs * 100) / 100, sent, dropped: 0, overruns: 0, read_errors: 0,
      queue: Math.floor(Math.random() * 3), queue_peak: idle ? 0 : 6, queue_cap: 45,
      stall_max_ms: idle ? 0 : 180, stalls: idle ? 0 : 12, peak_pct: peak, hold_pct: peak + 8, clips,
      agc_x: cfg.agc ? 1.7 : 1, mic_flat: scenario === 'latched', oh_latched: latched, oh_sensor_fault: false, oh_note: ohNote,
      log_seq: logs.length,
    },
  };
}

// Mirrors parseNum() and parseSetting() in src/SettingsParse.h.
function num(v, lo, hi, integral) {
  if (v === '' || !/^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$/.test(v)) return null;
  const d = Number(v);
  if (!Number.isFinite(d) || d < lo || d > hi || (integral && !Number.isInteger(d))) return null;
  return d;
}
const FLAGS = ['hpf', 'agc', 'ps', 'oh', 'sched'];
function parseSetting(key, v, s) {
  let d;
  switch (key) {
    case 'rate': d = num(v, 16000, 48000, true); if (d === null || !RATES.includes(d)) return 'must be 16000, 22050, 24000, 32000, 44100 or 48000'; s.rate = d; break;
    case 'buffer': d = num(v, 256, 4096, true); if (d === null) return 'must be 256 to 4096 samples'; s.buffer = d; break;
    case 'gain': d = num(v, 0.1, 100, false); if (d === null) return 'must be 0.1 to 100'; s.gain = d; break;
    case 'hpf_hz': d = num(v, 10, 10000, true); if (d === null) return 'must be 10 to 10000 Hz'; s.hpf_hz = d; break;
    case 'tx': d = num(v, -1, 19.5, false); if (d === null) return 'must be -1 to 19.5 dBm'; s.tx = snapTx(d); break;
    case 'cpu': d = num(v, 80, 240, true); if (d === null || ![80, 160, 240].includes(d)) return 'must be 80, 160 or 240'; s.cpu = d; break;
    case 'led': d = num(v, 0, 2, true); if (d === null) return 'must be 0, 1 or 2'; s.led = d; break;
    case 'oh_limit': d = num(v, 30, 95, true); if (d === null) return 'must be 30 to 95'; s.oh_limit = 30 + Math.floor((d - 30) / 5) * 5; break;
    case 'sched_h': d = num(v, 1, 168, true); if (d === null) return 'must be 1 to 168 hours'; s.sched_h = d; break;
    case 'name': {
      const n = v.replace(/[A-Z]/g, (c) => c.toLowerCase());   // ASCII only, as the firmware
      if (n.length < 1 || n.length > 31 || !/^[a-z0-9-]+$/.test(n) || n.startsWith('-') || n.endsWith('-')) return 'must be 1 to 31 letters, digits or hyphens, with no hyphen at either end';
      s.name = n; break;
    }
    default:
      if (!FLAGS.includes(key)) return 'unknown setting';
      if (['1', 'on', 'true'].includes(v)) s[key] = true;
      else if (['0', 'off', 'false'].includes(v)) s[key] = false;
      else return 'must be 1 or 0';
  }
  return null;
}

// A WAV header for an endless 16-bit mono stream, as wavHeader() in src/AudioDsp.h writes it.
function wavHeader(rate) {
  const h = Buffer.alloc(44), data = 0x7fff0000;
  h.write('RIFF', 0); h.writeUInt32LE(36 + data, 4); h.write('WAVEfmt ', 8);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(1, 22);
  h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * 2, 28); h.writeUInt16LE(2, 32); h.writeUInt16LE(16, 34);
  h.write('data', 36); h.writeUInt32LE(data, 40);
  return h;
}
// Something to look at in the spectrogram: hiss, a rising call every 1.5 s, a high trill every 4 s.
function birdSample(t) {
  let s = (Math.random() * 2 - 1) * 250;
  const p = t % 1.5;
  if (p < 0.25) s += Math.sin(Math.PI * p / 0.25) * 9000 * Math.sin(2 * Math.PI * (3000 * p + 3000 * p * p / 0.5));
  const q = t % 4;
  if (q > 2 && q < 2.4) s += (Math.floor(q * 40) % 2) * 5000 * Math.sin(2 * Math.PI * 9500 * t);
  return Math.max(-32768, Math.min(32767, Math.round(s)));
}
function endListening(why) {
  if (!listenRes) return;
  const res = listenRes;
  listenRes = null;
  log('Listening ended (' + why + ') for ' + listenerIp);
  listenerIp = '';
  res.destroy();
}

function body(req) {
  return new Promise((resolve) => { let b = ''; req.on('data', (c) => { b += c; }); req.on('end', () => resolve(new URLSearchParams(b))); });
}
function json(res, code, obj) { res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' }); res.end(JSON.stringify(obj)); }
const crossSite = (req) => req.headers.origin && req.headers.origin !== 'http://' + req.headers.host;

// isDeviceHost() in src/HostCheck.h: an IPv4 address, or the name alone or under one of these
// domains; any port, a trailing dot ignored, an empty Host accepted.
const DOMAINS = ['', '.local', '.lan', '.home', '.home.arpa', '.internal', '.localdomain', '.fritz.box'];
function isDeviceHost(host, name) {
  let h = host.split(':')[0];
  if (h.length > 95) return false;
  h = h.replace(/[A-Z]/g, (c) => c.toLowerCase()).replace(/\.$/, '');
  if (h === '') return true;
  if (/^\d+\.\d+\.\d+\.\d+$/.test(h) && h.split('.').every((p) => Number(p) <= 255)) return true;
  return name !== '' && h.startsWith(name) && DOMAINS.includes(h.slice(name.length));
}
// knownHost() in src/WebUI.cpp: the configured name or the mDNS name. Unlike the firmware the mock
// also accepts localhost (any port), where it runs for development.
function knownHost(req) {
  const host = req.headers.host || '';
  return isDeviceHost(host, cfg.name) || isDeviceHost(host, mdnsName()) || /^localhost(:|$)/i.test(host);
}
// fromThisPage() in src/WebUI.cpp, first for every POST: Host, then Origin. Answers and returns
// false when the request is refused.
function fromThisPage(req, res) {
  if (!knownHost(req)) { json(res, 403, { error: UNKNOWN_HOST }); return false; }
  if (crossSite(req)) { json(res, 403, { error: 'cross-site request refused' }); return false; }
  return true;
}
const GET_PATHS = ['/', '/api/state', '/api/logs', '/api/listen'];

createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  if (Date.now() < downUntil) { req.socket.destroy(); return; }   // "rebooting"
  // deviceHostOnly() in src/WebUI.cpp. A path the device does not serve gets its 404 whatever the Host.
  if (req.method === 'GET' && GET_PATHS.includes(url.pathname) && !knownHost(req)) { json(res, 403, { error: UNKNOWN_HOST }); return; }
  if (req.method === 'GET' && url.pathname === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-cache' });
    res.end(readFileSync(page));
  } else if (req.method === 'GET' && url.pathname === '/api/state') {
    json(res, 200, state());
  } else if (req.method === 'POST' && url.pathname === '/api/settings') {
    if (!fromThisPage(req, res)) return;
    const next = { ...cfg };
    const changes = [];
    for (const [k, v] of await body(req)) {
      const err = parseSetting(k, v, next);
      if (err) { json(res, 400, { error: k + ': ' + err }); return; }
      changes.push(k + '=' + v);
    }
    if (changes.length) log('Web UI settings: ' + changes.join(', '));
    if (next.rate !== cfg.rate || next.buffer !== cfg.buffer) endListening('audio reconfiguration');
    cfg = next;
    json(res, 200, state());
  } else if (req.method === 'POST' && url.pathname === '/api/action') {
    if (!fromThisPage(req, res)) return;
    const name = (await body(req)).get('name');
    if (name === 'restart_audio') { log('Web UI: restart audio'); endListening('audio reconfiguration'); }
    else if (name === 'server_on') {
      if (latched) { json(res, 409, { error: 'thermal protection is latched; use Resume streaming' }); return; }
      server = true; log('Web UI: RTSP server started');
    } else if (name === 'server_off') { server = false; log('Web UI: RTSP server stopped'); }
    else if (name === 'clear_thermal') { if (latched) { latched = false; ohNote = ''; server = true; } log('Web UI: thermal latch cleared'); }
    else if (name === 'reboot' || name === 'defaults') {
      log(name === 'reboot' ? 'Web UI: reboot' : 'Web UI: restore defaults and reboot');
      if (name === 'defaults') cfg = { ...defaults };
      restart(4000, name === 'reboot' ? 'Web UI reboot' : 'defaults restored from the Web UI');
    } else if (name === 'forget_wifi') {
      log('Web UI: forget the WiFi network and restart into the setup portal');
      restart(60000, 'WiFi network forgotten from the Web UI');
    } else { json(res, 400, { error: 'unknown action' }); return; }
    json(res, 200, { ok: true });
  } else if (req.method === 'POST' && url.pathname === '/api/update') {
    // A request that fails the Host or Origin check, as the firmware treats it. WebServer hands a
    // body that is not multipart to httpUpdateUpload() before reading any of it (raw mode, even
    // with no body at all), and a multipart body once a file part starts; httpUpdateUpload() then
    // closes the connection without a reply. A multipart body without a file part never reaches
    // it: WebServer reads the body and httpUpdateDone() answers through fromThisPage().
    const type = req.headers['content-type'] || '';
    const refused = !knownHost(req) || crossSite(req);
    if (refused && !type.startsWith('multipart/')) { req.socket.destroy(); return; }
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const raw = Buffer.concat(chunks);
    if (refused) {
      if (/^Content-Disposition:.*filename=/im.test(raw.toString('latin1'))) req.socket.destroy();
      else fromThisPage(req, res);
      return;
    }
    // Multipart body: the file's bytes run from the end of the part's header block to the boundary.
    const start = raw.indexOf('\r\n\r\n');
    const multipart = type.startsWith('multipart/form-data');
    if (!multipart || start < 0 || raw.length <= start + 4) { json(res, 400, { error: 'no firmware file in the request' }); return; }
    const end = raw.indexOf('\r\n--' + type.slice(type.indexOf('boundary=') + 9).replace(/"/g, ''), start + 4);
    const file = raw.subarray(start + 4, end < 0 ? raw.length : end);
    // isAppImage() in src/WebUI.cpp: 0xE9, then the app descriptor's magic word at offset 32.
    if (file.length < 36 || file[0] !== 0xe9 || file.readUInt32LE(32) !== 0xabcd5432) {
      log('Firmware update failed: ' + NOT_AN_IMAGE); json(res, 400, { error: NOT_AN_IMAGE }); return;
    }
    // Test hooks, by the image's second byte: 1 = the connection drops after the image was
    // verified (the device restarts into it), 2 = it drops before the end of the file, 3 = it
    // drops and the device restarts for another reason before the end of the file, 4 = the
    // running firmware is an update not confirmed yet (the device refuses any file then).
    if (file[1] === 4) { log('Firmware update failed: ' + ON_PROBATION); json(res, 409, { error: ON_PROBATION }); return; }
    if (file[1] === 2) { log('Firmware update interrupted: the upload connection dropped'); req.socket.destroy(); return; }
    if (file[1] === 3) { restart(4000, 'recovery: main loop stuck for 60 s'); req.socket.destroy(); return; }
    if (file[1] === 1) {
      log('Firmware update written, but the connection closed before the reply; restarting into it');
      restart(4000, 'firmware update');
      req.socket.destroy();
      return;
    }
    log('Firmware update written; restarting into it');
    restart(4000, 'firmware update');
    json(res, 200, { ok: true });
  } else if (req.method === 'GET' && url.pathname === '/api/listen') {
    const range = req.headers.range || '';
    if (range && !range.startsWith('bytes=0-')) { json(res, 416, { error: 'a live stream cannot be resumed' }); return; }
    const from = req.socket.remoteAddress.replace(/^::ffff:/, '');
    if (listenRes && from === listenerIp) endListening('replaced by a new request');   // same address: replace
    if (listenRes) { json(res, 409, { error: 'someone is already listening' }); return; }
    if (latched) { json(res, 503, { error: 'thermal protection is latched' }); return; }
    listenRes = res;
    listenerIp = from;
    log('Listening started for ' + listenerIp);
    res.writeHead(200, { 'Content-Type': 'audio/wav', 'Cache-Control': 'no-store', Connection: 'close' });
    res.write(wavHeader(cfg.rate));
    const rate = cfg.rate, started = Date.now();
    let t = 0;
    const tick = setInterval(() => {
      const n = Math.floor((Date.now() - started) / 1000 * rate) - t;
      if (n <= 0) return;
      const buf = Buffer.alloc(n * 2);
      for (let i = 0; i < n; i++, t++) buf.writeInt16LE(birdSample(t / rate), 2 * i);
      res.write(buf);
    }, 50);
    res.on('close', () => { clearInterval(tick); if (listenRes === res) endListening('listener closed the connection'); });
  } else if (req.method === 'GET' && url.pathname === '/api/logs') {
    const end = logs.length;
    let first = Number.parseInt(url.searchParams.get('since') || '0', 10) || 0;
    if (first < 0 || first > end) first = 0;
    if (end > LOG_CAP && first < end - LOG_CAP) first = end - LOG_CAP;
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store', 'X-Log-Seq': String(end) });
    res.end(logs.slice(first).map((l) => l + '\n').join(''));
  } else {
    res.writeHead(404, { 'Content-Type': 'text/plain' }); res.end('Not found');
  }
}).listen(port, () => console.log('mock device on http://localhost:' + port + '/ (' + scenario + ')'));
