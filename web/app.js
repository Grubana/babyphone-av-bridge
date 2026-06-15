const statusEl = document.getElementById('b');
const jmuxer = new JMuxer({ node: 'v', mode: 'video', flushingTime: 100, fps: 10, debug: false });
let actx = null;
document.getElementById('a').onclick = () => { actx = actx || new (window.AudioContext||window.webkitAudioContext)({sampleRate:8000}); actx.resume(); };

// G.711 mu-law -> linear PCM (16-bit)
const ulawTable = (() => { const t = new Int16Array(256);
  for (let i=0;i<256;i++){ let u=~i; let sign=u&0x80, exp=(u>>4)&7, man=u&0x0f;
    let s=((man<<3)+0x84)<<exp; s-=0x84; t[i]= sign? -s : s; } return t; })();
let playTime = 0;
function playAudio(bytes){ if(!actx) return; const f=new Float32Array(bytes.length);
  for(let i=0;i<bytes.length;i++) f[i]=ulawTable[bytes[i]]/32768;
  const buf=actx.createBuffer(1,f.length,8000); buf.copyToChannel(f,0);
  const src=actx.createBufferSource(); src.buffer=buf; src.connect(actx.destination);
  const now=actx.currentTime; if(playTime<now) playTime=now; src.start(playTime); playTime+=buf.duration; }

const ws = new WebSocket(`ws://${location.host}/ws`);
ws.binaryType = 'arraybuffer';
ws.onopen = () => statusEl.textContent = 'connected';
ws.onclose = () => statusEl.textContent = 'disconnected';
ws.onmessage = (ev) => { const a = new Uint8Array(ev.data); if(!a.length) return;
  const tag = a[0], payload = a.subarray(1);
  if (tag === 0) jmuxer.feed({ video: payload }); else playAudio(payload); };
