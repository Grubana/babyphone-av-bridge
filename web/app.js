'use strict';
// Live baby-monitor player.
//   Video: WebCodecs VideoDecoder -> <canvas> (low latency, decode-and-paint).
//          Falls back to jMuxer/MSE -> <video> where WebCodecs is unavailable.
//   Audio: G.711 µ-law via Web Audio.
// WS frames are [tag, ...payload]: tag 0 = video (Annex-B H.264), else audio.

const $ = id => document.getElementById(id);
const video = $('v'), canvas = $('canvas'), cctx = canvas.getContext('2d');
const statusEl = $('status'), statusText = $('statusText');
const veil = $('veil'), veilText = $('veilText'), soundBtn = $('soundBtn');
const spark = $('spark'), sctx = spark.getContext('2d');

// Cap the on-screen size to the source's native frame size (crisp on desktop).
function fitToSource(w, h) {
  if (!w || !h) return;
  const s = document.documentElement.style;
  s.setProperty('--maxw', w + 'px');
  s.setProperty('--ar', w + ' / ' + h);
}

// ---- G.711 µ-law -> linear PCM ----
const ulaw = (() => {
  const t = new Int16Array(256);
  for (let i = 0; i < 256; i++) {
    const u = ~i, sign = u & 0x80, exp = (u >> 4) & 7, man = u & 0x0f;
    let s = ((man << 3) + 0x84) << exp; s -= 0x84;
    t[i] = sign ? -s : s;
  }
  return t;
})();
function decodeAudio(bytes) {
  const f = new Float32Array(bytes.length);
  for (let i = 0; i < bytes.length; i++) f[i] = ulaw[bytes[i]] / 32768;
  return f;
}

// ---- activity model: instant level (halo) + sustained accumulator (red alarm) ----
let level = 0, targetLevel = 0, activity = 0;
function feedLevel(samples) {
  let sum = 0;
  for (let i = 0; i < samples.length; i++) sum += samples[i] * samples[i];
  const v = Math.min(1, Math.max(0, (Math.sqrt(sum / samples.length) - 0.004) * 9));
  if (v > targetLevel) targetLevel = v;
}
const root = document.documentElement.style;
function tick() {
  targetLevel *= 0.86;
  level += (targetLevel - level) * 0.35;
  if (level > 0.16) activity = Math.min(1, activity + level * 0.012);
  else activity = Math.max(0, activity - 0.0035);
  root.setProperty('--level', level.toFixed(3));      // cheap: only drives opacity
  root.setProperty('--activity', activity.toFixed(3));
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);   // deferred so the sparkline consts below exist first

// ---- activity sparkline (~20s history) ----
const N = 100, hist = new Float32Array(N);
let dpr = 1;
function sizeSpark() {
  dpr = Math.min(2, window.devicePixelRatio || 1);
  spark.width = spark.clientWidth * dpr; spark.height = spark.clientHeight * dpr;
}
window.addEventListener('resize', sizeSpark); sizeSpark();
setInterval(() => { hist.copyWithin(0, 1); hist[N - 1] = level; }, 200);
setInterval(drawSpark, 100);
function drawSpark() {
  const w = spark.width, h = spark.height; if (!w) return;
  sctx.clearRect(0, 0, w, h);
  const bw = w / N;
  for (let i = 0; i < N; i++) {
    const v = hist[i]; if (v < 0.02) continue;
    const bh = Math.max(dpr, v * h);
    const g = Math.round(178 + (95 - 178) * v), b = Math.round(122 + (95 - 122) * v);
    sctx.fillStyle = `rgba(255,${g},${b},${0.3 + 0.6 * v})`;
    sctx.fillRect(i * bw, h - bh, bw * 0.68, bh);
  }
}

// ---- audio playback (opt-in; browsers block autoplay audio) ----
let actx = null, playHead = 0, audioOn = false;
function playAudio(samples) {
  if (!audioOn || !actx) return;
  const buf = actx.createBuffer(1, samples.length, 8000);
  buf.copyToChannel(samples, 0);
  const src = actx.createBufferSource();
  src.buffer = buf; src.connect(actx.destination);
  const now = actx.currentTime;
  if (playHead < now) playHead = now + 0.06;
  src.start(playHead); playHead += buf.duration;
}
soundBtn.onclick = () => {
  audioOn = !audioOn;
  if (audioOn) {
    actx = actx || new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 8000 });
    actx.resume(); playHead = actx.currentTime;
  }
  soundBtn.classList.toggle('on', audioOn);
  soundBtn.setAttribute('aria-pressed', String(audioOn));
  soundBtn.title = audioOn ? 'Mute sound' : 'Turn sound on';
};

// ---- connection / liveness state (shared by both video paths) ----
let gotVideo = false, lastFrame = 0, stalled = false, sawVideoAt = 0;
function setState(kind, text) { statusEl.className = 'status' + (kind ? ' ' + kind : ''); statusText.textContent = text; }
function showVeil(text) { if (text) veilText.textContent = text; veil.classList.remove('hidden'); }
function hideVeil() { veil.classList.add('hidden'); }
function onVideoShown() {                       // a real frame reached the screen
  lastFrame = performance.now();
  gotVideo = true; stalled = false;
  setState('live', 'Live'); hideVeil();
}

// ==== VIDEO ================================================================
let videoMode = ('VideoDecoder' in window && 'EncodedVideoChunk' in window) ? 'wc' : 'mse';

// ---- MSE fallback (jMuxer) ----
let jmuxer = null;
function makeJmuxer() {
  return new JMuxer({
    node: 'v', mode: 'video', flushingTime: 100, maxDelay: 1000,
    clearBuffer: true, fps: 10, debug: false, onError: () => resetVideo(),
  });
}
function feedMse(payload) {
  if (!jmuxer) jmuxer = makeJmuxer();
  jmuxer.feed({ video: payload });
  onVideoShown();
}
function resetMse() { try { if (jmuxer) jmuxer.destroy(); } catch (e) {} jmuxer = null; }

