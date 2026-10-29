// Checks the KaiOS shim in a real browser engine.
//
// Serves an installed app from the origin root, the way vortex-kai-run serves
// it from app://ID/ in WPE, then verifies key mapping, Backspace-to-close and
// the mozApps / mozSettings / KaiAds stubs.  Run it against the test fixture
// (runtime/tests/run.sh does this), not against untrusted store apps.
//
//   node runtime/tests/shim-test.js APPDIR [screenshot.png]
const { chromium } = require('playwright');
const http = require('http');
const fs = require('fs');
const path = require('path');

const appDir = path.resolve(process.argv[2]);
const shot = process.argv[3];
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css',
  '.png': 'image/png', '.json': 'application/json', '.webapp': 'application/json' };

const server = http.createServer((req, res) => {
  const p = path.join(appDir, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!p.startsWith(appDir) || !fs.existsSync(p) || fs.statSync(p).isDirectory()) {
    res.writeHead(404);
    return res.end();
  }
  res.writeHead(200, { 'content-type': types[path.extname(p)] || 'application/octet-stream' });
  fs.createReadStream(p).pipe(res);
});

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;
  const manifest = JSON.parse(fs.readFileSync(path.join(appDir, 'manifest.webapp')));
  const launch = manifest.launch_path || '/index.html';
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 240, height: 320 } });
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.addInitScript(() => {
    window.__keys = [];
    window.__closed = 0;
    window.close = () => { window.__closed++; };
    window.addEventListener('DOMContentLoaded', () => {
      document.addEventListener('keydown', e => window.__keys.push(e.key + ':' + e.keyCode));
    });
  });
  await page.goto(`http://127.0.0.1:${port}${launch}`, { waitUntil: 'load' });

  const r = await page.evaluate(async () => {
    const self = await navigator.mozApps.getSelf();
    const lock = navigator.mozSettings.createLock();
    await lock.set({ 'vortex.test': 42 });
    const got = await lock.get('vortex.test');
    let ad = null;
    getKaiAd({ onerror: e => { ad = e; } });
    await new Promise(res => setTimeout(res, 10));
    return { shim: window.__vortexShim === true, name: self.manifest.name,
             setting: got['vortex.test'], ad, activity: typeof MozActivity };
  });

  for (const k of ['F1', 'F2', 'ArrowDown', 'Escape'])
    await page.keyboard.press(k);
  await page.waitForTimeout(50);
  const keys = await page.evaluate(() => window.__keys);
  const closed = await page.evaluate(() => window.__closed);
  if (shot)
    await page.screenshot({ path: shot });
  await browser.close();
  server.close();

  const checks = [
    ['shim loaded', r.shim],
    ['mozApps.getSelf returns the manifest', r.name === manifest.name],
    ['mozSettings round trip', r.setting === 42],
    ['KaiAds reports "no ad"', typeof r.ad === 'string'],
    ['MozActivity exists', r.activity === 'function'],
    ['F1 -> SoftLeft', keys.includes('SoftLeft:0')],
    ['F2 -> SoftRight', keys.includes('SoftRight:0')],
    ['ArrowDown passes through', keys.includes('ArrowDown:40')],
    ['Escape -> Backspace (keyCode 8)', keys.includes('Backspace:8')],
    ['unhandled Backspace closes the app', closed === 1],
    ['no page errors', errors.length === 0],
  ];
  let ok = true;
  for (const [name, pass] of checks) {
    console.log((pass ? 'ok   ' : 'FAIL ') + name);
    ok = ok && pass;
  }
  if (errors.length)
    console.log('page errors:\n  ' + errors.join('\n  '));
  process.exit(ok ? 0 : 1);
})().catch(e => {
  console.error(e);
  process.exit(1);
});
