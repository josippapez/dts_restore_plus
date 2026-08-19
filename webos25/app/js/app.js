/* DTS Enabler (universal) - frontend controller.
 *
 * Talks to our own JS service (io.github.josippapez.dtsenabler.service), which detects
 * the TV generation and shells the matching mechanism out as root through the
 * Homebrew Channel exec service. The frontend never touches root directly.
 * Most calls (detect/status/enable/disable/uninstall/test) take no parameters;
 * setMakeupGain is the one exception, sending {dts, truehd} gain dB values plus
 * {presetDts, presetThd} DRC presets (off/light/medium/night) and {centerDts,
 * centerThd} dialogue-boost dB values. The service clamps/validates all of it
 * (gain to [-20,+20], center to [-10,+10], preset against a fixed enum) before
 * any of it ever reaches a shell command.
 *
 * callService() uses webOS.service.request when the platform bridge is present,
 * and falls back to a raw PalmServiceBridge so the UI is testable in a plain
 * browser too.
 */
(function () {
  "use strict";

  var SERVICE = "luna://io.github.josippapez.dtsenabler.service";

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

  // Two-step "Try anyway (experimental)" force-enable arming state. Reset on
  // every status render (re-detect) and on focus leaving the button, so a
  // remote misclick can never apply it.
  var forcePending = false;

  function forceButtonReset() {
    forcePending = false;
    var btn = $("btnForce");
    if (btn) { btn.textContent = "Try anyway (experimental)"; }
  }

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

    // Verdict: drives pill wording/class and the report/opt-in block.
    // Falls back to the pre-verdict boolean so the UI still degrades sanely
    // if an older service build ever omits the field.
    var verdict = s.verdict || (supported ? "verified" : "unverified");

    // Master pill.
    var pill = $("masterState");
    if (verdict === "drift") {
      pill.textContent = "paused: firmware changed";
      pill.className = "pill pill--unknown";
    } else if (verdict === "unverified" || verdict === "refused" || !supported) {
      pill.textContent = "unsupported TV";
      pill.className = "pill pill--unknown";
    } else if (active) {
      pill.textContent = "DTS enabled" + (verdict === "forced" ? " (unverified TV)" : "");
      pill.className = "pill pill--on";
    } else {
      pill.textContent = "DTS disabled" + (verdict === "forced" ? " (unverified TV)" : "");
      pill.className = "pill pill--off";
    }

    // Gate the action buttons.
    $("btnEnable").disabled = !supported;
    $("btnDisable").disabled = !supported;
    $("btnUninstall").disabled = !supported;
    renderVerdict(s, verdict);
    renderHookStale(s);
    renderPayloadStale(s);

    // Test features: only the webOS 25 profile has a self-test + bundled samples.
    var canTest = profile === "webos25-armel-gst124";
    $("btnTest").disabled = !canTest;
    $("btnPlayMp4").disabled = !canTest;
    $("btnPlayTs").disabled = !canTest;
    $("btnPlayM2ts").disabled = !canTest;
    // A/B compare renders through the patched dtsdec, so it needs the same profile.
    $("btnAb").disabled = !canTest;
  }

  /* Installed-boot-script staleness.
   *
   * The boot script on the TV is only rewritten by Enable (or the CLI installer),
   * so a TV enabled under an older app keeps that script -- and its verified-sets
   * table -- until Enable is pressed again. The service compares the installed
   * stamp against the one this build ships and reports hookStale; we surface it as
   * a note rather than silently rewriting a privileged script on detect. Applies
   * even on a verified TV, which is why it lives outside the verdict block.
   */
  function renderHookStale(s) {
    var note = $("hookStaleNote");
    if (!note) return;
    var stale = !!s.hookStale;
    note.hidden = !stale;
    if (stale && s.hookGateVersion && s.appGateVersion) {
      note.setAttribute("data-versions", "installed " + s.hookGateVersion + ", app " + s.appGateVersion);
    }
  }

  /* "The staged decoders are older than the ones this app version ships."
   *
   * The sibling of renderHookStale, for the payload rather than the boot script.
   * An app update replaces the bundled .so but never re-runs Enable, so the TV
   * keeps decoding with whatever it was last enabled with -- and unlike the boot
   * script the binaries carry no version stamp, so the service compares md5s
   * instead. Same policy: surface it, never silently re-stage on detect, since
   * that would re-apply a mechanism the user may have chosen to Disable.
   *
   * The reason text is rendered from the service (it names the files that differ)
   * rather than hard-coded, so the note says which decoder is behind.
   */
  function renderPayloadStale(s) {
    var note = $("payloadStaleNote");
    if (!note) return;
    var stale = !!s.payloadStale;
    note.hidden = !stale;
    if (!stale) return;
    var reason = $("payloadStaleReason");
    if (reason) reason.textContent = s.payloadStaleReason || "The staged decoders differ from the ones this app version ships.";
    if (s.payloadStaleFiles && s.payloadStaleFiles.length) {
      note.setAttribute("data-files", s.payloadStaleFiles.join(" "));
    }
  }

  /* Verdict report block + "Try anyway (experimental)" opt-in.
   *
   * Shown whenever the verdict isn't verified/forced: the human reason, the
   * six values a maintainer needs to add the set to the verified table
   * (PRODUCT_ID, WEBOS_RELEASE, GST_VERSION, and the three measured stock
   * plugin md5s), and the report-issue line. The force button only appears
   * when the service says this TV's dynamic dependencies actually resolve
   * (canForce) and the verdict is exactly "unverified" -- drift/refused never
   * get an opt-in.
   */
  function md5OrNote(v) {
    return v ? v : "n/a (unmeasurable — our binds are already active)";
  }

  function renderVerdict(s, verdict) {
    var showBlock = verdict !== "verified" && verdict !== "forced";
    $("verdictBlock").hidden = !showBlock;

    if (showBlock) {
      setVal("verdictReason", s.verdictReason ||
        "This TV does not match a supported profile, so Enable is refused.");
      var measured = s.measured || {};
      $("verdictReport").textContent =
        "PRODUCT_ID=" + (s.model || "unknown") + "\n" +
        "WEBOS_RELEASE=" + (s.webosVersion || "unknown") + "\n" +
        "GST_VERSION=" + (s.gstVersion || "unknown") + "\n" +
        "libgstlibav.so=" + md5OrNote(measured.libgstlibav) + "\n" +
        "libgstisomp4.so=" + md5OrNote(measured.libgstisomp4) + "\n" +
        "libgstmpegtsdemux.so=" + md5OrNote(measured.libgstmpegtsdemux);
    }

    // The opt-in only ever applies to "unverified" (never drift/refused), and
    // only when the service confirms our dynamic deps resolve on this TV.
    var canForceNow = verdict === "unverified" && !!s.canForce;
    var btn = $("btnForce");
    btn.hidden = !canForceNow;
    btn.disabled = !canForceNow;
    $("forceWarning").hidden = !canForceNow;
    // Any re-detect (this function runs on every status render) drops the arm.
    forceButtonReset();
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

  // Two-step "Try anyway (experimental)" force-enable: a remote misclick must
  // not apply it. First press only arms the button (label changes); the
  // second press actually calls enable with {force:true}. Arming is reset by
  // renderVerdict() on any re-detect, and by a blur listener below on focus
  // moving elsewhere (navigation away without a re-detect).
  function doForceEnable() {
    if (!forcePending) {
      forcePending = true;
      $("btnForce").textContent = "Press again to apply on an unverified TV";
      return;
    }
    forcePending = false;
    $("btnForce").textContent = "Try anyway (experimental)";
    toast("Force-enabling on an unverified TV…", "busy");
    callService("enable", { force: true }).then(function (r) {
      toast("DTS force-enabled (unverified TV) — " + (r.profile || "?"), "ok");
      return refreshStatus();
    }).catch(function (e) { toast("Force-enable failed: " + errText(e), "err"); });
  }

  /* Disable/Uninstall can now succeed PARTIALLY, and saying so is the point.
   *
   * The service reports registryReverted:false when LG's plugin registry could not
   * be rebuilt, uninstallDeferred:true when the staged files were kept on purpose
   * because of that, and unmountWarning when an override could not be detached even
   * lazily. Printing "registry restored to stock" over any of those would be exactly
   * the silent-partial-success this whole change exists to remove -- so the happy
   * text is only used when nothing was flagged, and r.warning carries the detail.
   */
  function completionToast(r, okText, deferredText) {
    if (r && r.warning) {
      toast((r.uninstallDeferred ? deferredText : okText) + " — " + r.warning, "err");
    } else {
      toast(okText, "ok");
    }
  }

  function doDisable() {
    toast("Disabling DTS…", "busy");
    callService("disable", {}).then(function (r) {
      completionToast(r,
        "DTS disabled (registry restored to stock; no reboot needed)",
        "DTS disable incomplete");
      return refreshStatus();
    }).catch(function (e) { toast("Disable failed: " + errText(e), "err"); });
  }

  function doUninstall() {
    toast("Uninstalling…", "busy");
    callService("uninstall", {}).then(function (r) {
      completionToast(r,
        "Uninstalled (registry restored to stock; no reboot needed)",
        "Uninstall deferred — files kept on purpose; reboot and try again");
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
  /* Per-codec make-up gain                                                  */
  /* ---------------------------------------------------------------------- */

  // Stepper state. The service clamps authoritatively; these bounds just keep
  // the on-screen value sane while stepping. Mirrors the config contract.
  var GAIN_MIN = -20, GAIN_MAX = 20, GAIN_STEP_DECIMALS = 1;
  var gainVal   = { dts: 0, truehd: 0 };   // what's on screen
  var gainSaved = { dts: 0, truehd: 0 };   // what's on disk (for the dirty marker)

  var GAIN_EL = { dts: "gainDts", truehd: "gainTruehd" };

  function renderGain(which) {
    var el = $(GAIN_EL[which]);
    if (!el) return;
    var v = gainVal[which];
    el.textContent = (v > 0 ? "+" : "") + v.toFixed(GAIN_STEP_DECIMALS) + " dB";
    // Highlight while the on-screen value differs from what's saved on disk.
    if (v === gainSaved[which]) el.classList.remove("is-dirty");
    else el.classList.add("is-dirty");
  }

  function stepGain(which, delta) {
    var v = gainVal[which] + delta;
    if (v < GAIN_MIN) v = GAIN_MIN;
    if (v > GAIN_MAX) v = GAIN_MAX;
    // Avoid float drift accumulating over many 0.5 steps.
    gainVal[which] = Math.round(v * 10) / 10;
    renderGain(which);
  }

  /* ---------------------------------------------------------------------- */
  /* Per-codec DRC preset (Off/Light/Medium/Night)                          */
  /* ---------------------------------------------------------------------- */

  // Mirrors the EPIC "Preset mapping" table; the service does the actual
  // drc/drc_boost/drc_cut mapping and clamping, this is display-only.
  var PRESET_ORDER = ["off", "light", "medium", "night"];
  var PRESET_LABEL = { off: "Off", light: "Light", medium: "Medium", night: "Night" };
  var presetVal   = { dts: "off", truehd: "off" };  // what's on screen
  var presetSaved = { dts: "off", truehd: "off" };  // what's on disk

  var PRESET_EL = { dts: "presetDts", truehd: "presetTruehd" };

  function renderPreset(which) {
    var el = $(PRESET_EL[which]);
    if (!el) return;
    var v = presetVal[which];
    el.textContent = PRESET_LABEL[v] || "Off";
    if (v === presetSaved[which]) el.classList.remove("is-dirty");
    else el.classList.add("is-dirty");
  }

  function stepPreset(which, dir) {
    var idx = PRESET_ORDER.indexOf(presetVal[which]);
    if (idx < 0) idx = 0;
    idx = (idx + dir + PRESET_ORDER.length) % PRESET_ORDER.length;
    presetVal[which] = PRESET_ORDER[idx];
    renderPreset(which);
  }

  /* ---------------------------------------------------------------------- */
  /* Per-codec dialogue (center-channel) boost                              */
  /* ---------------------------------------------------------------------- */

  var CENTER_MIN = -10, CENTER_MAX = 10, CENTER_STEP_DECIMALS = 1;
  var centerVal   = { dts: 0, truehd: 0 };   // what's on screen
  var centerSaved = { dts: 0, truehd: 0 };   // what's on disk

  var CENTER_EL = { dts: "centerDts", truehd: "centerTruehd" };

  function renderCenter(which) {
    var el = $(CENTER_EL[which]);
    if (!el) return;
    var v = centerVal[which];
    el.textContent = (v > 0 ? "+" : "") + v.toFixed(CENTER_STEP_DECIMALS) + " dB";
    if (v === centerSaved[which]) el.classList.remove("is-dirty");
    else el.classList.add("is-dirty");
  }

  function stepCenter(which, delta) {
    var v = centerVal[which] + delta;
    if (v < CENTER_MIN) v = CENTER_MIN;
    if (v > CENTER_MAX) v = CENTER_MAX;
    centerVal[which] = Math.round(v * 10) / 10;
    renderCenter(which);
  }

  // Read the current on-device values back, if the service exposes them
  // (nice-to-have; on failure the controls just keep their Off/0.0 defaults).
  function loadGain() {
    return callService("getMakeupGain", {}).then(function (res) {
      if (!res) return;
      if (typeof res.dts === "number") gainVal.dts = gainSaved.dts = res.dts;
      if (typeof res.truehd === "number") gainVal.truehd = gainSaved.truehd = res.truehd;
      if (PRESET_LABEL.hasOwnProperty(res.presetDts)) presetVal.dts = presetSaved.dts = res.presetDts;
      if (PRESET_LABEL.hasOwnProperty(res.presetThd)) presetVal.truehd = presetSaved.truehd = res.presetThd;
      if (typeof res.centerDts === "number") centerVal.dts = centerSaved.dts = res.centerDts;
      if (typeof res.centerThd === "number") centerVal.truehd = centerSaved.truehd = res.centerThd;
    }).catch(function () { /* leave the Off/0.0 defaults in place */ })
      .then(function () {
        renderGain("dts"); renderGain("truehd");
        renderPreset("dts"); renderPreset("truehd");
        renderCenter("dts"); renderCenter("truehd");
      });
  }

  function doSaveGain() {
    toast("Saving audio settings…", "busy");
    callService("setMakeupGain", {
      dts: gainVal.dts, truehd: gainVal.truehd,
      presetDts: presetVal.dts, presetThd: presetVal.truehd,
      centerDts: centerVal.dts, centerThd: centerVal.truehd
    }).then(function (r) {
      // Reflect the server-clamped/validated values back into the controls.
      if (typeof r.dts === "number") gainVal.dts = r.dts;
      if (typeof r.truehd === "number") gainVal.truehd = r.truehd;
      if (PRESET_LABEL.hasOwnProperty(r.presetDts)) presetVal.dts = r.presetDts;
      if (PRESET_LABEL.hasOwnProperty(r.presetThd)) presetVal.truehd = r.presetThd;
      if (typeof r.centerDts === "number") centerVal.dts = r.centerDts;
      if (typeof r.centerThd === "number") centerVal.truehd = r.centerThd;

      gainSaved.dts = gainVal.dts; gainSaved.truehd = gainVal.truehd;
      presetSaved.dts = presetVal.dts; presetSaved.truehd = presetVal.truehd;
      centerSaved.dts = centerVal.dts; centerSaved.truehd = centerVal.truehd;

      renderGain("dts"); renderGain("truehd");
      renderPreset("dts"); renderPreset("truehd");
      renderCenter("dts"); renderCenter("truehd");

      toast("Saved: DTS " + gainVal.dts + " dB / " + PRESET_LABEL[presetVal.dts] + " / centre " +
            centerVal.dts + " dB, TrueHD " + gainVal.truehd + " dB / " + PRESET_LABEL[presetVal.truehd] +
            " / centre " + centerVal.truehd + " dB (applies next playback)", "ok");
    }).catch(function (e) { toast("Save failed: " + errText(e), "err"); });
  }

  /* ---------------------------------------------------------------------- */
  /* A/B compare (bundled DTS clip: DRC off vs the saved settings)          */
  /* ---------------------------------------------------------------------- */

  // Filled by abPreview; each is {url, bytes, rendered, meanDb, peakDb, ...}.
  // The service stamps a fresh basename into `url` on every render, so the
  // player can never serve a previous take -- and the URL must stay free of a
  // "?r=" cache-buster: webOS's starfish pipeline does not strip the query, so
  // it looks for a file literally named "..._a.wav?r=1" and refuses the clip.
  var abState = { a: null, b: null };

  function abDb(v) {
    if (typeof v !== "number") return "n/a";
    return (v > 0 ? "+" : "") + v.toFixed(1) + " dB";
  }

  function abVariantText(v) {
    if (!v) return "—";
    if (!v.rendered) return "render failed";
    var s = Math.round(v.bytes / 1024) + " KB";
    if (typeof v.meanDb === "number") s += " · mean " + abDb(v.meanDb) + ", peak " + abDb(v.peakDb);
    return s;
  }

  function abSetPlayEnabled() {
    $("btnAbA").disabled = !(abState.a && abState.a.rendered);
    $("btnAbB").disabled = !(abState.b && abState.b.rendered);
  }

  function doAbRender() {
    $("btnAb").disabled = true;
    abState.a = abState.b = null;
    abSetPlayEnabled();
    setVal("abA", "rendering…"); setVal("abB", "rendering…");
    setVal("abDelta", "—"); setVal("abConf", "—");
    toast("Rendering A/B on the TV (a few seconds)…", "busy");

    callService("abPreview", {}).then(function (res) {
      abState.a = res.a || null;
      abState.b = res.b || null;
      setVal("abA", abVariantText(res.a), res.a && res.a.rendered ? "ok" : "warn");
      setVal("abB", abVariantText(res.b), res.b && res.b.rendered ? "ok" : "warn");

      if (res.measured && typeof res.deltaMeanDb === "number") {
        setVal("abDelta", abDb(res.deltaMeanDb) + " mean, " + abDb(res.deltaPeakDb) + " peak",
               res.deltaMeanDb === 0 ? "warn" : "ok");
      } else {
        setVal("abDelta", "not measured — " + (res.measureNote || "no numbers available"), "warn");
      }

      setVal("abConf", res.configUnchanged ? "unchanged (" + res.configProof + ")"
                                           : "CHECK: " + (res.configProof || "could not verify"),
             res.configUnchanged ? "ok" : "warn");

      abSetPlayEnabled();
      if (abState.a || abState.b) {
        toast("A/B ready — play A, then B, and listen to the same clip twice.", "ok");
      } else {
        toast("A/B render produced nothing playable; see the numbers above.", "err");
      }
    }).catch(function (e) {
      setVal("abA", "—"); setVal("abB", "—");
      setVal("abDelta", "—"); setVal("abConf", "—");
      abSetPlayEnabled();
      toast("A/B failed: " + errText(e), "err");
    }).then(function () {
      $("btnAb").disabled = false;
    });
  }

  function doAbPlay(which) {
    var v = abState[which];
    if (!v || !v.rendered) return;
    var p = $("abPlayer");
    p.hidden = false;
    p.src = v.url;
    toast("Playing " + v.label, "busy");
    var pr = p.play();
    if (pr && typeof pr.catch === "function") {
      pr.catch(function () {
        toast("The in-app player refused the rendered clip; the measured numbers above still hold.", "err");
      });
    }
    p.scrollIntoView({ block: "nearest" });
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
    $("btnForce").addEventListener("click", doForceEnable);
    // Navigation away from the armed button (without a re-detect) also
    // disarms it -- a misclick elsewhere must never leave it primed.
    $("btnForce").addEventListener("blur", forceButtonReset);
    $("btnTest").addEventListener("click", doTest);
    $("btnPlayMp4").addEventListener("click", function () { doPlay("mp4"); });
    $("btnPlayTs").addEventListener("click", function () { doPlay("ts"); });
    $("btnPlayM2ts").addEventListener("click", function () { doPlay("m2ts"); });
    $("btnSaveGain").addEventListener("click", doSaveGain);
    $("btnAb").addEventListener("click", doAbRender);
    $("btnAbA").addEventListener("click", function () { doAbPlay("a"); });
    $("btnAbB").addEventListener("click", function () { doAbPlay("b"); });
    // A <video>/<audio> src that the pipeline can't handle fails via `error` on
    // the element, not via the play() promise, so report that path too.
    $("abPlayer").addEventListener("error", function () {
      toast("The in-app player couldn't load the rendered clip; the measured numbers above still hold.", "err");
    });
    // Don't leave ~1 MB of rendered wav in the app directory after we're gone.
    window.addEventListener("pagehide", function () {
      callService("abCleanup", {}).catch(function () { /* best effort */ });
    });

    // Gain +/- steppers (data-gain = which codec, data-step = delta in dB).
    var stepBtns = document.querySelectorAll("[data-gain][data-step]");
    for (var i = 0; i < stepBtns.length; i++) {
      stepBtns[i].addEventListener("click", function () {
        stepGain(this.getAttribute("data-gain"), parseFloat(this.getAttribute("data-step")));
      });
    }

    // DRC preset cycling buttons (data-preset = which codec, data-pdir = +1/-1).
    var presetBtns = document.querySelectorAll("[data-preset][data-pdir]");
    for (var j = 0; j < presetBtns.length; j++) {
      presetBtns[j].addEventListener("click", function () {
        stepPreset(this.getAttribute("data-preset"), parseInt(this.getAttribute("data-pdir"), 10));
      });
    }

    // Dialogue (center) boost +/- steppers (data-center = which codec, data-step = delta in dB).
    var centerBtns = document.querySelectorAll("[data-center][data-step]");
    for (var k = 0; k < centerBtns.length; k++) {
      centerBtns[k].addEventListener("click", function () {
        stepCenter(this.getAttribute("data-center"), parseFloat(this.getAttribute("data-step")));
      });
    }

    wirePointerFocus();
    document.addEventListener("keydown", onKey);
    setFocus($("btnRefresh"));

    refreshStatus();
    loadGain();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
