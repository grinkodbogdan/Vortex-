/*
 * Vortex KaiOS compatibility shim.
 *
 * Loaded before any app script (vortex-kai-install injects it into every
 * page).  It makes a KaiOS 2.5 app feel at home in WPE WebKit:
 *
 *   - maps the phone keys to KaiOS key names (SoftLeft, SoftRight,
 *     Backspace, Call, ...) with the keyCodes KaiOS uses
 *   - closes the app on an unhandled Backspace, like KaiOS does
 *   - provides the Mozilla/B2G APIs apps probe for (navigator.mozApps,
 *     mozSettings, mozSetMessageHandler, MozActivity, ...) as small, honest
 *     stubs: they succeed where we can emulate the behaviour and fail with
 *     a DOMError-like object where we cannot
 *   - backs the hardware ones with the phone through vortex-webview's
 *     native bridge: vibrate, getBattery, volumeManager, mozPower
 *     (brightness, power off, reboot), getDeviceStorage (files)
 *   - stubs KaiAds so ad-supported apps do not hang waiting for an ad
 *
 * Cross-origin "systemXHR" works because the runtime disables web security
 * for these local, user-installed apps; new XMLHttpRequest({mozSystem:true})
 * needs no special handling.
 */
(function () {
  'use strict';
  if (window.__vortexShim) return;
  window.__vortexShim = true;

  var nav = navigator;
  function define(obj, name, value) {
    if (obj[name] !== undefined) return;
    try {
      Object.defineProperty(obj, name, { value: value, configurable: true, writable: true });
    } catch (e) { /* read-only host object: leave it */ }
  }

  /* ---- DOMRequest ------------------------------------------------------ */

  function DOMRequest() {
    this.readyState = 'pending';
    this.result = undefined;
    this.error = null;
    this.onsuccess = null;
    this.onerror = null;
    this._listeners = { success: [], error: [] };
  }
  DOMRequest.prototype.addEventListener = function (type, fn) {
    if (this._listeners[type]) this._listeners[type].push(fn);
  };
  DOMRequest.prototype.removeEventListener = function (type, fn) {
    var l = this._listeners[type];
    if (l && l.indexOf(fn) >= 0) l.splice(l.indexOf(fn), 1);
  };
  DOMRequest.prototype._fire = function (type) {
    var ev = { type: type, target: this, currentTarget: this };
    var h = this['on' + type];
    if (typeof h === 'function') h.call(this, ev);
    this._listeners[type].slice().forEach(function (fn) { fn.call(this, ev); }, this);
  };
  DOMRequest.prototype.then = function (ok, fail) {
    var self = this;
    return new Promise(function (resolve, reject) {
      if (self.readyState === 'done') {
        return self.error ? reject(self.error) : resolve(self.result);
      }
      self.addEventListener('success', function () { resolve(self.result); });
      self.addEventListener('error', function () { reject(self.error); });
    }).then(ok, fail);
  };
  function succeed(value) {
    var r = new DOMRequest();
    setTimeout(function () { r.readyState = 'done'; r.result = value; r._fire('success'); }, 0);
    return r;
  }
  function fail(name) {
    var r = new DOMRequest();
    setTimeout(function () {
      r.readyState = 'done';
      r.error = { name: name, message: name };
      r._fire('error');
    }, 0);
    return r;
  }
  define(window, 'DOMRequest', DOMRequest);

  /* ---- keys ------------------------------------------------------------ */

  // DOM key produced by WPE for the evdev code -> KaiOS key name.
  var KEYMAP = {
    F1: 'SoftLeft', ContextMenu: 'SoftLeft',
    F2: 'SoftRight', Props: 'SoftRight',
    Escape: 'Backspace', BrowserBack: 'Backspace', GoBack: 'Backspace',
    F3: 'Call', MailSend: 'Call', Call: 'Call',
    Select: 'Enter', Accept: 'Enter',
    AudioVolumeUp: 'VolumeUp', AudioVolumeDown: 'VolumeDown'
  };
  var KEYCODES = {
    Backspace: 8, Enter: 13, ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40,
    SoftLeft: 0, SoftRight: 0, Call: 0, VolumeUp: 0, VolumeDown: 0, '*': 170, '#': 163
  };

  function isEditable(el) {
    if (!el) return false;
    var tag = (el.tagName || '').toLowerCase();
    return el.isContentEditable || tag === 'textarea' ||
      (tag === 'input' && !/^(button|checkbox|radio|submit|reset|range|color|file)$/i.test(el.type || ''));
  }

  function remap(e) {
    if (e.__vortex) return;
    var mapped = KEYMAP[e.key];
    // The keypad's # arrives as keypad "/" (see the image's udev hwdb).
    if (e.code === 'NumpadDivide') mapped = '#';
    var target = document.activeElement || document.body || document.documentElement;
    if (mapped) {
      e.stopImmediatePropagation();
      e.preventDefault();
      var ev = new KeyboardEvent(e.type, {
        key: mapped, code: e.code, bubbles: true, cancelable: true, repeat: e.repeat
      });
      var kc = KEYCODES[mapped] !== undefined ? KEYCODES[mapped] : e.keyCode;
      Object.defineProperty(ev, 'keyCode', { get: function () { return kc; } });
      Object.defineProperty(ev, 'which', { get: function () { return kc; } });
      ev.__vortex = true;
      var handled = !target.dispatchEvent(ev);
      if (mapped === 'Backspace' && e.type === 'keydown' && !handled && !isEditable(target)) {
        window.close();
      }
      return;
    }
    if (e.key === 'Backspace' && e.type === 'keydown' && !isEditable(target)) {
      // Native Backspace: close only if the app did not handle it.
      setTimeout(function () { if (!e.defaultPrevented) window.close(); }, 0);
    }
  }
  window.addEventListener('keydown', remap, true);
  window.addEventListener('keyup', remap, true);

  /* ---- navigator.mozApps ----------------------------------------------- */

  var manifest = null;
  try {
    var x = new XMLHttpRequest();
    x.open('GET', '/manifest.webapp', false); // tiny local file
    x.send();
    if (x.status === 0 || x.status === 200) manifest = JSON.parse(x.responseText);
  } catch (e) { manifest = {}; }
  manifest = manifest || {};

  var appSelf = {
    manifest: manifest,
    manifestURL: location.origin + '/manifest.webapp',
    origin: location.origin,
    installOrigin: location.origin,
    installTime: Date.now(),
    receipts: null,
    launch: function () {},
    connect: function () { return Promise.reject(new Error('IAC is not supported')); },
    getConnections: function () { return Promise.resolve([]); },
    addEventListener: function () {},
    removeEventListener: function () {}
  };
  define(nav, 'mozApps', {
    getSelf: function () { return succeed(appSelf); },
    getInstalled: function () { return succeed([appSelf]); },
    checkInstalled: function () { return succeed(appSelf); },
    install: function () { return fail('DENIED'); },
    installPackage: function () { return fail('DENIED'); },
    mgmt: {
      getAll: function () { return succeed([appSelf]); },
      addEventListener: function () {},
      removeEventListener: function () {}
    }
  });

  /* ---- system messages, activities ------------------------------------- */

  define(nav, 'mozSetMessageHandler', function () {});
  define(nav, 'mozHasPendingMessage', function () { return false; });
  define(window, 'MozActivity', function MozActivity(opts) {
    var r = fail('NO_PROVIDER');
    r.name = opts && opts.name;
    return r;
  });

  /* ---- settings (persisted per app) ------------------------------------- */

  var SETTINGS_KEY = '__vortex_settings';
  var defaults = {
    'language.current': nav.language || 'en-US',
    'audio.volume.content': 10,
    'screen.brightness': 0.6,
    'deviceinfo.os': '2.5.4',
    'deviceinfo.software': 'Vortex',
    'deviceinfo.product_model': 'Nokia 6300 4G',
    'time.timezone': (Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC')
  };
  function loadSettings() {
    try { return Object.assign({}, defaults, JSON.parse(localStorage.getItem(SETTINGS_KEY) || '{}')); }
    catch (e) { return Object.assign({}, defaults); }
  }
  var observers = {};
  define(nav, 'mozSettings', {
    createLock: function () {
      return {
        get: function (key) {
          var s = loadSettings(), out = {};
          if (key === '*') out = s; else out[key] = s[key];
          return succeed(out);
        },
        set: function (obj) {
          var s = loadSettings();
          Object.keys(obj).forEach(function (k) {
            s[k] = obj[k];
            (observers[k] || []).forEach(function (fn) {
              setTimeout(function () { fn({ settingName: k, settingValue: obj[k] }); }, 0);
            });
          });
          try { localStorage.setItem(SETTINGS_KEY, JSON.stringify(s)); } catch (e) {}
          return succeed(true);
        }
      };
    },
    addObserver: function (k, fn) { (observers[k] = observers[k] || []).push(fn); },
    removeObserver: function (k, fn) {
      var l = observers[k] || [];
      if (l.indexOf(fn) >= 0) l.splice(l.indexOf(fn), 1);
    }
  });

  /* ---- native bridge (vortex-webview) ---------------------------------- */

  // Hardware goes through the runtime: window.webkit.messageHandlers.vortex
  // takes {id, cmd, args}; the answer comes back as
  // __vortexBridgeReply(id, error, result).  Without it (another browser),
  // calls fail with NotSupportedError.
  var bridge = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.vortex;
  var pending = {}, nextId = 1;
  window.__vortexBridgeReply = function (id, error, result) {
    var p = pending[id];
    if (!p) return;
    delete pending[id];
    if (error) p.reject(error); else p.resolve(result);
  };
  function call(cmd, args) {
    return new Promise(function (resolve, reject) {
      if (!bridge) return reject('NotSupportedError');
      var id = nextId++;
      pending[id] = { resolve: resolve, reject: reject };
      bridge.postMessage(JSON.stringify({ id: id, cmd: cmd, args: args || {} }));
    });
  }
  // Where the bridge exists it is what reaches the phone's hardware, so it
  // replaces an engine's own version of an API (which would do nothing).
  function provide(obj, name, value) {
    if (!bridge) return define(obj, name, value);
    try {
      Object.defineProperty(obj, name, { value: value, configurable: true, writable: true });
    } catch (e) { /* read-only host object: leave it */ }
  }
  function request(promise) {
    var r = new DOMRequest();
    promise.then(function (v) {
      r.readyState = 'done'; r.result = v; r._fire('success');
    }, function (e) {
      r.readyState = 'done'; r.error = { name: String(e), message: String(e) }; r._fire('error');
    });
    return r;
  }

  /* ---- vibration, battery, volume, power -------------------------------- */

  provide(nav, 'vibrate', function (pattern) {
    var p = Array.isArray(pattern) ? pattern : [pattern];
    call('vibrate', { pattern: p.map(function (x) { return Number(x) || 0; }) }).catch(function () {});
    return true;
  });

  var battery = null;
  function getBattery() {
    if (battery) return Promise.resolve(battery);
    battery = { level: 1, charging: true, chargingTime: 0, dischargingTime: Infinity,
                onlevelchange: null, onchargingchange: null, _l: {} };
    battery.addEventListener = function (t, fn) { (battery._l[t] = battery._l[t] || []).push(fn); };
    battery.removeEventListener = function (t, fn) {
      var l = battery._l[t] || [];
      if (l.indexOf(fn) >= 0) l.splice(l.indexOf(fn), 1);
    };
    function fire(t) {
      var ev = { type: t, target: battery };
      if (typeof battery['on' + t] === 'function') battery['on' + t](ev);
      (battery._l[t] || []).forEach(function (fn) { fn(ev); });
    }
    function update() {
      return call('battery').then(function (b) {
        var lv = b.level !== battery.level, ch = b.charging !== battery.charging;
        battery.level = b.level;
        battery.charging = b.charging;
        battery.dischargingTime = b.charging ? Infinity : 0;
        if (lv) fire('levelchange');
        if (ch) fire('chargingchange');
      }).catch(function () {});
    }
    setInterval(update, 60000);
    return update().then(function () { return battery; });
  }
  provide(nav, 'getBattery', getBattery);
  getBattery().then(function (b) { define(nav, 'battery', b); });

  // KaiOS shows a volume bar when an app changes the volume.
  var volBar = null, volTimer = null;
  function showVolume(level) {
    if (!document.body) return;
    if (!volBar) {
      volBar = document.createElement('div');
      volBar.style.cssText = 'position:fixed;left:16px;right:16px;bottom:36px;height:22px;' +
        'background:rgba(8,16,28,.92);border:1px solid #39f;border-radius:6px;z-index:2147483647;' +
        'font:12px sans-serif;color:#fff;padding:3px 8px;box-sizing:border-box;pointer-events:none';
      volBar.innerHTML = '<span>Volume</span><div style="position:absolute;left:62px;right:10px;top:8px;' +
        'height:5px;background:#234"><div style="height:100%;background:#39f;width:0"></div></div>';
    }
    if (!volBar.parentNode) document.body.appendChild(volBar);
    volBar.lastChild.firstChild.style.width = Math.round(level * 100 / 15) + '%';
    clearTimeout(volTimer);
    volTimer = setTimeout(function () { if (volBar.parentNode) volBar.parentNode.removeChild(volBar); }, 1500);
  }
  function volume(op) {
    return call('volume', { op: op }).then(function (v) { showVolume(v); return v; });
  }
  define(nav, 'volumeManager', {
    requestUp: function () { volume('up').catch(function () {}); },
    requestDown: function () { volume('down').catch(function () {}); },
    requestShow: function () { volume('get').catch(function () {}); }
  });

  var brightness = 0.6;
  call('brightness').then(function (b) { brightness = b; }).catch(function () {});
  define(nav, 'mozPower', {
    screenEnabled: true, cpuSleepAllowed: true,
    get screenBrightness() { return brightness; },
    set screenBrightness(v) {
      brightness = Math.max(0.05, Math.min(1, Number(v) || 0));
      call('brightness', { value: brightness }).catch(function () {});
    },
    powerOff: function () { call('power', { op: 'poweroff' }).catch(function () {}); },
    reboot: function () { call('power', { op: 'reboot' }).catch(function () {}); }
  });
  define(nav, 'requestWakeLock', function (topic) { return { topic: topic, unlock: function () {} }; });

  /* ---- device storage: /var/lib/vortex/storage -------------------------- */

  function storageFile(meta) {
    return fetch('vortex-storage://local/' + meta.name.split('/').map(encodeURIComponent).join('/'))
      .then(function (r) {
        if (!r.ok) throw 'NotFoundError';
        return r.blob();
      }).then(function (b) {
        return new File([b], meta.name, { type: meta.type || b.type, lastModified: meta.mtime || Date.now() });
      });
  }
  function blobToBase64(blob) {
    return new Promise(function (resolve, reject) {
      var fr = new FileReader();
      fr.onload = function () { resolve(String(fr.result).replace(/^data:[^,]*,/, '')); };
      fr.onerror = function () { reject('UnknownError'); };
      fr.readAsDataURL(blob);
    });
  }
  function DeviceStorage(name) {
    this.storageName = name;
    this.default = true;
    this.canBeMounted = false;
    this.canBeShared = false;
    this.isRemovable = false;
  }
  DeviceStorage.prototype.get = function (name) {
    var self = this;
    return request(call('storage', { op: 'list', storage: self.storageName }).then(function (list) {
      var meta = list.filter(function (f) { return f.name === String(name).replace(/^\//, ''); })[0];
      if (!meta) throw 'NotFoundError';
      return storageFile(meta);
    }));
  };
  DeviceStorage.prototype.getEditable = DeviceStorage.prototype.get;
  DeviceStorage.prototype.addNamed = function (blob, name) {
    return request(blobToBase64(blob).then(function (data) {
      return call('storage', { op: 'put', name: String(name).replace(/^\//, ''), data: data });
    }));
  };
  DeviceStorage.prototype.add = function (blob) {
    var ext = ((blob && blob.type) || '').split('/')[1] || 'bin';
    return this.addNamed(blob, 'vortex-' + Date.now() + '.' + ext.replace(/[^a-z0-9]/gi, ''));
  };
  DeviceStorage.prototype.delete = function (name) {
    return request(call('storage', { op: 'delete', name: String(name).replace(/^\//, '') }));
  };
  DeviceStorage.prototype.freeSpace = function () {
    return request(call('storage', { op: 'space' }).then(function (s) { return s.free; }));
  };
  DeviceStorage.prototype.usedSpace = function () {
    return request(call('storage', { op: 'space' }).then(function (s) { return s.used; }));
  };
  DeviceStorage.prototype.available = function () {
    return request(call('storage', { op: 'space' }).then(function () { return 'available'; },
      function () { return 'unavailable'; }));
  };
  // enumerate([path], [options]): a cursor; onsuccess runs once per file
  // (this.result), continue() moves on, and the end has result null, done.
  DeviceStorage.prototype.enumerate = function (path) {
    var cursor = new DOMRequest(), list = null, i = 0;
    var prefix = typeof path === 'string' ? path.replace(/^\//, '') : '';
    cursor.done = false;
    function next() {
      if (i >= list.length) {
        cursor.readyState = 'done'; cursor.result = null; cursor.done = true; cursor._fire('success');
        return;
      }
      storageFile(list[i++]).then(function (f) {
        cursor.readyState = 'done'; cursor.result = f; cursor._fire('success');
      }, next);
    }
    cursor.continue = function () { setTimeout(next, 0); };
    call('storage', { op: 'list', storage: this.storageName }).then(function (l) {
      list = l.filter(function (f) { return !prefix || f.name.indexOf(prefix) === 0; });
      next();
    }, function (e) {
      cursor.readyState = 'done'; cursor.error = { name: String(e), message: String(e) }; cursor._fire('error');
    });
    return cursor;
  };
  DeviceStorage.prototype.enumerateEditable = DeviceStorage.prototype.enumerate;
  DeviceStorage.prototype.addEventListener = function () {};
  DeviceStorage.prototype.removeEventListener = function () {};
  define(nav, 'getDeviceStorage', function (name) { return new DeviceStorage(name || 'sdcard'); });
  define(nav, 'getDeviceStorages', function (name) { return [new DeviceStorage(name || 'sdcard')]; });

  /* ---- telephony and other stubs --------------------------------------- */

  define(nav, 'mozMobileConnections', []);
  define(nav, 'mozIccManager', { iccIds: [], getIccById: function () { return null; } });
  define(nav, 'mozTelephony', null);
  define(nav, 'mozAlarms', {
    add: function () { return fail('NotSupportedError'); },
    getAll: function () { return succeed([]); },
    remove: function () {}
  });
  define(nav, 'mozContacts', {
    find: function () { return succeed([]); },
    getAll: function () { var r = succeed(null); r.continue = function () {}; return r; },
    getCount: function () { return succeed(0); }
  });
  define(nav, 'mozAudioChannelManager', { volumeControlChannel: 'content' });
  define(nav, 'mozL10n', {
    language: { code: nav.language || 'en-US', direction: 'ltr' },
    get: function (id) { return id; },
    once: function (cb) { setTimeout(cb, 0); },
    ready: function (cb) { setTimeout(cb, 0); },
    translate: function () {},
    setAttributes: function (el, id) { if (el) el.setAttribute('data-l10n-id', id); }
  });
  // KaiOS lets apps toggle a cursor-free spatial navigation mode.
  try {
    Object.defineProperty(nav, 'spatialNavigationEnabled', {
      configurable: true, get: function () { return false; }, set: function () {}
    });
  } catch (e) {}

  /* ---- KaiAds ---------------------------------------------------------- */

  function noAds(cfg) {
    if (cfg && typeof cfg.onerror === 'function') setTimeout(function () { cfg.onerror('No ads on Vortex'); }, 0);
  }
  define(window, 'getKaiAd', noAds);
  define(window, 'KaiAds', { getKaiAd: noAds });
})();
