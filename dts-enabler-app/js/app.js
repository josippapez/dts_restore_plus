/* DTS Enabler - frontend controller.
 *
 * Talks to our own JS service (org.webosbrew.dtsenabler.service), which in turn
 * shells out as root through the Homebrew Channel exec service. The frontend
 * never touches root directly.
 *
 * All calls go through callService(), which uses webOS.service.request when the
 * platform bridge is present, and falls back to a raw PalmServiceBridge so the
 * app is testable in a plain browser context as well.
 */
(function () {
  "use strict";

  var SERVICE = "luna://org.webosbrew.dtsenabler.service";

  /* Downmix parameter definitions - the single source of truth for the UI,
   * defaults, and (client-side) validation ranges. Defaults mirror the
   * upstream dts_restore install.sh [downmix] section. */
  var DOWNMIX_PARAMS = [
    { key: "front",  label: "Front",  def: 1.25 },
    { key: "center", label: "Center", def: 0.75 },
    { key: "lfe",    label: "LFE",    def: 0.75 },
    { key: "rear",   label: "Rear",   def: 0.75 },
    { key: "rear2",  label: "Rear 2", def: 0.70 }
  ];
  var DOWNMIX_MIN = 0.0;
  var DOWNMIX_MAX = 2.0;
  var DOWNMIX_STEP = 0.05;

  /* ---------------------------------------------------------------------- */
  /* Service bridge                                                          */
  /* ---------------------------------------------------------------------- */

  /**
   * Call a method on our JS service.
   * @param {string} method  e.g. "status"
   * @param {object} params  parameters object (serialised to JSON)
   * @returns {Promise<object>} resolves with the service payload
   */
  function callService(method, params) {
    params = params || {};
    return new Promise(function (resolve, reject) {
      // Preferred path: webOSTV.js injected webOS.service.request
      if (window.webOS && window.webOS.service && window.webOS.service.request) {
        window.webOS.service.request(SERVICE, {
          method: method,
          parameters: params,
          onSuccess: function (res) { resolve(res); },
          onFailure: function (err) { reject(err); }
        });
        return;
      }
      // Fallback path: raw PalmServiceBridge
      if (window.PalmServiceBridge) {
        var bridge = new window.PalmServiceBridge();
        bridge.onservicecallback = function (raw) {
          var res;
          try { res = JSON.parse(raw); } catch (e) { reject({ errorText: "bad JSON: " + raw }); return; }
          if (res && res.returnValue === false) { reject(res); }
          else { resolve(res); }
        };
        bridge.call(SERVICE + "/" + method, JSON.stringify(params));
        return;
      }
      reject({ errorText: "No webOS service bridge available (not running on a TV?)" });
    });
  }

  /* ---------------------------------------------------------------------- */
  /* Small DOM helpers                                                       */
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
      toastTimer = setTimeout(function () { t.hidden = true; }, 4200);
    }
  }

  function setVal(id, text, cls) {
    var el = $(id);
    el.textContent = text;
    el.className = cls ? "val--" + cls : "";
  }

  /* ---------------------------------------------------------------------- */
  /* Downmix validation                                                      */
  /* ---------------------------------------------------------------------- */

  /**
   * Coerce and validate a single downmix coefficient.
   * SECURITY: these values are eventually interpolated into a root shell
   * command on the service side. We only ever forward a strict numeric string
   * (fixed 2 decimals, in-range) so nothing shell-meaningful can survive.
   * The service repeats this validation server-side; this is defence in depth.
   * @returns {string|null} normalised "d.dd" string, or null if invalid
   */
  function normaliseCoeff(raw) {
    var n = Number(raw);
    if (!isFinite(n)) return null;
    if (n < DOWNMIX_MIN || n > DOWNMIX_MAX) return null;
    // Fixed 2-decimal string: purely [0-9.] characters, no exponent/sign tricks.
    return n.toFixed(2);
  }

  /* ---------------------------------------------------------------------- */
  /* Build the downmix sliders                                               */
  /* ---------------------------------------------------------------------- */

  function buildSliders() {
    var wrap = $("sliders");
    wrap.innerHTML = "";
    DOWNMIX_PARAMS.forEach(function (p) {
      var cell = document.createElement("div");
      cell.className = "slider focusable";
      cell.setAttribute("data-nav", "");
      cell.setAttribute("data-slider", p.key);

      var lab = document.createElement("label");
      lab.setAttribute("for", "sl-" + p.key);
      lab.textContent = p.label;

      var val = document.createElement("div");
      val.className = "value";
      val.id = "val-" + p.key;
      val.textContent = p.def.toFixed(2);

      var input = document.createElement("input");
      input.type = "range";
      input.id = "sl-" + p.key;
      input.min = String(DOWNMIX_MIN);
      input.max = String(DOWNMIX_MAX);
      input.step = String(DOWNMIX_STEP);
      input.value = String(p.def);
      input.setAttribute("aria-label", p.label + " coefficient");
      input.addEventListener("input", function () {
        val.textContent = Number(input.value).toFixed(2);
      });

      var hint = document.createElement("span");
      hint.className = "hint";
      hint.textContent = DOWNMIX_MIN.toFixed(2) + "–" + DOWNMIX_MAX.toFixed(2);

      cell.appendChild(lab);
      cell.appendChild(val);
      cell.appendChild(input);
      cell.appendChild(hint);
      wrap.appendChild(cell);
    });
  }

  function readSliders() {
    var out = {};
    for (var i = 0; i < DOWNMIX_PARAMS.length; i++) {
      var p = DOWNMIX_PARAMS[i];
      var v = normaliseCoeff($("sl-" + p.key).value);
      if (v === null) {
        toast("Invalid value for " + p.label + " (must be " + DOWNMIX_MIN + "–" + DOWNMIX_MAX + ")", "err");
        return null;
      }
      out[p.key] = v; // normalised numeric string
    }
    return out;
  }

  function writeSliders(vals) {
    DOWNMIX_PARAMS.forEach(function (p) {
      var v = vals && vals[p.key] != null ? Number(vals[p.key]) : p.def;
      if (!isFinite(v)) v = p.def;
      $("sl-" + p.key).value = String(v);
      $("val-" + p.key).textContent = v.toFixed(2);
    });
  }

  /* ---------------------------------------------------------------------- */
  /* Status rendering                                                        */
  /* ---------------------------------------------------------------------- */

  function renderStatus(s) {
    s = s || {};
    setVal("stModel", s.model || "unknown");
    setVal("stWebos", s.webosVersion || "unknown");
    setVal("stGst", s.gstVersion || "unknown");

    var mounted = !!s.overridesMounted;
    setVal("stMounts", mounted ? "yes" : "no", mounted ? "ok" : "off");

    setVal("stRank", (s.avdecDcaRank != null ? String(s.avdecDcaRank) : "unknown"),
      (s.avdecDcaRank === 290 ? "ok" : "warn"));

    var init = !!s.initInstalled;
    setVal("stInit", init ? "installed" : "not installed", init ? "ok" : "off");

    var dvDisabled = !!s.dvDisabled;
    setVal("stDv", dvDisabled ? "disabled" : "enabled", dvDisabled ? "warn" : "ok");

    // Master pill = enabled when init installed AND overrides mounted.
    var pill = $("masterState");
    if (init && mounted) {
      pill.textContent = "DTS enabled";
      pill.className = "pill pill--on";
    } else if (init || mounted) {
      pill.textContent = "partial — refresh / re-enable";
      pill.className = "pill pill--unknown";
    } else {
      pill.textContent = "DTS disabled";
      pill.className = "pill pill--off";
    }
  }

  /* ---------------------------------------------------------------------- */
  /* Actions                                                                 */
  /* ---------------------------------------------------------------------- */

  function refreshStatus() {
    toast("Reading status…", "busy");
    return callService("status", {}).then(function (res) {
      renderStatus(res);
      if (res.downmix) writeSliders(res.downmix);
      toast("Status updated", "ok");
    }).catch(function (err) {
      toast("Status failed: " + errText(err), "err");
      $("masterState").textContent = "unavailable";
      $("masterState").className = "pill pill--unknown";
    });
  }

  function loadConfig() {
    return callService("getConfig", {}).then(function (res) {
      if (res.downmix) writeSliders(res.downmix);
    }).catch(function () { /* non-fatal: sliders keep defaults */ });
  }

  function doEnable() {
    toast("Enabling DTS…", "busy");
    callService("enable", {}).then(function () {
      toast("DTS enabled", "ok");
      return refreshStatus();
    }).catch(function (e) { toast("Enable failed: " + errText(e), "err"); });
  }

  function doDisable() {
    toast("Disabling DTS…", "busy");
    callService("disable", {}).then(function () {
      toast("DTS disabled (reboot recommended to fully unmount)", "ok");
      return refreshStatus();
    }).catch(function (e) { toast("Disable failed: " + errText(e), "err"); });
  }

  function doApplyDownmix() {
    var vals = readSliders();
    if (!vals) return; // validation already toasted
    toast("Applying downmix…", "busy");
    callService("setDownmix", vals).then(function () {
      toast("Downmix applied", "ok");
      return refreshStatus();
    }).catch(function (e) { toast("Downmix failed: " + errText(e), "err"); });
  }

  function doResetDownmix() {
    var vals = {};
    DOWNMIX_PARAMS.forEach(function (p) { vals[p.key] = p.def; });
    writeSliders(vals);
    toast("Reset to defaults — press Apply to save", "ok");
  }

  function doDv(disabled) {
    toast(disabled ? "Disabling DV…" : "Enabling DV…", "busy");
    callService("setDvDisabled", { disabled: disabled }).then(function () {
      toast(disabled ? "Dolby Vision disabled" : "Dolby Vision enabled", "ok");
      return refreshStatus();
    }).catch(function (e) { toast("DV toggle failed: " + errText(e), "err"); });
  }

  function errText(err) {
    if (!err) return "unknown error";
    return err.errorText || err.stderr || err.message || JSON.stringify(err);
  }

  /* ---------------------------------------------------------------------- */
  /* Spatial (D-pad) navigation                                             */
  /* ---------------------------------------------------------------------- */

  var KEY = { LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, ENTER: 13, BACK: 461 };

  function focusables() {
    return Array.prototype.slice.call(document.querySelectorAll("[data-nav]"));
  }

  function currentFocusIndex(list) {
    for (var i = 0; i < list.length; i++) {
      if (list[i].classList.contains("is-focused")) return i;
    }
    return -1;
  }

  function setFocus(el) {
    focusables().forEach(function (n) { n.classList.remove("is-focused"); });
    if (!el) return;
    el.classList.add("is-focused");
    if (typeof el.focus === "function") { try { el.focus(); } catch (e) {} }
    el.scrollIntoView({ block: "nearest", inline: "nearest" });
  }

  /* Geometry-based nearest-neighbour navigation: robust across the 2-column
   * grid + 5-wide slider row without hard-coding an order. */
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

      // Weight the primary axis; penalise cross-axis drift.
      var primary = (dir === KEY.LEFT || dir === KEY.RIGHT) ? Math.abs(dx) : Math.abs(dy);
      var cross = (dir === KEY.LEFT || dir === KEY.RIGHT) ? Math.abs(dy) : Math.abs(dx);
      var score = primary + cross * 2;
      if (score < bestScore) { bestScore = score; best = el; }
    });

    if (best) setFocus(best);
  }

  function adjustSlider(el, delta) {
    var input = el.querySelector("input[type=range]");
    if (!input) return;
    var v = Number(input.value) + delta;
    if (v < DOWNMIX_MIN) v = DOWNMIX_MIN;
    if (v > DOWNMIX_MAX) v = DOWNMIX_MAX;
    input.value = String(v);
    input.dispatchEvent(new Event("input"));
  }

  function activate(el) {
    if (!el) return;
    if (el.hasAttribute("data-slider")) {
      // Enter on a slider does nothing destructive; left/right adjust it.
      return;
    }
    el.click();
  }

  function onKey(e) {
    var list = focusables();
    var idx = currentFocusIndex(list);
    var cur = idx >= 0 ? list[idx] : null;
    var isSlider = cur && cur.hasAttribute("data-slider");

    switch (e.keyCode) {
      case KEY.LEFT:
        if (isSlider) { adjustSlider(cur, -DOWNMIX_STEP); e.preventDefault(); return; }
        move(KEY.LEFT); e.preventDefault(); break;
      case KEY.RIGHT:
        if (isSlider) { adjustSlider(cur, DOWNMIX_STEP); e.preventDefault(); return; }
        move(KEY.RIGHT); e.preventDefault(); break;
      case KEY.UP: move(KEY.UP); e.preventDefault(); break;
      case KEY.DOWN: move(KEY.DOWN); e.preventDefault(); break;
      case KEY.ENTER: activate(cur); e.preventDefault(); break;
      case KEY.BACK:
        // Let the platform close the app; nothing to do here.
        break;
      default: break;
    }
  }

  /* Mouse/pointer users (and the emulator) can click directly; keep focus in sync. */
  function wirePointerFocus() {
    focusables().forEach(function (el) {
      el.addEventListener("mouseenter", function () { setFocus(el); });
      el.addEventListener("click", function () {
        if (!el.hasAttribute("data-slider")) setFocus(el);
      });
    });
  }

  /* ---------------------------------------------------------------------- */
  /* Wire up                                                                 */
  /* ---------------------------------------------------------------------- */

  function init() {
    buildSliders();

    $("btnEnable").addEventListener("click", doEnable);
    $("btnDisable").addEventListener("click", doDisable);
    $("btnRefresh").addEventListener("click", refreshStatus);
    $("btnApplyDownmix").addEventListener("click", doApplyDownmix);
    $("btnResetDownmix").addEventListener("click", doResetDownmix);
    $("btnDvOn").addEventListener("click", function () { doDv(false); });
    $("btnDvOff").addEventListener("click", function () { doDv(true); });

    wirePointerFocus();
    document.addEventListener("keydown", onKey);

    // Initial focus on the primary action.
    setFocus($("btnEnable"));

    // Load persisted config first (fast), then live status.
    loadConfig().then(refreshStatus);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
