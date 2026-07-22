/* DTS Enabler (universal) - frontend controller.
 *
 * Talks to our own JS service (org.webosbrew.dtsenabler.service), which detects
 * the TV generation and shells the matching mechanism out as root through the
 * Homebrew Channel exec service. The frontend never touches root directly and
 * sends NO free-form parameters (the methods take none) -- it only invokes
 * detect/status/enable/disable/uninstall.
 *
 * callService() uses webOS.service.request when the platform bridge is present,
 * and falls back to a raw PalmServiceBridge so the UI is testable in a plain
 * browser too.
 */
(function () {
  "use strict";

  var SERVICE = "luna://org.webosbrew.dtsenabler.service";

  /* ---------------------------------------------------------------------- */
  /* Service bridge                                                          */
  /* ---------------------------------------------------------------------- */

  function callService(method, params) {
    params = params || {};
    return new Promise(function (resolve, reject) {
      if (window.webOS && window.webOS.service && window.webOS.service.request) {
        window.webOS.service.request(SERVICE, {
          method: method,
          parameters: params,
          onSuccess: function (res) {
            if (res && res.returnValue === false) { reject(res); } else { resolve(res); }
          },
          onFailure: function (err) { reject(err); }
        });
        return;
      }
      if (window.PalmServiceBridge) {
        var bridge = new window.PalmServiceBridge();
        bridge.onservicecallback = function (raw) {
          var res;
          try { res = JSON.parse(raw); } catch (e) { reject({ errorText: "bad JSON: " + raw }); return; }
          if (res && res.returnValue === false) { reject(res); } else { resolve(res); }
        };
        bridge.call(SERVICE + "/" + method, JSON.stringify(params));
        return;
      }
      reject({ errorText: "No webOS service bridge available (not running on a TV?)" });
    });
  }

  /* ---------------------------------------------------------------------- */
  /* DOM helpers                                                             */
  /* ---------------------------------------------------------------------- */

  function $(id) { return document.getElementById(id); }

  var toastTimer = null;
  function toast(msg, kind) {
    var t = $("toast");
    t.textContent = msg;
    t.className = "toast" + (kind ? " toast--" + kind : "");
    t.hidden = false;
    if (toastTimer) { clearTimeout(toastTimer); toastTimer = null; }
    if (kind !== "busy") {
      toastTimer = setTimeout(function () { t.hidden = true; }, 5200);
    }
  }

  function setVal(id, text, cls) {
    var el = $(id);
    el.textContent = text;
    el.className = cls ? "val--" + cls : "";
  }

  function errText(err) {
    if (!err) return "unknown error";
    return err.errorText || err.stderr || err.message || JSON.stringify(err);
  }

  /* ---------------------------------------------------------------------- */
  /* Status rendering                                                        */
  /* ---------------------------------------------------------------------- */

  var lastSupported = false;

  function mechLabel(profile, mech) {
    if (profile === "webos25-armel-gst124") return "decoder-inject (patched dtsdec)";
    if (profile === "cx-armv7-gst114") return "demuxer-override (rebuilt LG libs)";
    return mech || "none";
  }

  function renderStatus(s) {
    s = s || {};
    var profile = s.profile || "unknown";
    var supported = !!s.supported;
    lastSupported = supported;

    setVal("stProfile", profile, supported ? "ok" : "warn");
    setVal("stMech", mechLabel(profile, s.mechanism), supported ? null : "warn");
    setVal("stModel", s.model || "unknown");
    setVal("stWebos", s.webosVersion || "unknown");
    setVal("stGst", s.gstVersion || "unknown");
    setVal("stAbi", s.floatAbi || "unknown");
    setVal("stDisable", s.disableMechanism || "unknown");

    var active = !!s.active;
    setVal("stActive", active ? "yes" : "no", active ? "ok" : "off");

    // Container support (mp4/ts/m2ts) — only meaningful on the webOS 25 profile.
    if (profile === "webos25-armel-gst124") {
      var cont = !!s.containersActive;
      setVal("stContainers", cont ? "yes" : "no", cont ? "ok" : "off");
    } else {
      setVal("stContainers", "n/a");
    }

    if (!supported) {
      setVal("stVerified", "n/a", "warn");
    } else if (s.verified) {
      setVal("stVerified", "yes (webOS 25 / C5)", "ok");
    } else {
      setVal("stVerified", "NO - unverified on hardware", "warn");
    }

    // Master pill.
    var pill = $("masterState");
    if (!supported) {
      pill.textContent = "unsupported TV";
      pill.className = "pill pill--unknown";
    } else if (active) {
      pill.textContent = "DTS enabled";
      pill.className = "pill pill--on";
    } else {
      pill.textContent = "DTS disabled";
      pill.className = "pill pill--off";
    }

    // Gate the action buttons.
    $("btnEnable").disabled = !supported;
    $("btnDisable").disabled = !supported;
    $("btnUninstall").disabled = !supported;
    $("unsupportedNote").hidden = supported;

    // Test features: only the webOS 25 profile has a self-test + bundled samples.
    var canTest = profile === "webos25-armel-gst124";
    $("btnTest").disabled = !canTest;
    $("btnPlayMp4").disabled = !canTest;
    $("btnPlayTs").disabled = !canTest;
    $("btnPlayM2ts").disabled = !canTest;
  }

  /* Map a self-test verdict to a status cell. */
  function renderTestResults(res) {
    var r = (res && res.results) || {};
    [["mp4", "tRmp4"], ["ts", "tRts"], ["m2ts", "tRm2ts"]].forEach(function (pair) {
      var v = r[pair[0]] || {};
      var verdict = v.verdict || "—";
      var cls = verdict === "PASS" ? "ok" : (verdict === "FAIL" ? "warn" : null);
      var label = verdict === "PASS" ? "PASS (decoded)"
                : verdict === "FAIL" ? "FAIL (no audio)"
                : verdict === "MISSING" ? "sample missing" : "—";
      setVal(pair[1], label, cls);
    });
  }

  /* ---------------------------------------------------------------------- */
  /* Actions                                                                 */
  /* ---------------------------------------------------------------------- */

  function refreshStatus() {
    toast("Detecting target…", "busy");
    return callService("status", {}).then(function (res) {
      renderStatus(res);
      toast("Detected: " + (res.profile || "unknown"), res.supported ? "ok" : "err");
    }).catch(function (err) {
      toast("Status failed: " + errText(err), "err");
      $("masterState").textContent = "unavailable";
      $("masterState").className = "pill pill--unknown";
    });
  }

  function doEnable() {
    if (!lastSupported) { toast("Enable refused: unsupported profile", "err"); return; }
    toast("Enabling DTS…", "busy");
    callService("enable", {}).then(function (r) {
      toast("DTS enabled (" + (r.profile || "?") + ")", "ok");
      return refreshStatus();
    }).catch(function (e) { toast("Enable failed: " + errText(e), "err"); });
  }

  function doDisable() {
    toast("Disabling DTS…", "busy");
    callService("disable", {}).then(function (r) {
      toast("DTS disabled (reboot to fully clear the registry override)", "ok");
      return refreshStatus();
    }).catch(function (e) { toast("Disable failed: " + errText(e), "err"); });
  }

  function doUninstall() {
    toast("Uninstalling…", "busy");
    callService("uninstall", {}).then(function (r) {
      toast("Uninstalled (power-cycle the TV to clear any registry override)", "ok");
      return refreshStatus();
    }).catch(function (e) { toast("Uninstall failed: " + errText(e), "err"); });
  }

  function doTest() {
    toast("Running self-test (decoding samples)…", "busy");
    ["tRmp4", "tRts", "tRm2ts"].forEach(function (id) { setVal(id, "testing…"); });
    callService("test", {}).then(function (res) {
      renderTestResults(res);
      toast(res.summary || "Self-test done", res.pass ? "ok" : "err");
    }).catch(function (e) {
      ["tRmp4", "tRts", "tRm2ts"].forEach(function (id) { setVal(id, "—"); });
      toast("Self-test failed: " + errText(e), "err");
    });
  }

  // Play a bundled sample in-app (relative path resolves under the app root).
  var TEST_FILES = {
    mp4:  "payload/testfiles/DTS-in-mp4.mp4",
    ts:   "payload/testfiles/DTS-HD-MA-5.1.ts",
    m2ts: "payload/testfiles/DTS-HD-MA-5.1.m2ts"
  };
  function doPlay(key) {
    var v = $("testVideo");
    var src = TEST_FILES[key];
    if (!src) return;
    v.hidden = false;
    v.src = src;
    v.setAttribute("data-nav", "");   // make it focusable/scrollable
    toast("Playing " + key + " sample… (listen for audio)", "busy");
    var p = v.play();
    if (p && typeof p.catch === "function") {
      p.catch(function () {
        toast("In-app player couldn't play the " + key + " sample; try it from USB in Media Player.", "err");
      });
    }
    v.scrollIntoView({ block: "nearest" });
  }

  /* ---------------------------------------------------------------------- */
  /* Spatial (D-pad) navigation                                             */
  /* ---------------------------------------------------------------------- */

  var KEY = { LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, ENTER: 13, BACK: 461 };

  function focusables() {
    return Array.prototype.slice.call(document.querySelectorAll("[data-nav]"))
      .filter(function (el) { return !el.disabled; });
  }

  function currentFocusIndex(list) {
    for (var i = 0; i < list.length; i++) {
      if (list[i].classList.contains("is-focused")) return i;
    }
    return -1;
  }

  function setFocus(el) {
    Array.prototype.slice.call(document.querySelectorAll("[data-nav]"))
      .forEach(function (n) { n.classList.remove("is-focused"); });
    if (!el) return;
    el.classList.add("is-focused");
    if (typeof el.focus === "function") { try { el.focus(); } catch (e) {} }
    el.scrollIntoView({ block: "nearest", inline: "nearest" });
  }

  /* Geometry-based nearest-neighbour navigation. */
  function move(dir) {
    var list = focusables();
    if (!list.length) return;
    var idx = currentFocusIndex(list);
    if (idx < 0) { setFocus(list[0]); return; }

    var cur = list[idx].getBoundingClientRect();
    var cx = cur.left + cur.width / 2;
    var cy = cur.top + cur.height / 2;

    var best = null, bestScore = Infinity;
    list.forEach(function (el, i) {
      if (i === idx) return;
      var r = el.getBoundingClientRect();
      var ex = r.left + r.width / 2;
      var ey = r.top + r.height / 2;
      var dx = ex - cx, dy = ey - cy;
      var ok =
        (dir === KEY.LEFT && dx < -4) ||
        (dir === KEY.RIGHT && dx > 4) ||
        (dir === KEY.UP && dy < -4) ||
        (dir === KEY.DOWN && dy > 4);
      if (!ok) return;
      var primary = (dir === KEY.LEFT || dir === KEY.RIGHT) ? Math.abs(dx) : Math.abs(dy);
      var cross = (dir === KEY.LEFT || dir === KEY.RIGHT) ? Math.abs(dy) : Math.abs(dx);
      var score = primary + cross * 2;
      if (score < bestScore) { bestScore = score; best = el; }
    });
    if (best) setFocus(best);
  }

  function activate(el) { if (el) el.click(); }

  function onKey(e) {
    var list = focusables();
    var idx = currentFocusIndex(list);
    var cur = idx >= 0 ? list[idx] : null;
    switch (e.keyCode) {
      case KEY.LEFT:  move(KEY.LEFT);  e.preventDefault(); break;
      case KEY.RIGHT: move(KEY.RIGHT); e.preventDefault(); break;
      case KEY.UP:    move(KEY.UP);    e.preventDefault(); break;
      case KEY.DOWN:  move(KEY.DOWN);  e.preventDefault(); break;
      case KEY.ENTER: activate(cur);   e.preventDefault(); break;
      case KEY.BACK:  break; // let the platform close the app
      default: break;
    }
  }

  function wirePointerFocus() {
    Array.prototype.slice.call(document.querySelectorAll("[data-nav]"))
      .forEach(function (el) {
        el.addEventListener("mouseenter", function () { if (!el.disabled) setFocus(el); });
        el.addEventListener("click", function () { if (!el.disabled) setFocus(el); });
      });
  }

  /* ---------------------------------------------------------------------- */
  /* Wire up                                                                 */
  /* ---------------------------------------------------------------------- */

  function init() {
    $("btnEnable").addEventListener("click", doEnable);
    $("btnDisable").addEventListener("click", doDisable);
    $("btnUninstall").addEventListener("click", doUninstall);
    $("btnRefresh").addEventListener("click", refreshStatus);
    $("btnTest").addEventListener("click", doTest);
    $("btnPlayMp4").addEventListener("click", function () { doPlay("mp4"); });
    $("btnPlayTs").addEventListener("click", function () { doPlay("ts"); });
    $("btnPlayM2ts").addEventListener("click", function () { doPlay("m2ts"); });

    wirePointerFocus();
    document.addEventListener("keydown", onKey);
    setFocus($("btnRefresh"));

    refreshStatus();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