// ---- WebCodecs (primary) ----
let decoder = null, decoderReady = false, wcTs = 0, wcFramesOut = 0;
const hx = b => b.toString(16).padStart(2, '0');
// Scan Annex-B for NAL units; returns [{type, pos}] (pos = index of NAL header).
// A 3-byte 00 00 01 scan also catches 4-byte 00 00 00 01 (the 00 00 01 is inside).
function scanNals(d) {
  const out = [];
  for (let i = 0; i + 3 < d.length; i++) {
    if (d[i] === 0 && d[i + 1] === 0 && d[i + 2] === 1) { out.push({ type: d[i + 3] & 0x1f, pos: i + 3 }); i += 2; }
  }
  return out;
}
function paintFrame(frame) {
  try {
    if (canvas.width !== frame.displayWidth || canvas.height !== frame.displayHeight) {
      canvas.width = frame.displayWidth; canvas.height = frame.displayHeight;
      fitToSource(frame.displayWidth, frame.displayHeight);
    }
    cctx.drawImage(frame, 0, 0);
  } finally { frame.close(); }
  wcFramesOut++;
  onVideoShown();
}
function onDecErr(e) {
  try { if (decoder) decoder.close(); } catch (_) {}
  decoder = null; decoderReady = false;
  if (wcFramesOut === 0) switchToMse();   // never worked -> abandon WebCodecs
}
function feedWc(payload) {
  const nals = scanNals(payload);
  const isKey = nals.some(x => x.type === 5 || x.type === 7);   // IDR or SPS present
  if (!decoderReady) {
    if (!isKey) return;                    // decoding must start on a keyframe
    let codec = 'avc1.42E01E';             // sensible default (baseline 3.0)
    const sps = nals.find(x => x.type === 7);
    if (sps && sps.pos + 3 < payload.length)
      codec = 'avc1.' + hx(payload[sps.pos + 1]) + hx(payload[sps.pos + 2]) + hx(payload[sps.pos + 3]);
    try {
      decoder = new VideoDecoder({ output: paintFrame, error: onDecErr });
      decoder.configure({ codec, optimizeForLatency: true });
      decoderReady = true; wcFramesOut = 0;
    } catch (e) { console.warn('[avb] WebCodecs configure failed, using MSE:', e); switchToMse(); return feedMse(payload); }
  }
  try {
    decoder.decode(new EncodedVideoChunk({ type: isKey ? 'key' : 'delta', timestamp: (wcTs += 100000), data: payload }));
  } catch (e) { onDecErr(e); }
}
function resetWc() { try { if (decoder) decoder.close(); } catch (_) {} decoder = null; decoderReady = false; }
function switchToMse() {
  if (videoMode === 'mse') return;
  console.warn('[avb] falling back to MSE video');
  resetWc();
  videoMode = 'mse';
  canvas.classList.add('hide'); video.classList.remove('hide');
}

// ---- router ----
function feedVideo(payload) {
  if (!sawVideoAt) sawVideoAt = performance.now();
  if (videoMode === 'wc') feedWc(payload); else feedMse(payload);
}
function resetVideo() { if (videoMode === 'wc') resetWc(); else resetMse(); lastFrame = 0; }

// show the active surface
if (videoMode === 'wc') video.classList.add('hide'); else canvas.classList.add('hide');
video.addEventListener('loadedmetadata', () => fitToSource(video.videoWidth, video.videoHeight));
video.addEventListener('resize', () => fitToSource(video.videoWidth, video.videoHeight));

// Returning to a backgrounded tab (MSE only): jump once to live.
document.addEventListener('visibilitychange', () => {
  if (document.hidden || videoMode !== 'mse' || !video.buffered.length) return;
  try {
    const end = video.buffered.end(video.buffered.length - 1);
    if (end - video.currentTime > 3) video.currentTime = end - 0.4;
  } catch (e) {}
});

// ==== connection ===========================================================
function resetStream() { resetVideo(); }

let ws = null, backoff = 500;
function connect() {
  resetStream();
  setState(gotVideo ? 'wait' : '', gotVideo ? 'Reconnecting' : 'Connecting');
  showVeil(gotVideo ? 'Reconnecting to the camera…' : 'Connecting to the camera…');
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { backoff = 500; setState('wait', 'Waiting for camera'); showVeil('Waiting for the camera…'); };
  ws.onmessage = ev => {
    const a = new Uint8Array(ev.data);
    if (!a.length) return;
    const payload = a.subarray(1);
    if (a[0] === 0) feedVideo(payload);
    else { const s = decodeAudio(payload); feedLevel(s); playAudio(s); }
  };
  ws.onclose = () => { setState('down', 'Offline'); scheduleReconnect(); };
  ws.onerror = () => { try { ws.close(); } catch (e) {} };
}
function scheduleReconnect() { setTimeout(connect, backoff); backoff = Math.min(backoff * 1.6, 5000); }

// Liveness watchdog: frames stopped -> say so and drop the stale picture (once).
// Also: if the WebCodecs path takes video packets but never paints, fall back.
setInterval(() => {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const now = performance.now();
  if (videoMode === 'wc' && !gotVideo && sawVideoAt && now - sawVideoAt > 4000) {
    switchToMse();
  }
  if (gotVideo && !stalled && now - lastFrame > 2500) {
    stalled = true;
    setState('down', 'No signal');
    showVeil('Signal lost — waiting for live video.');
    resetStream();
  }
}, 1000);

connect();
