/* Execute the production portal's real JavaScript with an in-memory DOM/fetch.
 * No browser, network, Wi-Fi adapter, serial port, or server is used.
 * Run: node components/78__esp-wifi-connect/tests/test_provisioning_portal.js
 */
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const portalPath = path.join(__dirname, '..', 'assets', 'wifi_configuration.html');
const html = fs.readFileSync(portalPath, 'utf8');
const scripts = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/gi)]
  .map(match => match[1]).join('\n');

function deferred() {
  let resolve, reject;
  const promise = new Promise((accept, fail) => { resolve = accept; reject = fail; });
  return {promise, resolve, reject};
}

function response(data, ok = true, status = ok ? 200 : 500) {
  return {ok, status, json: async () => data};
}

function harness() {
  const nodes = new Map();
  for (const match of html.matchAll(/<([a-z][\w-]*)\b([^>]*\bid="([^"]+)"[^>]*)>/gi)) {
    const classes = new Set();
    const node = {
      id: match[3], value: '', checked: false,
      disabled: /\bdisabled(?:\s|=|$)/.test(match[2]),
      textContent: '', innerHTML: '', style: {},
      classList: {add: name => classes.add(name), remove: name => classes.delete(name),
        contains: name => classes.has(name)},
      addEventListener() {}, removeAttribute() {}, focus() {},
      querySelector() { return null; }, appendChild() {},
    };
    nodes.set(node.id, node);
  }
  const get = id => {
    assert(nodes.has(id), `Portal used missing element ${id}`);
    return nodes.get(id);
  };
  get('language').value = 'en-US';
  get('ssid').value = 'Robot home';
  get('password').value = 'robot-home-password';
  get('ota_url').value = 'http://ota.example.com/ota/';
  get('max_tx_power').value = '80';
  get('remember_bssid').checked = true;
  get('sleep_mode').checked = false;
  const calls = [];
  const timers = new Map();
  let nextTimer = 0;
  let route = async () => response({success: true, saved: true, restarting: true});
  const context = vm.createContext({
    document: {getElementById: get, addEventListener() {}, querySelectorAll: () => [],
      querySelector: () => null},
    window: {location: {href: '', search: ''}, addEventListener() {}},
    navigator: {language: 'en-US'}, localStorage: {getItem: () => null, setItem() {}},
    URLSearchParams, AbortController, console: {error() {}, warn() {}, log() {}},
    setTimeout(fn, ms) { const id = ++nextTimer; timers.set(id, {fn, ms}); return id; },
    clearTimeout(id) { timers.delete(id); },
    fetch: async (url, options = {}) => {
      const call = {url, method: options.method || 'GET',
        body: options.body ? JSON.parse(options.body) : undefined};
      calls.push(call);
      return route(call);
    },
  });
  vm.runInContext(scripts, context, {filename: portalPath});
  // Refreshing the list and displaying a toast are unrelated to the save
  // transaction; keep the assertions focused on requests that persist settings.
  vm.runInContext('loadSavedList = function() {}; showToast = function() {}; loadAPList = function() {};', context);
  return {
    get, calls, context, timers,
    expireRequests() {
      for (const [id, timer] of timers) if (timer.ms === 8000) { timers.delete(id); timer.fn(); }
    },
    route: fn => { route = fn; },
    run: code => vm.runInContext(code, context),
    async ready() {
      route = async () => response({ota_url: get('ota_url').value,
        max_tx_power: 80, remember_bssid: true, sleep_mode: false});
      await vm.runInContext('loadAdvancedConfig()', context);
      calls.length = 0;
      route = async () => response({success: true, saved: true, restarting: true});
    },
  };
}

let cases = 0;
async function test(name, fn) {
  await fn();
  ++cases;
  process.stdout.write(`ok ${cases} - ${name}\n`);
}

