// Drives web/index.html in headless Chrome against mock_server.mjs and checks the page's behaviour.
//   1. Start Chrome with the DevTools protocol on port 9222, e.g.
//        chrome --headless --remote-debugging-port=9222 about:blank
//      or in a container sharing the host network:
//        podman run -d --rm --network host --name ui-chrome docker.io/zenika/alpine-chrome:124 \
//          --headless --no-sandbox --remote-debugging-address=127.0.0.1 --remote-debugging-port=9222 about:blank
//   2. node test/webui/ui_test.mjs [screenshot-dir]
// Needs Node 22 or later (global WebSocket). Exits non-zero if a check fails.
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';

const CDP = process.env.CDP || 'http://127.0.0.1:9222';
const shotDir = process.argv[2] || '';
let failures = 0, checks = 0;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function check(cond, what, detail) {
  checks++;
  if (cond) return;
  failures++;
  console.log('FAIL: ' + what + (detail === undefined ? '' : ' (got ' + JSON.stringify(detail) + ')'));
}

async function startMock(port, scenario) {
  const proc = spawn(process.execPath, [new URL('./mock_server.mjs', import.meta.url).pathname, String(port)],
    { env: { ...process.env, MOCK_SCENARIO: scenario }, stdio: ['ignore', 'pipe', 'inherit'] });
  await new Promise((resolve) => proc.stdout.once('data', resolve));
  return proc;
}

async function openPage() {
  const target = await (await fetch(CDP + '/json/new?about:blank', { method: 'PUT' })).json();
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
  let nextId = 1;
  const pending = new Map(), listeners = [];
  ws.onmessage = (m) => {
    const msg = JSON.parse(m.data);
    if (msg.id && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(msg.error.message)); else resolve(msg.result);
    } else if (msg.method) {
      for (const l of listeners) l(msg);
    }
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    ws.send(JSON.stringify({ id, method, params }));
  });
  const once = (method) => new Promise((resolve) => {
    const l = (msg) => { if (msg.method === method) { listeners.splice(listeners.indexOf(l), 1); resolve(msg.params); } };
    listeners.push(l);
  });
  const evaluate = async (expression) => {
    const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error(expression + ': ' + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
    return r.result.value;
  };
  await send('Page.enable');
  await send('Runtime.enable');
  // Record every request the page makes, so checks can see what was sent.
  const requests = [];
  await send('Network.enable');
  listeners.push((msg) => {
    if (msg.method === 'Network.requestWillBeSent') {
      const r = msg.params.request;
      requests.push({ method: r.method, url: r.url, body: r.postData || '' });
    }
  });
  const close = async () => { ws.close(); await fetch(CDP + '/json/close/' + target.id); };
  return { send, once, evaluate, requests, close };
}

async function load(page, url, width = 1280, height = 900, mobile = false) {
  await page.send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile });
  const loaded = page.once('Page.loadEventFired');
  await page.send('Page.navigate', { url });
  await loaded;
  await page.evaluate('window.confirm = () => true');   // a real dialog would block the page
  await waitFor(page, "document.getElementById('status').textContent !== 'Loading…'", 'first render');
}

async function waitFor(page, expr, what, timeoutMs = 4000) {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    if (await page.evaluate('!!(' + expr + ')')) return true;
    await sleep(50);
  }
  check(false, 'timed out waiting for ' + what);
  return false;
}

async function screenshot(page, name) {
  if (!shotDir) return;
  const { cssContentSize } = await page.send('Page.getLayoutMetrics');
  const { data } = await page.send('Page.captureScreenshot', {
    format: 'png', captureBeyondViewport: true,
    clip: { x: 0, y: 0, width: cssContentSize.width, height: cssContentSize.height, scale: 1 },
  });
  mkdirSync(shotDir, { recursive: true });
  writeFileSync(shotDir + '/' + name + '.png', Buffer.from(data, 'base64'));
}

const text = (page, id) => page.evaluate(`document.getElementById(${JSON.stringify(id)}).textContent`);
const field = (name) => `document.querySelector('[name=${name}]')`;
const formOf = (name) => `${field(name)}.form`;
const setValue = (page, name, value) => page.evaluate(
  `(() => { const e = ${field(name)}; e.value = ${JSON.stringify(value)}; e.dispatchEvent(new Event('input', { bubbles: true })); })()`);

