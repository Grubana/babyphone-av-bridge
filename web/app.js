'use strict';
// Live baby-monitor player. Video: H.264 via jMuxer (MSE). Audio: G.711 µ-law via
// Web Audio. WS frames are [tag, ...payload] where tag 0 = video, else audio.

const $ = id => document.getElementById(id);
const video = $('v'), statusEl = $('status'), statusText = $('statusText');
const veil = $('veil'), veilText = $('veilText'), soundBtn = $('soundBtn'), fsBtn = $('fsBtn');

const jmuxer = new JMuxer({ node: 'v', mode: 'video', flushingTime: 100, fps: 10, debug: false });

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

// ---- sound halo: track audio level even while muted ----
let level = 0, targetLevel = 0;
function feedLevel(samples) {
  let sum = 0;
  for (let i = 0; i < samples.length; i++) sum += samples[i] * samples[i];
  const rms = Math.sqrt(sum / samples.length);
  // Map quiet-room→cry onto 0..1 with a small noise floor and generous gain.
  const v = Math.min(1, Math.max(0, (rms - 0.004) * 9));
  if (v > targetLevel) targetLevel = v;            // attack: jump up
}
(function animateHalo() {
  targetLevel *= 0.86;                             // decay the peak
  level += (targetLevel - level) * 0.35;           // smooth toward it
  document.documentElement.style.setProperty('--level', level.toFixed(3));
  requestAnimationFrame(animateHalo);
})();

// ---- audio playback (opt-in; browsers block autoplay audio) ----
let actx = null, playHead = 0, audioOn = false;
function playAudio(samples) {
  if (!audioOn || !actx) return;
  const buf = actx.createBuffer(1, samples.length, 8000);
  buf.copyToChannel(samples, 0);
  const src = actx.createBufferSource();
  src.buffer = buf; src.connect(actx.destination);
  const now = actx.currentTime;
  if (playHead < now) playHead = now + 0.06;       // small jitter cushion
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

// ---- fullscreen ----
fsBtn.onclick = () => {
  const el = document.documentElement;
  if (document.fullscreenElement) document.exitFullscreen();
  else if (el.requestFullscreen) el.requestFullscreen();
  else if (video.webkitEnterFullscreen) video.webkitEnterFullscreen(); // iOS Safari
};

// ---- connection state ----
let gotVideo = false, lastFrame = 0;
function setState(kind, text) {
  statusEl.className = 'status' + (kind ? ' ' + kind : '');
  statusText.textContent = text;
}
function showVeil(text) { if (text) veilText.textContent = text; veil.classList.remove('hidden'); }
function hideVeil() { veil.classList.add('hidden'); }

// A monitor should never just die: reconnect forever, backing off to 5s.
let ws = null, backoff = 500;
function connect() {
  setState(gotVideo ? 'wait' : '', gotVideo ? 'Reconnecting' : 'Connecting');
  showVeil(gotVideo ? 'Reconnecting…' : 'Connecting to the camera…');
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
      gotVideo = true;
      setState('live', 'Live'); hideVeil();
    } else {
      const s = decode(payload);
      feedLevel(s); playAudio(s);
    }
  };
  ws.onclose = () => { setState('down', 'Offline'); scheduleReconnect(); };
  ws.onerror = () => { try { ws.close(); } catch (e) {} };
}
function scheduleReconnect() {
  setTimeout(connect, backoff);
  backoff = Math.min(backoff * 1.6, 5000);
}

// Stall watchdog: connected but no frames for a while -> tell the user plainly.
setInterval(() => {
  if (ws && ws.readyState === WebSocket.OPEN && gotVideo && performance.now() - lastFrame > 4000) {
    setState('wait', 'Waiting for camera'); showVeil('The camera stopped sending video.');
  }
}, 2000);

connect();
