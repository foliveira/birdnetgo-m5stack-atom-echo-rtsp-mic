// Checks that web/index.html, src/WebUI.cpp and mock_server.mjs agree on the API's names: every
// state key the page reads is one the firmware sends, every settings field is a key the firmware
// accepts, and the mock sends exactly what the firmware sends. Run: node test/webui/check_keys.mjs
import { readFileSync } from 'node:fs';
import { spawn } from 'node:child_process';

const root = new URL('../../', import.meta.url);
const read = (p) => readFileSync(new URL(p, root), 'utf8');
const cpp = read('src/WebUI.cpp');
const html = read('web/index.html');
let failures = 0;
const fail = (msg) => { failures++; console.log('FAIL: ' + msg); };
const diff = (a, b) => [...a].filter((x) => !b.has(x));

// Firmware: keys written by sendState(), split into its "cfg" and "live" objects.
const body = cpp.slice(cpp.indexOf('static void sendState()'), cpp.indexOf('static bool fromThisPage()'));
const section = (name) => {
  const start = body.indexOf(`j.open("${name}")`);
  return body.slice(start, body.indexOf('j.close();', start));
};
const keysIn = (src) => new Set([...src.matchAll(/\.(?:u|i|b|s|f|null)\("([a-z_]+)"/g)].map((m) => m[1]));
const fwCfg = keysIn(section('cfg'));
const fwLive = keysIn(section('live'));
const fwSettable = new Set([...cpp.matchAll(/key == "([a-z_]+)"/g)].map((m) => m[1]));

// Page: fields of the settings forms, and the state keys its script reads.
const formFields = new Set([...html.matchAll(/<(?:input|select)[^>]*\bname="([a-z_]+)"/g)].map((m) => m[1]));
const pageLive = new Set([...html.matchAll(/\bL\.([a-z_]+)/g)].map((m) => m[1]));
const pageCfg = new Set([...html.matchAll(/\bC\.([a-z_]+)/g)].map((m) => m[1]));

for (const k of diff(pageLive, fwLive)) fail(`page reads live.${k}, which the firmware does not send`);
for (const k of diff(pageCfg, fwCfg)) fail(`page reads cfg.${k}, which the firmware does not send`);
for (const k of diff(formFields, fwSettable)) fail(`form field "${k}" is not a setting the firmware accepts`);
for (const k of diff(formFields, fwCfg)) fail(`form field "${k}" has no value in the firmware's cfg`);
for (const k of diff(fwSettable, fwCfg)) fail(`setting "${k}" is accepted but not reported in cfg`);

// Mock: start it and compare its /api/state with the firmware's key sets.
const port = 18090;
const mock = spawn(process.execPath, [new URL('test/webui/mock_server.mjs', root).pathname, String(port)], { stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise((resolve) => mock.stdout.once('data', resolve));
try {
  const st = await (await fetch(`http://127.0.0.1:${port}/api/state`)).json();
  const mockCfg = new Set(Object.keys(st.cfg)), mockLive = new Set(Object.keys(st.live));
  for (const k of diff(fwCfg, mockCfg)) fail(`mock cfg lacks "${k}"`);
  for (const k of diff(mockCfg, fwCfg)) fail(`mock cfg has "${k}", the firmware does not`);
  for (const k of diff(fwLive, mockLive)) fail(`mock live lacks "${k}"`);
  for (const k of diff(mockLive, fwLive)) fail(`mock live has "${k}", the firmware does not`);
} finally {
  mock.kill();
}
console.log(`check_keys: ${fwCfg.size} cfg keys, ${fwLive.size} live keys, ${failures} problems`);
process.exit(failures ? 1 : 0);
