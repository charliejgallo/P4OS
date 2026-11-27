// P4OS - a screenshot of a portal page after it ran for a while in real
// time, through Chrome's DevTools protocol (Node 22+, Chrome installed).
//
//     node webshot.mjs http://127.0.0.1:8127/#mensajes web.png 960 640 7000 dark
//
// Chrome's own --headless --screenshot only waits in virtual time
// (--virtual-time-budget): a page that polls the board's live channel
// takes its picture before the simulator ever answered, and shows "the app
// is not open". This waits for real. The last argument is the colour
// scheme (dark or light); a width under 600 is drawn as a phone.
import { spawn } from 'node:child_process';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
const [url, out, w, h, wait, dark] = process.argv.slice(2);
const prof = mkdtempSync(join(tmpdir(), 'ws-'));
const chrome = spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--remote-debugging-port=9333', `--user-data-dir=${prof}`,
   `--window-size=${w},${h}`, 'about:blank'], { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let ver;
for (let i = 0; i < 50; i++) { try { ver = await (await fetch('http://127.0.0.1:9333/json/list')).json(); break; } catch { await sleep(200); } }
const page = ver.find(t => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = {};
ws.onmessage = m => { const d = JSON.parse(m.data); if (d.id && pend[d.id]) { pend[d.id](d.result); delete pend[d.id]; } };
const call = (method, params = {}) => new Promise(r => { const i = ++id; pend[i] = r; ws.send(JSON.stringify({ id: i, method, params })); });
await call('Emulation.setDeviceMetricsOverride', { width: +w, height: +h, deviceScaleFactor: 1, mobile: +w < 600 });
if (dark) await call('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: dark }] });
await call('Page.enable');
await call('Page.navigate', { url });
await sleep(+wait);
const shot = await call('Page.captureScreenshot', { format: 'png' });
writeFileSync(out, Buffer.from(shot.data, 'base64'));
ws.close(); chrome.kill();
console.log('wrote', out);