async function streamingScenario(base) {
  const page = await openPage();
  await load(page, base);

  check(await text(page, 'status') === 'Streaming', 'status pill shows Streaming', await text(page, 'status'));
  check(await text(page, 'url') === 'rtsp://192.168.1.50:8554/audio', 'RTSP URL from the device address', await text(page, 'url'));
  check(await page.evaluate(`${field('gain')}.value`) === '3', 'gain field filled from cfg', await page.evaluate(`${field('gain')}.value`));
  check(await page.evaluate(`${field('rate')}.value`) === '32000', 'rate select filled from cfg');
  check((await text(page, 'wifi')).includes('Garden "AP"'), 'SSID shown as text, quotes intact', await text(page, 'wifi'));
  check((await text(page, 'format')).startsWith('32 kHz'), 'audio format line', await text(page, 'format'));
  check(await text(page, 'restart') === 'scheduled reboot', 'restart reason shown', await text(page, 'restart'));
  check(await text(page, 'ap') === '3C:84:6A:12:34:56, channel 6', 'access point and channel', await text(page, 'ap'));
  check(/^1 since boot, last 2h \d+m ago \(BEACON_TIMEOUT\)$/.test(await text(page, 'drops')), 'last WiFi drop', await text(page, 'drops'));
  check(await page.evaluate("document.getElementById('micFlat').hidden"), 'no microphone warning while it delivers signal');
  const frameLabel = await page.evaluate(`${field('buffer')}.selectedOptions[0].textContent`);
  check(frameLabel === '1024 samples (32 ms)', 'frame size label shows duration at the current rate', frameLabel);
  await screenshot(page, 'desktop');

  // Edit -> dirty -> Save posts only the edited field; the reply clears the dirty mark.
  await setValue(page, 'gain', '4.5');
  check(await page.evaluate(`${field('gain')}.classList.contains('dirty')`), 'edited field marked dirty');
  check(await page.evaluate(`!${formOf('gain')}.querySelector('button[type=submit]').disabled`), 'Save enabled after an edit');
  // A poll must not overwrite a field being edited.
  await sleep(1300);
  check(await page.evaluate(`${field('gain')}.value`) === '4.5', 'poll leaves a dirty field alone');
  page.requests.length = 0;
  await page.evaluate(`${formOf('gain')}.requestSubmit()`);
  await waitFor(page, `${formOf('gain')}.querySelector('.msg').textContent === 'Saved.'`, 'Saved message');
  const post = page.requests.find((r) => r.url.endsWith('/api/settings'));
  check(post && post.body === 'gain=4.5', 'only the edited field is posted', post && post.body);
  check(await page.evaluate(`!${field('gain')}.classList.contains('dirty')`), 'dirty mark cleared after save');
  check(await page.evaluate(`${formOf('gain')}.querySelector('button[type=submit]').disabled`), 'Save disabled again after save');

  // The browser's form validation stops an out-of-range value before any request.
  page.requests.length = 0;
  await setValue(page, 'hpf_hz', '5');
  await page.evaluate(`${formOf('hpf_hz')}.requestSubmit()`);
  await sleep(200);
  check(await page.evaluate(`!${field('hpf_hz')}.validity.valid`), 'out-of-range value flagged invalid');
  check(!page.requests.some((r) => r.url.endsWith('/api/settings')), 'invalid value not sent');

  // Undo restores the device value.
  await page.evaluate(`${formOf('hpf_hz')}.reset()`);
  check(await page.evaluate(`${field('hpf_hz')}.value`) === '300', 'Undo restores the saved value', await page.evaluate(`${field('hpf_hz')}.value`));
  check(await page.evaluate(`!${field('hpf_hz')}.classList.contains('dirty')`), 'Undo clears the dirty mark');

  // A value the device rejects shows the device's message.
  await page.evaluate(`(() => { const s = ${field('buffer')}; s.add(new Option('9000', '9000')); s.value = '9000'; s.dispatchEvent(new Event('change', { bubbles: true })); })()`);
  await page.evaluate(`${formOf('buffer')}.requestSubmit()`);
  await waitFor(page, `${formOf('buffer')}.querySelector('.msg').textContent.startsWith('Not saved')`, 'server error message');
  const srvMsg = await page.evaluate(`${formOf('buffer')}.querySelector('.msg').textContent`);
  check(srvMsg === 'Not saved: buffer: must be 256 to 8192 samples', 'device error shown', srvMsg);
  check(await page.evaluate(`${field('buffer')}.classList.contains('dirty')`), 'rejected field stays dirty');
  await page.evaluate(`${formOf('buffer')}.reset()`);

  // A gain stored by an older version with more decimals must not block saving the form.
  check(await page.evaluate(`(() => { const e = ${field('gain')}; e.value = '3.25'; return e.validity.valid; })()`),
    'gain with two decimals is valid');
  await page.evaluate(`${formOf('gain')}.reset()`);

  // Checkbox round trip.
  await page.evaluate(`(() => { const e = ${field('agc')}; e.click(); })()`);
  page.requests.length = 0;
  await page.evaluate(`${formOf('agc')}.requestSubmit()`);
  await waitFor(page, `${formOf('agc')}.querySelector('.msg').textContent === 'Saved.'`, 'agc saved');
  const agcPost = page.requests.find((r) => r.url.endsWith('/api/settings'));
  check(agcPost && agcPost.body === 'agc=1', 'checkbox posted as 1', agcPost && agcPost.body);
  await waitFor(page, "document.getElementById('agcText').textContent.startsWith('AGC')", 'AGC multiplier shown');

  // Text setting: a valid name is saved, an invalid one is stopped by the form's pattern.
  await setValue(page, 'name', 'garden-mic');
  page.requests.length = 0;
  await page.evaluate(`${formOf('name')}.requestSubmit()`);
  await waitFor(page, `${formOf('name')}.querySelector('.msg').textContent === 'Saved.'`, 'name saved');
  const namePost = page.requests.find((r) => r.url.endsWith('/api/settings'));
  check(namePost && namePost.body === 'name=garden-mic', 'name posted', namePost && namePost.body);
  check((await text(page, 'ip')).endsWith('garden-mic.local'), 'address shows the new name', await text(page, 'ip'));
  await setValue(page, 'name', 'bad name-');
  check(await page.evaluate(`!${field('name')}.validity.valid`), 'invalid name flagged by the form');
  await page.evaluate(`${formOf('name')}.reset()`);

  // Server stop/start toggles the button.
  await page.evaluate("document.getElementById('serverBtn').click()");
  await waitFor(page, "document.getElementById('serverBtn').textContent === 'Start RTSP server'", 'server stopped');
  check(await text(page, 'status') === 'RTSP server stopped', 'pill shows the stopped server', await text(page, 'status'));
  await page.evaluate("document.getElementById('serverBtn').click()");
  await waitFor(page, "document.getElementById('serverBtn').textContent === 'Stop RTSP server'", 'server started');

  // Logs load when opened and contain the actions above; only new lines are fetched later.
  await page.evaluate("document.getElementById('logBox').open = true");
  await waitFor(page, "document.getElementById('log').textContent.includes('Web UI settings: gain=4.5')", 'log lines');
  const logText = await text(page, 'log');
  check(logText.includes('Web UI: RTSP server stopped'), 'log has the server action', logText.slice(-200));
  const lines = logText.trim().split('\n');
  check(new Set(lines).size === lines.length, 'no duplicated log lines', lines.length);

  // Dark scheme and a phone-sized screen.
  await page.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'dark' }] });
  await screenshot(page, 'desktop-dark');
  await page.send('Emulation.setEmulatedMedia', { features: [] });
  await load(page, base, 390, 844, true);
  const overflow = await page.evaluate('document.documentElement.scrollWidth - document.documentElement.clientWidth');
  check(overflow <= 0, 'no horizontal scrolling at 390 px', overflow);
  await screenshot(page, 'mobile');

  // Reboot shows the overlay and the page reloads when the device answers again.
  await page.evaluate("document.querySelector('[data-action=reboot]').click()");
  await waitFor(page, "getComputedStyle(document.getElementById('overlay')).display === 'flex'", 'reboot overlay');
  await page.close();
}

