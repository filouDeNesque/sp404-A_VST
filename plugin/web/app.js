    // Minimal vendored port of JUCE's frontend bridge (window.__JUCE__.backend +
    // getNativeFunction), normally consumed via the "juce-framework-frontend" npm package
    // (modules/juce_gui_extra/native/javascript/{check_native_interop,index}.js in the JUCE
    // repo). Inlined here because this placeholder page has no bundler yet — replace with the
    // real npm package once a React/Vite frontend is introduced (see README roadmap).
    //
    // IMPORTANT: window.__JUCE__ itself (postMessage, initialisationData) is injected by the
    // native WebBrowserComponent before this script runs — only the JS-side event-listener
    // bookkeeping and getNativeFunction() wrapper below are ours to provide.
    const hasNativeBridge = typeof window.__JUCE__ !== "undefined";

    (function bootstrapJuceBridge() {
      if (typeof window.__JUCE__ === "undefined") {
        window.__JUCE__ = { postMessage: function () {} };
      }
      if (typeof window.__JUCE__.initialisationData === "undefined") {
        window.__JUCE__.initialisationData = { __juce__functions: [] };
      }

      class ListenerList {
        constructor() { this.listeners = new Map(); this.nextId = 0; }
        add(fn) { const id = this.nextId++; this.listeners.set(id, fn); return id; }
        callAll(payload) { for (const fn of this.listeners.values()) fn(payload); }
      }

      class Backend {
        constructor() { this.listenersByEvent = new Map(); }
        addEventListener(eventId, fn) {
          if (!this.listenersByEvent.has(eventId)) this.listenersByEvent.set(eventId, new ListenerList());
          return this.listenersByEvent.get(eventId).add(fn);
        }
        emitEvent(eventId, payload) {
          window.__JUCE__.postMessage(JSON.stringify({ eventId, payload }));
        }
        // Called by native code to deliver an event/result back into the page.
        emitByBackend(eventId, jsonPayload) {
          if (this.listenersByEvent.has(eventId))
            this.listenersByEvent.get(eventId).callAll(JSON.parse(jsonPayload));
        }
      }

      if (typeof window.__JUCE__.backend === "undefined") window.__JUCE__.backend = new Backend();

      let lastPromiseId = 0;
      const pendingResolvers = new Map();
      window.__JUCE__.backend.addEventListener("__juce__complete", ({ promiseId, result }) => {
        if (pendingResolvers.has(promiseId)) {
          pendingResolvers.get(promiseId)(result);
          pendingResolvers.delete(promiseId);
        }
      });

      window.getNativeFunction = function (name) {
        return function () {
          const promiseId = lastPromiseId++;
          const promise = new Promise((resolve) => pendingResolvers.set(promiseId, resolve));
          window.__JUCE__.backend.emitEvent("__juce__invoke", {
            name: name,
            params: Array.prototype.slice.call(arguments),
            resultId: promiseId,
          });
          return promise;
        };
      };
    })();

    const statusEl = document.getElementById("status");
    const banksEl = document.getElementById("banks");
    const padsEl = document.getElementById("pads");
    const screenEl = document.getElementById("screen");
    const screenReadoutEl = document.getElementById("screen-readout");
    const screenReadoutLabelEl = document.getElementById("screen-readout-label");
    const sdIndicatorEl = document.getElementById("sd-indicator");

    // Pad detail cards always fetch/carry the trim range (origSampleStart/End,
    // userSampleStart/End) but it's not shown by default -- flip this to reveal it again.
    const SHOW_PAD_RANGE = false;

    // Mirrors the SP-404A's own Shift key: each of the 5 physical bank buttons covers 2 of the
    // 10 banks (A/F, B/G, C/H, D/I, E/J -- see updateBankLetterHighlights), Shift picks which.
    let shiftActive = false;

    function updateBankLetterHighlights() {
      for (const el of banksEl.querySelectorAll(".bank")) {
        const letters = el.querySelectorAll(".letter");
        letters[0].classList.toggle("active", !shiftActive);
        letters[1].classList.toggle("active", shiftActive);
      }
    }

    // Bumped on every new request; a response is only rendered if its token is still current.
    // Guards against out-of-order results if the user clicks a second bank before the first
    // bank's listPads() call has resolved.
    let requestToken = 0;

    function setLoading(isLoading) {
      document.body.classList.toggle("loading", isLoading);
    }

    // Paused while an OS file drag is hovering the window (see wirePadDragDrop) -- the polls
    // below each make a native round-trip every 100-2000ms, which is one plausible contributor
    // to the host slowing down while a drag session is active over the plugin's WebView. Cheap
    // to pause either way, so it's guarded even though the sample-transfer path (also fixed to
    // yield in chunks, see arrayBufferToBase64) is the more likely single cause of a *large*
    // slowdown.
    let dragInProgress = false;
    document.addEventListener("dragenter", () => { dragInProgress = true; });
    document.addEventListener("drop", () => { dragInProgress = false; });
    document.addEventListener("dragend", () => { dragInProgress = false; });
    // dragleave fires when the drag leaves any element, including children -- only actually
    // treat it as "drag left the window" when it lands on the document itself, else hovering
    // from one pad to another would flicker dragInProgress off and on constantly.
    document.addEventListener("dragleave", (e) => { if (e.target === document.documentElement) dragInProgress = false; });

    function formatDuration(seconds) {
      return typeof seconds === "number" && isFinite(seconds) ? `${seconds.toFixed(2)}s` : "—";
    }

    function renderPads(bankName, data) {
      const padsHtml = data.pads
        .map((pad, padIndex) => {
          const sample = pad.sampleName
            ? pad.sampleName
            : '<span class="empty">(empty)</span>';
          const tempo =
            pad.tempoMode === "off"
              ? "off"
              : `${pad.tempoMode} — orig ${pad.origBpm.toFixed(1)} / user ${pad.userBpm.toFixed(1)} BPM`;
          const channelsLabel = pad.channels === 2 ? "stereo" : "mono";
          const isTrimmed =
            pad.userSampleStart !== pad.origSampleStart || pad.userSampleEnd !== pad.origSampleEnd;
          const range = isTrimmed
            ? `user ${pad.userSampleStart}–${pad.userSampleEnd} (orig ${pad.origSampleStart}–${pad.origSampleEnd})`
            : `${pad.origSampleStart}–${pad.origSampleEnd}`;
          const hasSampleClass = pad.sampleName ? " has-sample" : "";
          const toggleBtn = (field, label) =>
            `<button type="button" class="toggle-btn${pad[field] ? " on" : ""}" data-field="${field}" aria-pressed="${!!pad[field]}">${label}</button>`;
          const rangeLine = SHOW_PAD_RANGE ? `<span class="detail">range: ${range}</span>` : "";
          return `<div class="pad${hasSampleClass}" data-pad-index="${padIndex}" data-bank="${bankName}" data-index-in-bank="${padIndex + 1}">
            <strong>${pad.label}</strong> — ${sample}<br/>
            <span class="detail">${pad.format}, ${channelsLabel}, ${formatDuration(pad.durationSeconds)}</span><br/>
            <span class="detail">tempo: ${tempo}</span>${rangeLine ? "<br/>" + rangeLine : ""}
            <div class="pad-controls">
              <div class="toggle-row">
                ${toggleBtn("loop", "loop")}
                ${toggleBtn("gate", "gate")}
                ${toggleBtn("reverse", "rev")}
                ${toggleBtn("lofi", "lofi")}
              </div>
              <label class="vol-row">vol
                <input type="range" min="0" max="127" data-field="volume" value="${pad.volume}"/>
                <span class="vol-value">${pad.volume}</span>
              </label>
              <button type="button" class="dsp-open-btn">DSP…</button>
            </div>
          </div>`;
        })
        .join("");
      padsEl.innerHTML = padsHtml;
      void padsEl.offsetHeight; // force a layout reflow after this async DOM mutation
      wirePadControls();
      wirePadPreview();
      wirePadDragDrop();
    }

    // Press-and-hold preview: mousedown triggers the pad, mouseup/mouseleave releases it (so
    // gate-mode pads stop correctly). Goes through the same MIDI trigger path as a real note,
    // see PluginProcessor::previewPadOn/Off.
    function wirePadPreview() {
      const previewOn = window.getNativeFunction("previewPadOn");
      const previewOff = window.getNativeFunction("previewPadOff");

      for (const el of padsEl.querySelectorAll(".pad")) {
        const padIndex = Number(el.dataset.padIndex);
        const release = () => {
          if (!el.classList.contains("pressed")) return;
          el.classList.remove("pressed");
          previewOff(padIndex);
        };
        el.addEventListener("mousedown", (e) => {
          if (e.target.closest(".pad-controls")) return; // don't preview while adjusting a control
          el.classList.add("pressed");
          previewOn(padIndex);
        });
        el.addEventListener("mouseup", release);
        el.addEventListener("mouseleave", release);
      }
    }

    // Editable pad settings: writes immediately to PAD_INFO.BIN on the real SD card on every
    // change (see WebUIBridge::updatePad) -- no separate save step, no undo.
    function wirePadControls() {
      const updatePad = window.getNativeFunction("updatePad");

      for (const el of padsEl.querySelectorAll(".pad")) {
        const bank = el.dataset.bank;
        const indexInBank = Number(el.dataset.indexInBank);
        const volumeValueEl = el.querySelector(".vol-value");

        const push = (field, value) => {
          updatePad(bank, indexInBank, { [field]: value }).then((result) => {
            if (!result || !result.ok) statusEl.textContent = `Failed to update ${bank}${indexInBank} (${field}).`;
          });
        };

        for (const btn of el.querySelectorAll(".toggle-btn[data-field]")) {
          btn.addEventListener("click", () => {
            const on = !btn.classList.contains("on");
            btn.classList.toggle("on", on);
            btn.setAttribute("aria-pressed", String(on));
            push(btn.dataset.field, on);
          });
        }

        const volumeInput = el.querySelector('input[type="range"][data-field="volume"]');
        if (volumeInput) {
          volumeInput.addEventListener("input", () => {
            if (volumeValueEl) volumeValueEl.textContent = volumeInput.value;
          });
          volumeInput.addEventListener("change", () => push("volume", Number(volumeInput.value)));
        }

        const dspBtn = el.querySelector(".dsp-open-btn");
        if (dspBtn) dspBtn.addEventListener("click", () => openDspPanel(bank, indexInBank));
      }
    }

    // ArrayBuffer -> base64, in chunks so String.fromCharCode.apply doesn't blow the call stack
    // on a multi-MB sample (see wirePadDragDrop -- this is the only binary payload the
    // window.__JUCE__ bridge carries anywhere in this app). Yields back to the event loop every
    // few chunks (macrotask via setTimeout, not just a microtask) so a large file doesn't block
    // this thread -- shared with the plugin's whole host UI -- for one long stretch. This won't
    // shrink the total encode time, but it turns one big freeze into short interruptible bursts,
    // which matters a lot when the host (e.g. a DAW) is repainting/processing on the same thread.
    async function arrayBufferToBase64(buffer) {
      const bytes = new Uint8Array(buffer);
      const chunkSize = 0x8000;
      const chunksPerYield = 32; // ~1MB between yields
      let binary = "";
      for (let i = 0; i < bytes.length; i += chunkSize) {
        binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunkSize));
        if ((i / chunkSize) % chunksPerYield === 0) await new Promise((resolve) => setTimeout(resolve, 0));
      }
      return btoa(binary);
    }

    // Anything the native import pipeline can decode (see sp404::importAudioToWav): wav/aiff/
    // flac natively, +mp3/mp4/m4a/aac via CoreAudioFormat on macOS. The native side always
    // resamples/re-encodes to the SP-404SX's own rate regardless of source, so this is purely a
    // client-side pre-filter to reject obviously-wrong drops early with a clear message.
    const kSupportedAudioExtensions = /\.(wav|aif|aiff|flac|mp3|mp4|m4a)$/i;

    function normalizeFileUri(path) {
      if (!path.startsWith("file://")) return path;
      try {
        return decodeURIComponent(path.replace(/^file:\/\//, ""));
      } catch (e) {
        return path; // malformed URI, leave as-is (won't match the .wav check below anyway)
      }
    }

    // Recursively hunts a parsed JSON drag payload for a string field that looks like an absolute
    // path or file:// URI ending in .wav -- e.g. Ableton's Splice browser panel drops
    // {"fileName":"foo.wav","assetId":"<uuid>",...} as "text/plain" (confirmed: no real File, no
    // "text/uri-list" either), and "fileName" alone is just a bare name with no directory. We
    // don't know Splice's full schema (this was seen truncated), so this checks every string
    // field rather than one hardcoded key -- if some other field (e.g. a local cache path) is
    // present, this picks it up without needing to special-case Splice's exact JSON shape.
    function findPathLikeField(value, depth) {
      depth = depth || 0;
      if (depth > 3 || value == null) return null;
      if (typeof value === "string")
        return kSupportedAudioExtensions.test(value) && (value.startsWith("/") || value.startsWith("file://"))
          ? value
          : null;
      if (Array.isArray(value)) {
        for (const item of value) {
          const found = findPathLikeField(item, depth + 1);
          if (found) return found;
        }
        return null;
      }
      if (typeof value === "object") {
        for (const key of Object.keys(value)) {
          const found = findPathLikeField(value[key], depth + 1);
          if (found) return found;
        }
      }
      return null;
    }

    // Some drag sources (confirmed with Ableton's Splice browser panel) don't put a real OS file
    // into the HTML5 drag session -- dataTransfer.files stays empty, only "text/plain" is offered
    // (no "text/uri-list" either). If that text is itself, or contains, a filesystem path or a
    // file:// URI (plausible: Splice caches previews locally, so a real file likely exists even
    // if it isn't handed over as a proper File), we can still use it -- just have native code
    // read it directly from disk instead of going through the browser's File API. Returns null if
    // nothing path-like is found (e.g. the text is just a sample title/label with no location
    // info -- which is what Splice's payload turned out to be, see README limitations).
    function extractDroppedFilePath(dataTransfer) {
      const candidates = [dataTransfer.getData("text/uri-list"), dataTransfer.getData("text/plain")];
      for (const raw of candidates) {
        if (!raw) continue;

        try {
          const parsed = JSON.parse(raw);
          const pathField = findPathLikeField(parsed);
          if (pathField) return normalizeFileUri(pathField);
        } catch (e) {
          // Not JSON -- fall through to treating it as a plain path/URI below.
        }

        const line = raw
          .split("\n")
          .map((s) => s.trim())
          .find((s) => s && !s.startsWith("#"));
        if (!line) continue;
        const path = normalizeFileUri(line);
        if (kSupportedAudioExtensions.test(path)) return path;
      }
      return null;
    }

    // Drag a .wav onto a pad to replace its sample -- raw byte copy, no re-encoding (see
    // sp404::replacePadSample). Writes immediately to the real SD card, same "no save step, no
    // undo" rule as wirePadControls above. Only .wav is accepted for now (see README
    // limitations). Two paths depending on what the drag actually offers (see
    // extractDroppedFilePath above): a real File (e.g. dragged from the Finder) is read in JS and
    // sent as bytes; a path-only drag (e.g. from Splice) is read directly by native code instead.
    function wirePadDragDrop() {
      const swapPadSample = window.getNativeFunction("swapPadSample");
      const swapPadSampleFromPath = window.getNativeFunction("swapPadSampleFromPath");
      const listPads = window.getNativeFunction("listPads");

      for (const el of padsEl.querySelectorAll(".pad")) {
        const bank = el.dataset.bank;
        const indexInBank = Number(el.dataset.indexInBank);

        el.addEventListener("dragover", (e) => {
          e.preventDefault(); // required for "drop" to fire at all
          if (!el.classList.contains("drag-over")) el.classList.add("drag-over");
        });
        el.addEventListener("dragleave", () => el.classList.remove("drag-over"));
        el.addEventListener("drop", (e) => {
          e.preventDefault();
          el.classList.remove("drag-over");

          const file = e.dataTransfer.files && e.dataTransfer.files[0];
          const droppedPath = !file ? extractDroppedFilePath(e.dataTransfer) : null;

          if (file && !kSupportedAudioExtensions.test(file.name)) {
            statusEl.textContent = "Unsupported file type -- .wav/.aif/.flac/.mp3/.mp4/.m4a are supported for sample swap.";
            return;
          }
          if (!file && !droppedPath) {
            const types = e.dataTransfer && e.dataTransfer.types ? Array.from(e.dataTransfer.types).join(", ") : "";
            const text = (e.dataTransfer.getData("text/plain") || "").slice(0, 600);
            statusEl.textContent =
              `No usable .wav in this drop (dataTransfer types: ${types || "none"}` +
              (text ? `, text: "${text}"` : "") +
              `) -- this drag source may not expose a real file or path.`;
            return;
          }

          const myToken = ++requestToken;
          setLoading(true);
          const startedAt = performance.now();
          const label = file ? `${(file.size / (1024 * 1024)).toFixed(1)} MB` : droppedPath;
          statusEl.textContent = `Swapping sample on ${bank}${indexInBank} (${label})…`;

          const swapPromise = file
            ? file
                .arrayBuffer()
                .then((buffer) => arrayBufferToBase64(buffer))
                .then((base64) => swapPadSample(bank, indexInBank, base64))
            : swapPadSampleFromPath(bank, indexInBank, droppedPath);

          swapPromise
            .then((result) => {
              if (myToken !== requestToken) return; // a newer bank/action superseded this swap
              if (!result || !result.ok) {
                statusEl.textContent = `Failed to swap sample on ${bank}${indexInBank}.`;
                return;
              }
              const elapsedS = ((performance.now() - startedAt) / 1000).toFixed(1);
              pollConnection(); // a swap deletes the old sample file -- free space shouldn't wait for the next 2s poll
              return listPads(bank).then((data) => {
                if (myToken !== requestToken) return;
                renderPads(bank, data);
                statusEl.textContent = `Swapped ${bank}${indexInBank} (${label} in ${elapsedS}s).`;
              });
            })
            .catch((err) => {
              if (myToken !== requestToken) return;
              statusEl.textContent = "Sample swap failed: " + err;
            })
            .finally(() => {
              if (myToken === requestToken) setLoading(false);
            });
        });
      }
    }

    // Polls which pads of the currently-displayed (== currently-armed, see selectBank) bank are
    // actively sounding, and toggles a glowing ".active" outline on them. This is what makes
    // gate/loop actually visible -- e.g. releasing a gate pad should make its glow disappear
    // almost immediately, whereas a non-gate pad keeps glowing until its sample finishes.
    let lastActiveCount = 0;
    let lastClipping = false;

    // Screen's inner glow (see .screen-glow) is blue while something is actually sounding, red
    // while clipping, and OFF the rest of the time -- never a permanent ambient glow.
    function updateScreenGlowState() {
      screenEl.classList.toggle("sound-active", lastActiveCount > 0 && !lastClipping);
      screenEl.classList.toggle("clipping", lastClipping);
    }

    function pollActivePads() {
      if (dragInProgress) return;
      const getActivePads = window.getNativeFunction("getActivePads");
      getActivePads()
        .then((indices) => {
          const activeSet = new Set(indices || []);
          for (const el of padsEl.querySelectorAll(".pad"))
            el.classList.toggle("active", activeSet.has(Number(el.dataset.padIndex)));
          lastActiveCount = activeSet.size;
          updateScreenGlowState();
        })
        .catch(() => {});
    }

    // Output clipped in roughly the last 500ms (see PluginProcessor::isClipping) -- an
    // at-a-glance overload indicator, folded into the same glow as pollActivePads above.
    function pollClipping() {
      if (dragInProgress) return;
      const getClipping = window.getNativeFunction("getClipping");
      getClipping()
        .then((clipping) => {
          lastClipping = !!clipping;
          updateScreenGlowState();
        })
        .catch(() => {});
    }

    function formatBytes(bytes) {
      if (typeof bytes !== "number" || !isFinite(bytes)) return null;
      const gb = bytes / 1024 ** 3;
      return gb >= 1 ? `${gb.toFixed(1)} GB` : `${(bytes / 1024 ** 2).toFixed(0)} MB`;
    }

    // Hovering the SD icon (native title tooltip) shows connection state + capacity/free space.
    // While offline the icon/tooltip describe the local mirror instead -- the physical card's
    // connection state is irrelevant to whether the plugin is usable in that mode.
    function sdTooltipText(data) {
      if (sdOfflineMode) return "Offline: using the local mirror, not the SD card";
      if (!data.connected) return "No SD card connected";
      const free = formatBytes(data.freeBytes);
      const total = formatBytes(data.totalBytes);
      return free && total ? `SD card connected — ${free} free of ${total}` : "SD card connected";
    }

    // Swaps the header icon between the SD card glyph (live mode) and a hard-drive glyph
    // (offline mode, editing the local mirror rather than a physical card) -- set by
    // refreshSyncModeUi() in wireBankMenu(), read here and by applyCardConnectionState().
    let sdOfflineMode = false;
    const kSdCardIconSvg =
      '<svg viewBox="0 0 24 24"><path d="M16 2H8a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2V7l-5-5zM9 5h2v3H9V5zm3 0h2v3h-2V5zm4 14H8v-6h8v6z"/></svg>';
    const kHardDriveIconSvg =
      '<svg viewBox="0 0 24 24"><path d="M2 20h20v-4H2v4zm2-3h2v2H4v-2zM2 4v4h20V4H2zm4 3H4V5h2v2zm-4 7h20v-4H2v4zm2-3h2v2H4v-2z"/></svg>';
    function updateSdIndicatorIcon() {
      sdIndicatorEl.innerHTML = sdOfflineMode ? kHardDriveIconSvg : kSdCardIconSvg;
    }

    // Shared by pollConnection() and the initial listBanks() call below: while offline the
    // indicator reads as "connected" regardless of the physical card (the mirror is what
    // matters), otherwise it reflects the real card's presence as before.
    function applyCardConnectionState(data) {
      sdIndicatorEl.classList.toggle("connected", sdOfflineMode ? true : !!data.connected);
      sdIndicatorEl.title = sdTooltipText(data);
    }

    // Green when a card is mounted, grey otherwise. Polls independently of the initial
    // listBanks() call (and doesn't touch #banks) so a card inserted/ejected while the plugin
    // stays open is reflected without disturbing the user's current bank/shift selection.
    function pollConnection() {
      if (dragInProgress) return;
      const listBanks = window.getNativeFunction("listBanks");
      listBanks()
        .then((data) => {
          applyCardConnectionState(data);
        })
        .catch(() => {});
    }

    // --- Knobs: drag to change value (0-127), right-click to MIDI-learn a CC ------------------
    let knobDrag = null; // { index, startY, startValue } while a drag is in progress
    const knobValues = [64, 64, 64, 64]; // local cache, refreshed by pollKnobs()
    const kKnobDragPixelsForFullRange = 150;

    // 7 pre-rendered rotation frames (knob0.png..knob6.png) rather than one image spun with a
    // CSS transform: the artwork has baked-in perspective/shading for a specific orientation, so
    // a plain 2D rotate() looks wrong away from that one angle. Swapping frames keeps every
    // position looking like a real photographed knob at that rotation.
    //
    // The source sprite sheet only draws one half of the sweep (knob0.png = 50%, ..., knob6.png
    // = 0%, confirmed against the artwork) -- there is no artwork for 50%-100%. The icon set is
    // left/right symmetric (knob0 and knob6 are mirror images of each other), so the 50%-100%
    // half is synthesized by horizontally flipping the same 7 frames rather than by resampling
    // or interpolating new artwork.
    const kKnobFrameTable = [
      { percent: 0, file: "knob6.png", mirror: false },
      { percent: 5, file: "knob5.png", mirror: false },
      { percent: 10, file: "knob4.png", mirror: false },
      { percent: 15, file: "knob3.png", mirror: false },
      { percent: 35, file: "knob2.png", mirror: false },
      { percent: 40, file: "knob1.png", mirror: false },
      { percent: 50, file: "knob0.png", mirror: false },
      { percent: 60, file: "knob1.png", mirror: true },
      { percent: 65, file: "knob2.png", mirror: true },
      { percent: 85, file: "knob3.png", mirror: true },
      { percent: 90, file: "knob4.png", mirror: true },
      { percent: 95, file: "knob5.png", mirror: true },
      { percent: 100, file: "knob6.png", mirror: true },
    ];

    function knobFrameForValue(value) {
      const percent = (value / 127) * 100;
      let closest = kKnobFrameTable[0];
      let closestDist = Infinity;
      for (const entry of kKnobFrameTable) {
        const dist = Math.abs(entry.percent - percent);
        if (dist < closestDist) {
          closestDist = dist;
          closest = entry;
        }
      }
      return closest;
    }

    function applyKnobVisual(wrapEl, state) {
      // Both representations are kept in sync on every call, regardless of which one the active
      // theme actually shows (see [data-theme] rules in styles.css) -- so switching themes never
      // needs a re-poll to look right.
      const img = wrapEl.querySelector(".knob-sprite");
      const frame = knobFrameForValue(state.value);
      if (!img.src.endsWith(frame.file)) img.src = frame.file;
      img.classList.toggle("knob-mirrored", frame.mirror);

      // Same -135deg..+135deg sweep as the tick ring (kKnobTickSweepDeg, defined below but
      // already initialized by the time this ever runs -- see wireKnobs) so the procedural
      // indicator always lines up with the ticks around it.
      const svg = wrapEl.querySelector(".knob-procedural");
      const angleDeg = -kKnobTickSweepDeg / 2 + (state.value / 127) * kKnobTickSweepDeg;
      svg.style.setProperty("--knob-angle", `${angleDeg}deg`);

      wrapEl.classList.toggle("learning", !!state.learning);
      wrapEl.classList.toggle("mapped", state.cc >= 0);
      wrapEl.title = state.learning
        ? "Waiting for a MIDI CC…"
        : state.cc >= 0
          ? `CC ${state.cc} — right-click to re-map`
          : "Drag to adjust, right-click to MIDI-learn";
    }

    // Graduated tick marks around a knob-dial, like a real hardware control -- a fixed sweep
    // (-135deg to +135deg), evenly spaced, independent of the knob's current value. Built as
    // individual <div>s placed with rotate()+translateY() (see .knob-tick in styles.css) rather
    // than a masked CSS ring, so they unambiguously sit outside the knob artwork.
    const kKnobTickCount = 11;
    const kKnobTickSweepDeg = 270;

    function buildKnobTicks() {
      for (const dialEl of document.querySelectorAll(".knob-dial")) {
        for (let i = 0; i < kKnobTickCount; i++) {
          const angle = -kKnobTickSweepDeg / 2 + (i * kKnobTickSweepDeg) / (kKnobTickCount - 1);
          const tick = document.createElement("div");
          tick.className = "knob-tick";
          tick.style.setProperty("--tick-angle", `${angle}deg`);
          dialEl.appendChild(tick);
        }
      }
    }

    function wireKnobs() {
      buildKnobTicks();
      const setKnobValue = window.getNativeFunction("setKnobValue");
      const startKnobLearn = window.getNativeFunction("startKnobLearn");

      document.addEventListener("mousemove", (e) => {
        if (!knobDrag) return;
        const deltaY = knobDrag.startY - e.clientY; // dragging up increases the value
        const value = Math.max(
          0,
          Math.min(127, Math.round(knobDrag.startValue + (deltaY * 127) / kKnobDragPixelsForFullRange))
        );
        knobValues[knobDrag.index] = value;
        setKnobValue(knobDrag.index, value);
        const wrapEl = document.querySelector(`.knob-wrap[data-knob-index="${knobDrag.index}"]`);
        if (wrapEl) applyKnobVisual(wrapEl, { value, cc: wrapEl.classList.contains("mapped") ? 0 : -1, learning: false });
      });
      document.addEventListener("mouseup", () => {
        knobDrag = null;
      });

      for (const wrapEl of document.querySelectorAll(".knob-wrap")) {
        const index = Number(wrapEl.dataset.knobIndex);
        // Bound to .knob-dial (the container), not .knob-sprite/.knob-procedural specifically --
        // whichever representation the active theme shows is the only one actually visible/
        // hit-testable anyway, so this works correctly no matter which theme is active.
        const dial = wrapEl.querySelector(".knob-dial");

        dial.addEventListener("mousedown", (e) => {
          e.preventDefault();
          knobDrag = { index, startY: e.clientY, startValue: knobValues[index] };
        });
        dial.addEventListener("contextmenu", (e) => {
          e.preventDefault();
          startKnobLearn(index);
        });
      }
    }

    function pollKnobs() {
      if (dragInProgress) return;
      const getKnobStates = window.getNativeFunction("getKnobStates");
      getKnobStates()
        .then((states) => {
          (states || []).forEach((state, index) => {
            knobValues[index] = state.value;
            if (knobDrag && knobDrag.index === index) return; // don't fight an in-progress drag
            const wrapEl = document.querySelector(`.knob-wrap[data-knob-index="${index}"]`);
            if (wrapEl) applyKnobVisual(wrapEl, state);
          });
        })
        .catch(() => {});
    }

    function selectBank(bankName, el) {
      for (const other of banksEl.querySelectorAll(".bank.selected")) other.classList.remove("selected");
      el.classList.add("selected");
      screenReadoutEl.textContent = bankName;
      screenReadoutLabelEl.textContent = "bank";

      // Arms this bank for MIDI playback (notes 36-47 / C1-B1 trigger its pads) -- fire-and-forget,
      // independent of the listPads() call below which is only for the on-screen pad detail.
      window.getNativeFunction("selectBank")(bankName);

      const myToken = ++requestToken;
      padsEl.innerHTML = `<div class="pad-loading">Loading pads…</div>`;
      void padsEl.offsetHeight;
      setLoading(true);

      const listPads = window.getNativeFunction("listPads");
      listPads(bankName)
        .then((data) => {
          if (myToken !== requestToken) return; // a newer bank was selected; drop this stale response
          renderPads(bankName, data);
        })
        .catch((err) => {
          if (myToken !== requestToken) return;
          statusEl.textContent = "listPads() failed: " + err;
        })
        .finally(() => {
          if (myToken === requestToken) setLoading(false);
        });
    }

    // Generic confirmation modal for destructive actions (clear/replace) -- resolves true/false.
    // In-WebView rather than a native dialog, to stay visually consistent with the rest of the
    // hardware-styled panel (see plan notes).
    function showConfirm(message) {
      return new Promise((resolve) => {
        const overlay = document.getElementById("confirm-modal");
        document.getElementById("confirm-message").textContent = message;
        const cancelBtn = document.getElementById("confirm-cancel-btn");
        const okBtn = document.getElementById("confirm-ok-btn");

        const finish = (result) => {
          overlay.classList.add("hidden");
          cancelBtn.removeEventListener("click", onCancel);
          okBtn.removeEventListener("click", onOk);
          resolve(result);
        };
        const onCancel = () => finish(false);
        const onOk = () => finish(true);
        cancelBtn.addEventListener("click", onCancel);
        okBtn.addEventListener("click", onOk);

        overlay.classList.remove("hidden");
      });
    }

    // Lets the user pick a destination bank (A-J) when restoring a single-bank backup -- may
    // differ from the bank it was saved from (see sp404::loadBankFromZip, which handles the
    // sample-file renaming this implies). Resolves the chosen letter, or null if cancelled.
    function pickBankTarget(defaultBank) {
      return new Promise((resolve) => {
        const overlay = document.getElementById("bank-target-modal");
        const lettersEl = document.getElementById("bank-target-letters");
        const cancelBtn = document.getElementById("bank-target-cancel-btn");
        lettersEl.innerHTML = "";

        const finish = (letter) => {
          overlay.classList.add("hidden");
          cancelBtn.removeEventListener("click", onCancel);
          resolve(letter);
        };
        const onCancel = () => finish(null);
        cancelBtn.addEventListener("click", onCancel);

        for (let code = 65; code <= 74; code++) {
          const letter = String.fromCharCode(code);
          const btn = document.createElement("button");
          btn.type = "button";
          btn.textContent = letter;
          if (letter === defaultBank) btn.style.borderColor = "var(--orange)";
          btn.addEventListener("click", () => finish(letter));
          lettersEl.appendChild(btn);
        }

        overlay.classList.remove("hidden");
      });
    }

    // --- Themes --------------------------------------------------------------------------------
    // A theme is just a [data-theme="id"] block of CSS custom-property overrides in themes.css --
    // applying one is nothing more than setting the attribute (plus, for the 2 sticker themes,
    // populating #sticker-layer). swatchColor here is only for the picker UI, it doesn't need to
    // match themes.css exactly (it's a rough single-color preview, themes.css is the real source
    // of truth for the full palette).
    const kThemes = [
      { id: "hardware", name: "Hardware", swatchColor: "#ff6a13" },
      { id: "neon-cyber", name: "Neon Cyber", swatchColor: "#00eaff" },
      { id: "vaporwave", name: "Vaporwave", swatchColor: "#ff71ce" },
      { id: "lofi-tape", name: "Lo-Fi Tape", swatchColor: "#c9702a" },
      { id: "kawaii", name: "Kawaii Stickers", swatchColor: "#ff6fa5" },
      { id: "japandi", name: "Japandi Zen", swatchColor: "#7c9473" },
      { id: "midnight-studio", name: "Midnight Studio", swatchColor: "#3d8bff" },
      { id: "sunset-funk", name: "Sunset Funk", swatchColor: "#ffb703" },
      { id: "mono-terminal", name: "Mono Terminal", swatchColor: "#33ff66" },
      { id: "pastel-dreams", name: "Pastel Dreams", swatchColor: "#ff9ecb" },
    ];

    // Fixed (not randomized -- stays identical across reloads/re-renders) scattered positions for
    // the 2 sticker themes, clustered around the header and the bottom status area since that's
    // where this compact layout actually has open background to show them against; everywhere
    // else is covered edge-to-edge by pads/knobs/banks. See plugin/web/stickers/ + the cutout
    // script used to detour these from user-provided sticker sheets (see README).
    const kStickerLayouts = {
      kawaii: [
        { file: "kawaii1.png", top: "1%", left: "3%", size: 42, rotate: -14 },
        { file: "kawaii2.png", top: "6%", left: "13%", size: 34, rotate: 10 },
        { file: "kawaii3.png", top: "2%", left: "80%", size: 40, rotate: 12 },
        { file: "kawaii4.png", top: "7%", left: "90%", size: 32, rotate: -8 },
        { file: "kawaii5.png", top: "93%", left: "4%", size: 38, rotate: 8 },
        { file: "kawaii6.png", top: "90%", left: "15%", size: 30, rotate: -16 },
        { file: "kawaii7.png", top: "94%", left: "82%", size: 36, rotate: -10 },
        { file: "kawaii8.png", top: "89%", left: "92%", size: 30, rotate: 14 },
      ],
      japandi: [
        { file: "jp1.png", top: "1%", left: "4%", size: 46, rotate: -6 },
        { file: "jp2.png", top: "6%", left: "14%", size: 30, rotate: 8 },
        { file: "jp3.png", top: "2%", left: "81%", size: 38, rotate: 6 },
        { file: "jp4.png", top: "7%", left: "91%", size: 32, rotate: -10 },
        { file: "jp5.png", top: "93%", left: "5%", size: 34, rotate: 6 },
        { file: "jp6.png", top: "89%", left: "16%", size: 28, rotate: -8 },
        { file: "jp7.png", top: "94%", left: "83%", size: 32, rotate: -6 },
        { file: "jp8.png", top: "89%", left: "93%", size: 28, rotate: 10 },
      ],
    };

    function renderStickerLayer(themeId) {
      const layer = document.getElementById("sticker-layer");
      layer.innerHTML = "";
      const layout = kStickerLayouts[themeId];
      if (!layout) return;
      for (const s of layout) {
        const img = document.createElement("img");
        img.src = s.file; // served flat by provideResource(), like every other image asset
        img.alt = "";
        img.style.top = s.top;
        img.style.left = s.left;
        img.style.width = `${s.size}px`;
        img.style.transform = `rotate(${s.rotate}deg)`;
        layer.appendChild(img);
      }
    }

    function applyTheme(themeId) {
      document.documentElement.dataset.theme = themeId;
      renderStickerLayer(themeId);
      const grid = document.getElementById("theme-grid");
      if (grid)
        for (const swatch of grid.querySelectorAll(".theme-swatch"))
          swatch.classList.toggle("active", swatch.dataset.themeId === themeId);
    }

    function openThemePicker() {
      document.getElementById("theme-modal").classList.remove("hidden");
    }

    function wireThemePicker() {
      const grid = document.getElementById("theme-grid");
      grid.innerHTML = "";
      for (const theme of kThemes) {
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "theme-swatch";
        btn.dataset.themeId = theme.id;
        btn.innerHTML = `<span class="theme-swatch-dot" style="--swatch-color:${theme.swatchColor}"></span>${theme.name}`;
        btn.addEventListener("click", () => {
          applyTheme(theme.id);
          window.getNativeFunction("setTheme")(theme.id);
        });
        grid.appendChild(btn);
      }

      document
        .getElementById("theme-close-btn")
        .addEventListener("click", () => document.getElementById("theme-modal").classList.add("hidden"));
    }

    // Loads the saved theme (native prefs, see sp404::getTheme) once at startup, before the pads/
    // knobs poll loop starts -- so the UI never flashes the Hardware default first.
    async function loadInitialTheme() {
      const result = await window.getNativeFunction("getTheme")();
      applyTheme((result && result.theme) || "hardware");
    }

    // Bank-management menu (top-left): save/load/clear the whole card or a single bank. Every
    // destructive action (clear/replace) is gated by showConfirm first -- none of this has an
    // undo, same rule as every other write path in this app.
    function wireBankMenu() {
      const menuBtn = document.getElementById("menu-btn");
      const dropdown = document.getElementById("menu-dropdown");
      menuBtn.addEventListener("click", (e) => {
        e.stopPropagation();
        dropdown.classList.toggle("open");
      });
      document.addEventListener("click", () => dropdown.classList.remove("open"));

      function currentBank() {
        const letter = screenReadoutEl.textContent;
        return /^[A-J]$/.test(letter) ? letter : null;
      }

      // Re-syncs playback + the on-screen pad grid after a native operation changed files on disk
      // out from under BankLoader -- same refresh pattern already used after a drag&drop swap
      // (see wirePadDragDrop): re-invoke selectBank/listPads rather than adding a new mechanism.
      function refreshAfterFileChange(bank) {
        pollConnection(); // SD free-space readout shouldn't wait for the next 2s poll
        if (!bank) return;
        window.getNativeFunction("selectBank")(bank);
        if (bank === currentBank())
          window.getNativeFunction("listPads")(bank).then((data) => renderPads(bank, data));
      }

      const actions = {
        "save-all": async () => {
          const picked = await window.getNativeFunction("pickZipToSave")("SP404_AllBanks.zip");
          if (picked.cancelled) return;
          const result = await window.getNativeFunction("saveAllBanksToZip")(picked.path);
          statusEl.textContent =
            result && result.ok ? `Saved all banks to ${picked.path}.` : "Failed to save all banks.";
        },
        "load-all": async () => {
          const picked = await window.getNativeFunction("pickZipToOpen")();
          if (picked.cancelled) return;
          const confirmed = await showConfirm(
            "This replaces every bank on the card with the backup's contents. This cannot be undone. Continue?"
          );
          if (!confirmed) return;
          const result = await window.getNativeFunction("loadAllBanksFromZip")(picked.path);
          statusEl.textContent = result && result.ok ? "All banks restored." : "Failed to load the backup.";
          if (result && result.ok) refreshAfterFileChange(currentBank());
        },
        "save-bank": async () => {
          const bank = currentBank();
          if (!bank) {
            statusEl.textContent = "Select a bank first.";
            return;
          }
          const picked = await window.getNativeFunction("pickZipToSave")(`SP404_Bank_${bank}.zip`);
          if (picked.cancelled) return;
          const result = await window.getNativeFunction("saveBankToZip")(bank, picked.path);
          statusEl.textContent =
            result && result.ok ? `Saved bank ${bank} to ${picked.path}.` : `Failed to save bank ${bank}.`;
        },
        "load-bank": async () => {
          const picked = await window.getNativeFunction("pickZipToOpen")();
          if (picked.cancelled) return;
          const info = await window.getNativeFunction("peekBankZip")(picked.path);
          if (!info || !info.valid) {
            statusEl.textContent = "Not a single-bank backup (use Load All Banks for a full-card backup).";
            return;
          }
          const target = await pickBankTarget(info.savedFromBank);
          if (!target) return;
          const confirmed = await showConfirm(
            `This replaces all 12 pads of bank ${target} with bank ${info.savedFromBank}'s backup. This cannot be undone. Continue?`
          );
          if (!confirmed) return;
          const result = await window.getNativeFunction("loadBankFromZip")(picked.path, target);
          statusEl.textContent =
            result && result.ok
              ? `Bank ${target} restored from bank ${info.savedFromBank}'s backup.`
              : `Failed to load into bank ${target}.`;
          if (result && result.ok) refreshAfterFileChange(target);
        },
        "clear-bank": async () => {
          const bank = currentBank();
          if (!bank) {
            statusEl.textContent = "Select a bank first.";
            return;
          }
          const confirmed = await showConfirm(`This deletes all 12 samples in bank ${bank}. This cannot be undone. Continue?`);
          if (!confirmed) return;
          const result = await window.getNativeFunction("clearBank")(bank);
          statusEl.textContent = result && result.ok ? `Bank ${bank} cleared.` : `Failed to clear bank ${bank}.`;
          if (result && result.ok) refreshAfterFileChange(bank);
        },
        "clear-all": async () => {
          const confirmed = await showConfirm(
            "This deletes every sample on the card (all 10 banks, 120 pads). This cannot be undone. Continue?"
          );
          if (!confirmed) return;
          const result = await window.getNativeFunction("clearAllBanks")();
          statusEl.textContent = result && result.ok ? "All banks cleared." : "Failed to clear all banks.";
          if (result && result.ok) refreshAfterFileChange(currentBank());
        },
        "toggle-sync-mode": async () => {
          const mode = await window.getNativeFunction("getSyncMode")();
          if (mode && mode.offline) {
            await window.getNativeFunction("exitOfflineMode")();
            statusEl.textContent = "Back to live mode -- editing the connected SD card directly.";
          } else {
            const result = await window.getNativeFunction("enterOfflineMode")();
            statusEl.textContent =
              result && result.ok
                ? "Offline mode -- editing a local mirror. Use \"Sync Mirror → Card\" when ready."
                : "Couldn't start offline mode (connect a card first to create the initial mirror).";
          }
          await refreshSyncModeUi();
          refreshAfterFileChange(currentBank());
        },
        "sync-to-card": async () => {
          const confirmed = await showConfirm(
            "This overwrites the connected SD card with the offline mirror's contents. This cannot be undone. Continue?"
          );
          if (!confirmed) return;
          const result = await window.getNativeFunction("syncMirrorToCard")();
          statusEl.textContent =
            result && result.ok ? "Mirror synced to the connected card." : "Sync failed (is a card connected?).";
        },
        "open-theme-picker": () => openThemePicker(),
      };

      for (const btn of dropdown.querySelectorAll("button[data-action]")) {
        btn.addEventListener("click", () => {
          dropdown.classList.remove("open");
          const action = actions[btn.dataset.action];
          if (action) action();
        });
      }

      // Reflects current live/offline state in the badge + menu labels -- called on load and
      // after any action that might change the mode.
      async function refreshSyncModeUi() {
        const mode = await window.getNativeFunction("getSyncMode")();
        if (!mode) return;
        const badge = document.getElementById("mode-badge");
        badge.textContent = mode.offline ? "OFFLINE" : "LIVE";
        badge.classList.toggle("offline", mode.offline);
        badge.title = mode.offline
          ? "Offline: editing a local mirror, not the real card -- use the menu to sync"
          : "Live: editing the connected SD card directly";

        const toggleBtn = document.getElementById("toggle-sync-mode-btn");
        toggleBtn.textContent = mode.offline ? "Go Live" : "Go Offline…";

        const syncBtn = document.getElementById("sync-to-card-btn");
        syncBtn.disabled = !mode.offline;
        syncBtn.style.display = mode.offline ? "" : "none";

        sdOfflineMode = mode.offline;
        updateSdIndicatorIcon();
        pollConnection(); // refreshes the indicator's color/tooltip against the new mode right away
      }
      refreshSyncModeUi();
    }

    // Per-pad DSP panel (normalize/mono-stereo/fade/trim/BPM/pitch/time-stretch) -- one modal
    // reused across pads (opened via the .dsp-open-btn wired in wirePadControls) rather than a
    // separate panel instance per pad. Each action writes immediately to the sample, same "no
    // undo" rule as every other pad edit -- see plugin/SampleDsp.h for what each one actually
    // does to the underlying file (all of them re-import the result, so trim/loop/gate/reverse/
    // lofi/volume follow the same "trim resets, playback settings preserved" contract as a
    // drag&drop swap).
    let dspTarget = null; // {bank, indexInBank} for whichever pad's panel is currently open

    const kSilenceEpsilonSeconds = 0.005; // below this, edge "silence" is just float/measurement noise
    const formatMs = (seconds) => `${Math.round(seconds * 1000)}ms`;
    const formatDb = (db) => `${db >= 0 ? "+" : ""}${db.toFixed(1)} dB`;

    // Sets a feedback <span> next to a DSP action button: green ("good") for an actual change,
    // dim grey for "nothing to do" (already normalized/already that channel count/no silence) --
    // same information either way ({ok: true}), but visually distinct so a no-op doesn't read as
    // a mistake or a silent failure.
    function setDspFeedback(elId, text, good) {
      const el = document.getElementById(elId);
      el.textContent = text;
      el.classList.toggle("good", !!good);
    }

    // Runs a DSP action while a request for the same target is in flight: disables every DSP
    // button (prevents double-submission from an impatient double-click, which would otherwise
    // race two writes to the same file) and re-enables them all when it settles either way.
    async function runDspAction(fn) {
      const buttons = document.querySelectorAll("#dsp-modal button:not(#dsp-close-btn)");
      buttons.forEach((b) => (b.disabled = true));
      try {
        await fn();
      } finally {
        buttons.forEach((b) => (b.disabled = false));
        updateMonoStereoActiveState(); // re-disabling above clears the "current format" greyed-out state too
      }
    }

    // Highlights whichever of Mono/Stereo matches the pad's actual channel count right now, and
    // disables it (converting a pad to the format it's already in is a pointless round-trip write).
    function updateMonoStereoActiveState(channels) {
      const monoBtn = document.getElementById("dsp-mono-btn");
      const stereoBtn = document.getElementById("dsp-stereo-btn");
      if (channels === undefined) channels = updateMonoStereoActiveState.lastChannels;
      updateMonoStereoActiveState.lastChannels = channels;
      monoBtn.classList.toggle("active", channels === 1);
      monoBtn.disabled = channels === 1;
      stereoBtn.classList.toggle("active", channels === 2);
      stereoBtn.disabled = channels === 2;
    }

    // Read-only refresh of the "what's true right now" status line + Mono/Stereo highlighting --
    // called on open and after every action that could change peak/channels/silence.
    async function refreshDspStatus() {
      if (!dspTarget) return;
      const statusResultEl = document.getElementById("dsp-status");
      const result = await window.getNativeFunction("getPadDspStatus")(dspTarget.bank, dspTarget.indexInBank);
      if (!dspTarget || !result || !result.ok) {
        statusResultEl.textContent = "";
        updateMonoStereoActiveState(undefined);
        return;
      }

      updateMonoStereoActiveState(result.channels);

      const channelsLabel = result.channels === 2 ? "Stereo" : "Mono";
      if (result.isSilent) {
        statusResultEl.textContent = `${channelsLabel} (${result.channels}ch) · silent`;
        return;
      }
      statusResultEl.textContent =
        `${channelsLabel} (${result.channels}ch) · peak ${formatDb(result.peakDb)} · ` +
        `${formatMs(result.leadingSilenceSeconds)} lead / ${formatMs(result.trailingSilenceSeconds)} trail silence`;
    }

    function openDspPanel(bank, indexInBank) {
      dspTarget = { bank, indexInBank };
      document.getElementById("dsp-modal-title").textContent = `DSP — ${bank}${indexInBank}`;
      setDspFeedback("dsp-normalize-result", "", false);
      setDspFeedback("dsp-trim-result", "", false);
      document.getElementById("dsp-bpm-result").textContent = "";
      document.getElementById("dsp-fadein-input").value = "0";
      document.getElementById("dsp-fadeout-input").value = "0";
      document.getElementById("dsp-pitch-input").value = "0";
      document.getElementById("dsp-stretch-input").value = "1";
      document.getElementById("dsp-modal").classList.remove("hidden");

      refreshDspStatus();

      // Prefills the BPM readout with a previously-detected/saved value (see handleDetectBpm)
      // instead of leaving it blank until the user re-runs detection every time they reopen the
      // panel for the same pad.
      window.getNativeFunction("listPads")(bank).then((data) => {
        if (!dspTarget || dspTarget.bank !== bank || dspTarget.indexInBank !== indexInBank) return;
        const pad = data.pads && data.pads[indexInBank - 1];
        if (pad && pad.tempoMode === "user")
          document.getElementById("dsp-bpm-result").textContent = `${pad.userBpm.toFixed(1)} BPM (saved)`;
      });
    }

    function closeDspPanel() {
      document.getElementById("dsp-modal").classList.add("hidden");
      dspTarget = null;
    }

    // Same refresh pattern used everywhere else in this app after a native write: re-fetch
    // listPads for the affected bank and re-render; a DSP edit changes the sample file's size
    // too, so also nudge the SD free-space readout instead of waiting for the next 2s poll.
    async function refreshPadAfterDsp() {
      if (!dspTarget) return;
      pollConnection();
      const bank = dspTarget.bank;
      const data = await window.getNativeFunction("listPads")(bank);
      renderPads(bank, data);
      await refreshDspStatus();
    }

    function wireDspPanel() {
      document.getElementById("dsp-close-btn").addEventListener("click", closeDspPanel);

      document.getElementById("dsp-normalize-btn").addEventListener("click", () =>
        runDspAction(async () => {
          if (!dspTarget) return;
          const result = await window.getNativeFunction("normalizePad")(dspTarget.bank, dspTarget.indexInBank);
          if (!result || !result.ok) {
            setDspFeedback("dsp-normalize-result", "Failed (empty or silent pad?).", false);
            return;
          }
          const before = result.peakBeforeDb;
          const after = result.peakAfterDb;
          const changed = before === undefined || after === undefined || Math.abs(after - before) > 0.05;
          setDspFeedback(
            "dsp-normalize-result",
            changed ? `Normalized: ${formatDb(before)} → ${formatDb(after)}.` : `Already normalized (${formatDb(after)}).`,
            changed
          );
          await refreshPadAfterDsp();
        })
      );

      document.getElementById("dsp-mono-btn").addEventListener("click", () =>
        runDspAction(async () => {
          if (!dspTarget) return;
          const result = await window.getNativeFunction("convertPadChannels")(dspTarget.bank, dspTarget.indexInBank, 1);
          statusEl.textContent =
            result && result.ok
              ? result.channelsBefore === 1
                ? "Already mono."
                : "Converted to mono."
              : "Mono conversion failed.";
          if (result && result.ok) await refreshPadAfterDsp();
        })
      );

      document.getElementById("dsp-stereo-btn").addEventListener("click", () =>
        runDspAction(async () => {
          if (!dspTarget) return;
          const result = await window.getNativeFunction("convertPadChannels")(dspTarget.bank, dspTarget.indexInBank, 2);
          statusEl.textContent =
            result && result.ok
              ? result.channelsBefore === 2
                ? "Already stereo."
                : "Converted to stereo."
              : "Stereo conversion failed.";
          if (result && result.ok) await refreshPadAfterDsp();
        })
      );

      document.getElementById("dsp-fade-btn").addEventListener("click", () =>
        runDspAction(async () => {
          if (!dspTarget) return;
          const fadeIn = Number(document.getElementById("dsp-fadein-input").value) || 0;
          const fadeOut = Number(document.getElementById("dsp-fadeout-input").value) || 0;
          if (fadeIn <= 0 && fadeOut <= 0) {
            statusEl.textContent = "Set a fade in and/or fade out duration above zero first.";
            return;
          }
          const result = await window.getNativeFunction("fadePad")(dspTarget.bank, dspTarget.indexInBank, fadeIn, fadeOut);
          statusEl.textContent = result && result.ok ? "Fades applied." : "Fade failed.";
          if (result && result.ok) await refreshPadAfterDsp();
        })
      );

      document.getElementById("dsp-trim-btn").addEventListener("click", () =>
        runDspAction(async () => {
          if (!dspTarget) return;
          const result = await window.getNativeFunction("trimSilencePad")(dspTarget.bank, dspTarget.indexInBank);
          if (!result || !result.ok) {
            const message = !result || !result.hadSample
              ? "No sample on this pad."
              : result.wasSilent
                ? "Pad is silent -- nothing to trim."
                : "Sample is quiet throughout -- nothing to trim to.";
            setDspFeedback("dsp-trim-result", message, false);
            return;
          }
          const lead = result.leadingSilenceSecondsBefore || 0;
          const trail = result.trailingSilenceSecondsBefore || 0;
          const hadSilence = lead > kSilenceEpsilonSeconds || trail > kSilenceEpsilonSeconds;
          setDspFeedback(
            "dsp-trim-result",
            hadSilence ? `Trimmed ${formatMs(lead)} lead / ${formatMs(trail)} trail.` : "No silence found to trim.",
            hadSilence
          );
          await refreshPadAfterDsp();
        })
      );

      document.getElementById("dsp-bpm-btn").addEventListener("click", () =>
        runDspAction(async () => {
          if (!dspTarget) return;
          const resultEl = document.getElementById("dsp-bpm-result");
          resultEl.textContent = "…";
          const result = await window.getNativeFunction("detectBpm")(dspTarget.bank, dspTarget.indexInBank);
          resultEl.textContent =
            result && result.ok
              ? `${result.bpm.toFixed(1)} BPM${result.saved ? " (saved)" : ""}`
              : "No clear tempo found.";
        })
      );

      document.getElementById("dsp-stretch-btn").addEventListener("click", () =>
        runDspAction(async () => {
          if (!dspTarget) return;
          const pitch = Number(document.getElementById("dsp-pitch-input").value) || 0;
          const ratio = Number(document.getElementById("dsp-stretch-input").value) || 1;
          if (pitch === 0 && ratio === 1) {
            statusEl.textContent = "Set a pitch (semitones) and/or time ratio different from the default first.";
            return;
          }
          const result = await window.getNativeFunction("applyPitchTimeStretch")(
            dspTarget.bank,
            dspTarget.indexInBank,
            ratio,
            pitch
          );
          statusEl.textContent = result && result.ok ? "Pitch/time-stretch applied." : "Pitch/time-stretch failed.";
          if (result && result.ok) await refreshPadAfterDsp();
        })
      );
    }

    if (!hasNativeBridge) {
      statusEl.textContent = "window.__JUCE__ is not available (not running inside the plugin's WebView).";
    } else {
      loadInitialTheme(); // fire-and-forget, before anything else -- avoids a flash of Hardware
      setLoading(true);
      const listBanks = window.getNativeFunction("listBanks");
      listBanks()
        .then((data) => {
          // Pair bank i with bank i+5 (A+F, B+G, ...) -- data.banks is built A..J in order by
          // WebUIBridge::listBanks, so this matches the hardware's physical button layout exactly.
          const pairs = data.banks.slice(0, 5).map((bank, i) => [bank.name, data.banks[i + 5].name]);
          banksEl.innerHTML = pairs
            .map(
              ([first, second]) =>
                `<div class="bank" data-letters="${first},${second}">` +
                `<span class="letter">${first}</span>/<span class="letter">${second}</span></div>`
            )
            .join("");
          void banksEl.offsetHeight; // force a layout reflow after this async DOM mutation
          updateBankLetterHighlights();
          let bankAEl = null;
          for (const el of banksEl.querySelectorAll(".bank")) {
            if (el.dataset.letters.split(",")[0] === "A") bankAEl = el;
            el.addEventListener("click", () => {
              const [first, second] = el.dataset.letters.split(",");
              selectBank(shiftActive ? second : first, el);
            });
          }
          // BankLoader defaults to Bank A on startup (see PluginProcessor's constructor), so
          // show its pads immediately instead of leaving an empty pad grid until the user
          // clicks a bank letter themselves.
          if (bankAEl) selectBank("A", bankAEl);

          screenReadoutLabelEl.textContent = data.connected ? "ready" : "no card";
          statusEl.classList.toggle("connected", data.connected);
          applyCardConnectionState(data);
          statusEl.textContent = data.connected
            ? `Native bridge OK — SD card connected, ${data.banks.length} banks. Click a bank to see its pads.`
            : `Native bridge OK — no SD card connected (showing bank shell only).`;
        })
        .catch((err) => {
          statusEl.textContent = "Native call failed: " + err;
        })
        .finally(() => setLoading(false));

      setInterval(pollActivePads, 100);
      setInterval(pollClipping, 100);
      setInterval(pollConnection, 2000);
      setInterval(pollKnobs, 150);
      wireKnobs();
      wireBankMenu();
      wireDspPanel();
      wireThemePicker();

      const stopAll = window.getNativeFunction("stopAll");
      document.getElementById("stop-btn").addEventListener("click", () => stopAll());

      const shiftBtn = document.getElementById("shift-btn");
      shiftBtn.addEventListener("click", () => {
        shiftActive = !shiftActive;
        shiftBtn.classList.toggle("on", shiftActive);
        updateBankLetterHighlights();
      });
    }