async function main() {
  await test('OTA is a single field in the Wi-Fi form', async () => {
    assert.equal([...html.matchAll(/\bid="ota_url"/g)].length, 1);
    const wifiForm = html.match(/<form\b[^>]*action="\/submit"[^>]*>([\s\S]*?)<\/form>/);
    assert(wifiForm && /\bid="ota_url"/.test(wifiForm[1]));
    assert(/\bid="ssid"/.test(wifiForm[1]));
  });

  await test('SSID accepts manual entry without waiting for a scan or touch event', async () => {
    const ssidInput = html.match(/<input\b[^>]*\bid="ssid"[^>]*>/);
    assert(ssidInput);
    assert(!/\breadonly(?:\s|=|>)/.test(ssidInput[0]));
    assert(!/\bdisabled(?:\s|=|>)/.test(ssidInput[0]));
  });

  await test('configuration load prevents an early save from overwriting OTA', async () => {
    const h = harness();
    const loaded = deferred();
    h.route(() => loaded.promise);
    const loading = h.run('loadAdvancedConfig()');
    assert.equal(h.get('button').disabled, true);
    assert.equal(h.get('save_wifi_button').disabled, true);
    assert.equal(h.get('advanced_button').disabled, true);
    loaded.resolve(response({ota_url: 'https://new.example/ota', max_tx_power: 44,
      remember_bssid: false, sleep_mode: true}));
    await loading;
    assert.equal(h.get('ota_url').value, 'https://new.example/ota');
    assert.equal(h.get('button').disabled, false);
    assert.equal(h.get('save_wifi_button').disabled, false);
    assert.equal(h.get('advanced_button').disabled, false);
    assert.equal(h.get('remember_bssid').checked, false);
    assert.equal(h.get('sleep_mode').checked, true);
  });

  for (const [failure, send] of [
    ['HTTP error', () => response({ota_url: ''}, false)],
    ['network rejection', () => Promise.reject(new Error('load unreachable'))],
    ['invalid JSON', () => ({ok: true, json: async () => { throw new Error('invalid JSON'); }})],
  ]) {
    await test(`configuration ${failure} keeps all save controls disabled`, async () => {
      const h = harness();
      h.route(send);
      await h.run('loadAdvancedConfig()');
      assert.equal(h.get('button').disabled, true);
      assert.equal(h.get('save_wifi_button').disabled, true);
      assert.equal(h.get('advanced_button').disabled, true);
      assert.match(h.get('error').textContent, /reload/i);
      assert.deepEqual(h.calls.map(call => call.url), ['/advanced/config']);
      assert.equal(h.context.window.location.href, '');
    });
  }

  for (const [functionCall, endpoint] of [
    ['saveWifiOnly()', '/saved/add'],
    ['submitForm({preventDefault(){}})', '/submit'],
  ]) {
    await test(`${endpoint} waits for successful OTA persistence`, async () => {
      const h = harness();
      await h.ready();
      const stored = deferred();
      h.route(call => call.url === '/advanced/submit' ? stored.promise : response({success: true, saved: true, restarting: true}));
      const pending = h.run(functionCall);
      assert.deepEqual(h.calls.map(call => call.url), ['/advanced/submit']);
      assert.deepEqual(h.calls[0].body, {ota_url: 'http://ota.example.com/ota/'});
      assert.equal(h.context.window.location.href, '');
      stored.resolve(response({success: true}));
      await pending;
      assert.deepEqual(h.calls.map(call => call.url), ['/advanced/submit', endpoint]);
      assert.deepEqual(h.calls[1].body, {ssid: 'Robot home', password: 'robot-home-password'});
      assert.equal(h.context.window.location.href, '');
      assert.equal(h.timers.size, 0);
      if (endpoint === '/submit') {
        assert.match(h.get('provisioning_status').textContent, /saved.*restarting/i);
        assert.match(h.get('provisioning_status').textContent, /60 seconds/);
        assert.equal(h.get('button').disabled, true);
        assert.equal(h.get('loadingOverlay').classList.contains('show'), false);
      } else {
        assert.equal(h.get('button').disabled, false);
        assert.equal(h.get('provisioning_status').textContent, '');
      }
    });

    for (const [failure, send] of [
      ['JSON failure', () => response({success: false, error: 'NVS commit failed'})],
      ['HTTP error', () => response({success: true}, false)],
      ['network rejection', () => Promise.reject(new Error('save unreachable'))],
      ['invalid response JSON', () => ({ok: true, json: async () => { throw new Error('invalid JSON'); }})],
    ]) {
      await test(`${endpoint} stops after OTA ${failure}`, async () => {
        const h = harness();
        await h.ready();
        h.route(send);
        await h.run(functionCall);
        assert.deepEqual(h.calls.map(call => call.url), ['/advanced/submit']);
        assert.equal(h.context.window.location.href, '');
        assert.notEqual(h.get('error').textContent, '');
        assert.equal(h.get('button').disabled, false);
        assert.equal(h.get('save_wifi_button').disabled, false);
      });
    }
  }

  await test('advanced save reads the shared OTA field', async () => {
    const h = harness();
    await h.ready();
    h.get('ota_url').value = 'https://changed.example/ota';
    await h.run('submitAdvancedForm({preventDefault(){}})');
    assert.deepEqual(h.calls.map(call => call.url), ['/advanced/submit']);
    assert.deepEqual(h.calls[0].body, {ota_url: 'https://changed.example/ota',
      max_tx_power: 80, remember_bssid: true, sleep_mode: false});
    assert.equal(h.context.window.location.href, '');
  });

  await test('clearing the shared OTA field is persisted before Wi-Fi', async () => {
    const h = harness();
    await h.ready();
    h.run('clearOtaUrl()');
    await h.run('saveWifiOnly()');
    assert.equal(h.calls[0].url, '/advanced/submit');
    assert.equal(h.calls[0].body.ota_url, '');
    assert.equal(h.calls[1].url, '/saved/add');
  });

  await test('submit responds to loss of acknowledgement without claiming a Wi-Fi connection', async () => {
    const h = harness(); await h.ready();
    h.route(call => call.url === '/advanced/submit' ? response({success: true}) : Promise.reject(new Error('hotspot disconnected')));
    await h.run('submitForm({preventDefault(){}})');
    assert.match(h.get('error').textContent, /hotspot disconnected/);
    assert.match(h.get('provisioning_status').textContent, /could not be confirmed/);
    assert.match(h.get('provisioning_status').textContent, /60 seconds/);
    assert.equal(h.get('loadingOverlay').classList.contains('show'), false);
    assert.equal(h.get('button').disabled, false);
    assert.equal(h.context.window.location.href, '');
  });

  await test('request timeout bounds loading even when fetch never settles', async () => {
    const h = harness(); await h.ready();
    h.route(() => new Promise(() => {}));
    const submitting = h.run('submitForm({preventDefault(){}})');
    h.expireRequests(); await submitting;
    assert.match(h.get('error').textContent, /timed out/i);
    assert.equal(h.get('loadingOverlay').classList.contains('show'), false);
    assert.equal(h.get('button').disabled, false);
    assert.deepEqual(h.calls.map(call => call.url), ['/advanced/submit']);
    assert.equal(h.timers.size, 0);
  });

  await test('request timeout also bounds reading a stalled response body', async () => {
    const h = harness(); await h.ready();
    h.route(() => ({ok: true, json: () => new Promise(() => {})}));
    const pending = h.run('saveWifiOnly()');
    await Promise.resolve(); h.expireRequests(); await pending;
    assert.match(h.get('error').textContent, /timed out/i);
    assert.equal(h.get('button').disabled, false);
    assert.equal(h.timers.size, 0);
  });

  await test('returning hotspot explains the previous station failure', async () => {
    const h = harness();
    h.route(() => response({ota_url: 'https://retained.example/ota', connection_failure: 'Authentication failed'}));
    await h.run('loadAdvancedConfig()');
    assert.match(h.get('error').textContent, /previous Wi-Fi connection did not complete/);
    assert.match(h.get('error').textContent, /Authentication failed/);
    assert.equal(h.get('ota_url').value, 'https://retained.example/ota');
    assert.equal(h.get('button').disabled, false);
  });

  await test('saved response must explicitly confirm independent restart', async () => {
    const h = harness(); await h.ready();
    h.route(() => response({success: true}));
    await h.run('submitForm({preventDefault(){}})');
    assert.match(h.get('error').textContent, /Failed to save/);
    assert.doesNotMatch(h.get('provisioning_status').textContent, /^Configuration saved/);
    assert.equal(h.get('button').disabled, false);
  });

  await test('configuration load starts the scan after enabling controls', async () => {
    const h = harness();
    h.run('scanStarts = 0; loadAPList = function() { if (!button.disabled) ++scanStarts; };');
    h.route(() => response({ota_url: ''}));
    await h.run('loadAdvancedConfig()');
    assert.equal(h.run('scanStarts'), 1);
  });

  await test('completion page contains no restart request or false connection success', async () => {
    const done = fs.readFileSync(path.join(__dirname, '..', 'assets', 'wifi_configuration_done.html'), 'utf8');
    assert(!done.includes("fetch('/exit'"));
    assert(!done.includes('Configuration successful'));
    assert(done.includes('Configuration saved'));
    assert(done.includes('60 seconds'));
  });

  process.stdout.write(`${cases} production provisioning portal cases passed\n`);
}

main().catch(error => { console.error(error); process.exitCode = 1; });