async function latchedScenario(base) {
  const page = await openPage();
  await load(page, base);
  check(await text(page, 'status') === 'Stopped: thermal protection', 'pill shows the latch', await text(page, 'status'));
  check(await page.evaluate("getComputedStyle(document.getElementById('thermalAlert')).display") === 'block', 'latch alert visible');
  check((await text(page, 'thermalAlertText')).includes('81.5 °C'), 'latch note shown', await text(page, 'thermalAlertText'));
  check(await page.evaluate("!document.getElementById('micFlat').hidden"), 'microphone warning shown');
  await screenshot(page, 'latched');
  await page.evaluate("document.querySelector('#thermalAlert button').click()");
  await waitFor(page, "getComputedStyle(document.getElementById('thermalAlert')).display === 'none'", 'alert cleared');
  check(await text(page, 'status') === 'Waiting for a client', 'server back after Resume', await text(page, 'status'));

  // Change WiFi network: the device leaves the network, so the page explains instead of waiting for it.
  await page.evaluate("document.querySelector('[data-action=forget_wifi]').click()");
  await waitFor(page, "getComputedStyle(document.getElementById('overlay')).display === 'flex'", 'portal overlay');
  check((await text(page, 'overlayMsg')).includes('ESP32-RTSP-Mic-AP'), 'overlay names the setup network', await text(page, 'overlayMsg'));
  await page.close();
}

try {
  await fetch(CDP + '/json/version');
} catch (e) {
  console.log('No Chrome DevTools endpoint at ' + CDP + ' (see the comment at the top of this file).');
  process.exit(2);
}
const mocks = [await startMock(18080, 'streaming'), await startMock(18081, 'latched')];
try {
  await streamingScenario('http://127.0.0.1:18080/');
  await latchedScenario('http://127.0.0.1:18081/');
} catch (e) {
  failures++;
  console.log('FAIL: ' + e.message);
} finally {
  for (const m of mocks) m.kill();
}
console.log(`ui_test: ${checks} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
