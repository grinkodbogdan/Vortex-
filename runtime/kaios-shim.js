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
 *     mozSettings, mozSetMessageHandler, MozActivity, getDeviceStorage, ...)
 *     as small, honest stubs: they succeed where we can emulate the
 *     behaviour and fail with a DOMError-like object where we cannot
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

  /* ---- hardware & telephony stubs -------------------------------------- */

  define(nav, 'getDeviceStorage', function () {
    return {
      storageName: 'sdcard',
      available: function () { return succeed('unavailable'); },
      freeSpace: function () { return succeed(0); },
      usedSpace: function () { return succeed(0); },
      get: function () { return fail('NotFoundError'); },
      add: function () { return fail('SecurityError'); },
      addNamed: function () { return fail('SecurityError'); },
      delete: function () { return fail('SecurityError'); },
      enumerate: function () {
        var r = succeed(null);
        r.continue = function () {};
        return r;
      },
      addEventListener: function () {},
      removeEventListener: function () {}
    };
  });
  define(nav, 'getDeviceStorages', function () { return [nav.getDeviceStorage('sdcard')]; });
  define(nav, 'mozPower', {
    screenEnabled: true, screenBrightness: 0.6, cpuSleepAllowed: true,
    powerOff: function () {}, reboot: function () {}
  });
  define(nav, 'requestWakeLock', function (topic) { return { topic: topic, unlock: function () {} }; });
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
  define(nav, 'volumeManager', {
    requestUp: function () {}, requestDown: function () {}, requestShow: function () {}
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
