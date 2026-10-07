// Fake device for working on web/index.html without hardware:
//   node test/webui/mock_server.mjs [port]      then open http://localhost:8080/
// Serves the page and imitates the firmware's endpoints in src/WebUI.cpp: the same JSON keys,
// validation messages, status codes and log numbering. MOCK_SCENARIO=latched starts with the
// thermal latch set, =idle with no client.
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';

const port = Number(process.argv[2]) || 8080;
const page = new URL('../../web/index.html', import.meta.url);
const scenario = process.env.MOCK_SCENARIO || 'streaming';
const boot = Date.now();

const defaults = { rate: 32000, buffer: 1024, gain: 3, hpf: true, hpf_hz: 300, agc: false,
  tx: 19.5, ps: false, cpu: 80, led: 1, oh: true, oh_limit: 80, sched: false, sched_h: 24 };
let cfg = { ...defaults };
let server = scenario !== 'latched', latched = scenario === 'latched', streaming = scenario === 'streaming';
let sent = 0, clips = 0, downUntil = 0;
let ohNote = latched ? 'Thermal protection stopped the stream on 2026-10-07 at 14:03 UTC: 81.5 °C for 3 minutes (limit 80 °C).' : '';

const LOG_CAP = 48;
const logs = [];   // every line since boot; the firmware keeps only the last LOG_CAP
const log = (line) => logs.push('[' + new Date().toISOString().slice(11, 19) + 'Z] ' + line);
log('Loaded settings: Rate=32000, Gain=3.0, Buffer=1024');
log('Frame pool: 46 x 2068 B (1472 ms of audio), free heap 174 KB -> 81 KB');
if (streaming) log('STREAMING STARTED for 192.168.1.20');

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
    fw: '3.1.0',
    cfg: { ...cfg, tx: snapTx(cfg.tx) },
    live: {
      uptime: up + 86400 * 2, ip: '192.168.1.50', ssid: 'Garden "AP"', rssi: -63, wifi_drops: 1,
      ps_suspended: false, heap_kb: 82, heap_min_kb: 71, cpu_mhz: cfg.cpu,
      temp_c: 51.2 + Math.random(), temp_max_c: 57.9,
      server, streaming: live, client: live ? '192.168.1.20' : '', session_s: live ? up : 0,
      end_reason: live ? '' : 'client closed connection', connects: 4, plays: 3,
      rate: cfg.rate, frame_ms: Math.round(frameMs * 100) / 100, sent, dropped: 0, overruns: 0, read_errors: 0,
      queue: Math.floor(Math.random() * 3), queue_peak: 6, queue_cap: 46,
      stall_max_ms: 180, stalls: 12, peak_pct: peak, hold_pct: peak + 8, clips,
      agc_x: cfg.agc ? 1.7 : 1, oh_latched: latched, oh_sensor_fault: false, oh_note: ohNote,
      log_seq: logs.length,
    },
  };
}

// Mirrors parseSetting() in src/WebUI.cpp.
function num(v, lo, hi, integral) {
  if (v === '' || !/^\s*[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$/.test(v)) return null;
  const d = Number(v);
  if (!Number.isFinite(d) || d < lo || d > hi || (integral && !Number.isInteger(d))) return null;
  return d;
}
const FLAGS = ['hpf', 'agc', 'ps', 'oh', 'sched'];
function parseSetting(key, v, s) {
  let d;
  switch (key) {
    case 'rate': d = num(v, 16000, 48000, true); if (d === null || !RATES.includes(d)) return 'must be 16000, 22050, 24000, 32000, 44100 or 48000'; s.rate = d; break;
    case 'buffer': d = num(v, 256, 8192, true); if (d === null) return 'must be 256 to 8192 samples'; s.buffer = d; break;
    case 'gain': d = num(v, 0.1, 100, false); if (d === null) return 'must be 0.1 to 100'; s.gain = d; break;
    case 'hpf_hz': d = num(v, 10, 10000, true); if (d === null) return 'must be 10 to 10000 Hz'; s.hpf_hz = d; break;
    case 'tx': d = num(v, -1, 19.5, false); if (d === null) return 'must be -1 to 19.5 dBm'; s.tx = snapTx(d); break;
    case 'cpu': d = num(v, 80, 240, true); if (d === null || ![80, 160, 240].includes(d)) return 'must be 80, 160 or 240'; s.cpu = d; break;
    case 'led': d = num(v, 0, 2, true); if (d === null) return 'must be 0, 1 or 2'; s.led = d; break;
    case 'oh_limit': d = num(v, 30, 95, true); if (d === null) return 'must be 30 to 95'; s.oh_limit = 30 + Math.floor((d - 30) / 5) * 5; break;
    case 'sched_h': d = num(v, 1, 168, true); if (d === null) return 'must be 1 to 168 hours'; s.sched_h = d; break;
    default:
      if (!FLAGS.includes(key)) return 'unknown setting';
      if (['1', 'on', 'true'].includes(v)) s[key] = true;
      else if (['0', 'off', 'false'].includes(v)) s[key] = false;
      else return 'must be 1 or 0';
  }
  return null;
}

function body(req) {
  return new Promise((resolve) => { let b = ''; req.on('data', (c) => { b += c; }); req.on('end', () => resolve(new URLSearchParams(b))); });
}
function json(res, code, obj) { res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' }); res.end(JSON.stringify(obj)); }
const crossSite = (req) => req.headers.origin && req.headers.origin !== 'http://' + req.headers.host;

createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  if (Date.now() < downUntil) { req.socket.destroy(); return; }   // "rebooting"
  if (req.method === 'GET' && url.pathname === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-cache' });
    res.end(readFileSync(page));
  } else if (req.method === 'GET' && url.pathname === '/api/state') {
    json(res, 200, state());
  } else if (req.method === 'POST' && url.pathname === '/api/settings') {
    if (crossSite(req)) { json(res, 403, { error: 'cross-site request refused' }); return; }
    const next = { ...cfg };
    const changes = [];
    for (const [k, v] of await body(req)) {
      const err = parseSetting(k, v, next);
      if (err) { json(res, 400, { error: k + ': ' + err }); return; }
      changes.push(k + '=' + v);
    }
    if (changes.length) log('Web UI settings: ' + changes.join(', '));
    cfg = next;
    json(res, 200, state());
  } else if (req.method === 'POST' && url.pathname === '/api/action') {
    if (crossSite(req)) { json(res, 403, { error: 'cross-site request refused' }); return; }
    const name = (await body(req)).get('name');
    if (name === 'restart_audio') log('Web UI: restart audio');
    else if (name === 'server_on') {
      if (latched) { json(res, 409, { error: 'thermal protection is latched; use Resume streaming' }); return; }
      server = true; log('Web UI: RTSP server started');
    } else if (name === 'server_off') { server = false; log('Web UI: RTSP server stopped'); }
    else if (name === 'clear_thermal') { if (latched) { latched = false; ohNote = ''; server = true; } log('Web UI: thermal latch cleared'); }
    else if (name === 'reboot' || name === 'defaults') {
      log(name === 'reboot' ? 'Web UI: reboot' : 'Web UI: restore defaults and reboot');
      if (name === 'defaults') cfg = { ...defaults };
      downUntil = Date.now() + 4000;
    } else { json(res, 400, { error: 'unknown action' }); return; }
    json(res, 200, { ok: true });
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
