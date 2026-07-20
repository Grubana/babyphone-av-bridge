'use strict';
// Live baby-monitor player. Video: H.264 via jMuxer (MSE). Audio: G.711 µ-law via
// Web Audio. WS frames are [tag, ...payload] where tag 0 = video, else audio.
// Priority: the picture must be LIVE — we pin to the live edge and, if frames
// stop, we say so rather than replay stale video.

const $ = id => document.getElementById(id);
const video = $('v'), statusEl = $('status'), statusText = $('statusText');
const veil = $('veil'), veilText = $('veilText'), soundBtn = $('soundBtn');
const spark = $('spark'), sctx = spark.getContext('2d');

// maxDelay keeps jMuxer near the live edge; clearBuffer drops played data so the
// buffer (and thus latency) can't grow without bound.
function makeJmuxer() {
  return new JMuxer({
    node: 'v', mode: 'video', flushingTime: 50, maxDelay: 250,
    clearBuffer: true, fps: 10, debug: false,
    onError: () => resetStream('decoder error'),
  });
}
let jmuxer = makeJmuxer();

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
function decode(bytes) {
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
(function tick() {
  targetLevel *= 0.86;
  level += (targetLevel - level) * 0.35;
  // sustained activity rises while there's real sound, decays slowly when quiet
  if (level > 0.16) activity = Math.min(1, activity + level * 0.012);
  else activity = Math.max(0, activity - 0.0035);
  root.setProperty('--level', level.toFixed(3));
  root.setProperty('--activity', activity.toFixed(3));
  // halo warms amber(255,178,122) -> alarm-red(245,95,95) as activity builds
  const g = Math.round(178 + (95 - 178) * activity), b = Math.round(122 + (95 - 122) * activity);
  root.setProperty('--halo-rgb', `255,${g},${b}`);
  drawSpark();
  requestAnimationFrame(tick);
})();

// ---- activity sparkline (~20s history), so you can see recent noise at a glance ----
const N = 100, hist = new Float32Array(N);
let dpr = 1;
function sizeSpark() {
  dpr = Math.min(2, window.devicePixelRatio || 1);
  spark.width = spark.clientWidth * dpr; spark.height = spark.clientHeight * dpr;
}
window.addEventListener('resize', sizeSpark); sizeSpark();
setInterval(() => { hist.copyWithin(0, 1); hist[N - 1] = level; }, 200);  // 100*200ms = 20s
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

// ---- keep the picture at the live edge (bounded latency, no replay of old video) ----
setInterval(() => {
  if (!video.buffered.length) return;
  try {
    const end = video.buffered.end(video.buffered.length - 1);
    const gap = end - video.currentTime;
    if (gap > 1.0) { video.currentTime = end - 0.15; video.playbackRate = 1; }
    else if (gap > 0.45) video.playbackRate = 1.12;   // gently catch up
    else video.playbackRate = 1;
  } catch (e) {}
}, 700);
// Returning to a backgrounded tab: jump straight back to live.
document.addEventListener('visibilitychange', () => {
  if (document.hidden || !video.buffered.length) return;
  try { video.currentTime = video.buffered.end(video.buffered.length - 1) - 0.15; } catch (e) {}
});

// ---- connection / liveness state ----
let gotVideo = false, lastFrame = 0, stalled = false;
function setState(kind, text) { statusEl.className = 'status' + (kind ? ' ' + kind : ''); statusText.textContent = text; }
function showVeil(text) { if (text) veilText.textContent = text; veil.classList.remove('hidden'); }
function hideVeil() { veil.classList.add('hidden'); }

function resetStream(why) {           // flush stale video so we never show old frames
  try { jmuxer.destroy(); } catch (e) {}
  jmuxer = makeJmuxer();
  lastFrame = 0;
}

// A monitor must never just die: reconnect forever, backing off to 5s.
let ws = null, backoff = 500;
function connect() {
  resetStream('reconnect');
  setState(gotVideo ? 'wait' : '', gotVideo ? 'Reconnecting' : 'Connecting');
  showVeil(gotVideo ? 'Reconnecting to the camera…' : 'Connecting to the camera…');
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { backoff = 500; setState('wait', 'Waiting for camera'); showVeil('Waiting for the camera…'); };
  ws.onmessage = ev => {
    const a = new Uint8Array(ev.data);
    if (!a.length) return;
    const payload = a.subarray(1);
    if (a[0] === 0) {
      jmuxer.feed({ video: payload });
      lastFrame = performance.now();
      gotVideo = true; stalled = false;
      setState('live', 'Live'); hideVeil();
    } else {
      const s = decode(payload);
      feedLevel(s); playAudio(s);
    }
  };
  ws.onclose = () => { setState('down', 'Offline'); scheduleReconnect(); };
  ws.onerror = () => { try { ws.close(); } catch (e) {} };
}
function scheduleReconnect() { setTimeout(connect, backoff); backoff = Math.min(backoff * 1.6, 5000); }

// Liveness watchdog: frames stopped -> say so and drop the stale picture (once),
// don't fake "live". Clears itself when frames resume (stalled=false above).
setInterval(() => {
  if (ws && ws.readyState === WebSocket.OPEN && gotVideo && !stalled
      && performance.now() - lastFrame > 2500) {
    stalled = true;
    setState('down', 'No signal');
    showVeil('Signal lost — waiting for live video.');
    resetStream('stall');
  }
}, 1000);

connect();
