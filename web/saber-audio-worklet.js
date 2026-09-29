/* The AudioWorklet half of the page's audio: a queue the main thread fills and this drains one quantum at a time.
 *
 * The module mixes into a ring in its own linear memory and the main thread copies out whatever is ready after
 * each frame, then posts it here (the reference WASM port's arrangement: a copy per frame, no SharedArrayBuffer,
 * so the page works when it is served from anywhere without COOP/COEP headers). 147 frames of stereo S16 is
 * under 600 bytes a frame, so the copy is not worth optimising away.
 *
 * The queue is kept slightly ahead of the clock. If it runs dry the last sample is held and faded out rather than
 * cut to zero, so a late frame from the main thread (a GC pause, a tab switch) is a dip and not a click. */
class SaberAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];
    this.queued = 0;
    this.started = false;
    this.preroll = Math.floor(sampleRate * 0.10);
    this.lowWater = Math.floor(sampleRate * 0.04);
    this.maxQueue = Math.floor(sampleRate * 0.40);
    this.lastL = 0;
    this.lastR = 0;
    this.fade = 0;
    this.consumed = 0;
    this.underruns = 0;
    this.peak = 0;

    this.port.onmessage = (ev) => {
      const m = ev.data || {};
      if (m.type === 'reset') {
        this.queue.length = 0;
        this.queued = 0;
        this.started = false;
        this.lastL = this.lastR = 0;
        this.fade = 0;
        this.consumed = 0;
        this.underruns = 0;
      } else if (m.type === 'config') {
        if (m.preroll) this.preroll = m.preroll | 0;
        if (m.lowWater) this.lowWater = m.lowWater | 0;
        if (m.maxQueue) this.maxQueue = m.maxQueue | 0;
      } else if (m.type === 'audio' && m.data) {
        const data = m.data instanceof Int16Array ? m.data : new Int16Array(m.data);
        const frames = (m.frames | 0) || (data.length >> 1);
        if (frames <= 0) return;
        this.queue.push({ data, frames, pos: 0 });
        this.queued += frames;
        // the main thread got ahead: drop the oldest, as a hardware ring would
        while (this.queued > this.maxQueue && this.queue.length > 2) {
          const head = this.queue.shift();
          this.queued -= head.frames - head.pos;
        }
      }
    };
  }

  pop() {
    while (this.queue.length) {
      const h = this.queue[0];
      if (h.pos < h.frames) {
        const i = h.pos++ << 1;
        this.queued--;
        this.consumed++;
        if (h.pos >= h.frames) this.queue.shift();
        this.lastL = h.data[i] / 32768;
        this.lastR = h.data[i + 1] / 32768;
        const a = this.lastL < 0 ? -this.lastL : this.lastL;
        const b = this.lastR < 0 ? -this.lastR : this.lastR;
        if (a > this.peak) this.peak = a;
        if (b > this.peak) this.peak = b;
        return true;
      }
      this.queue.shift();
    }
    return false;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const l = out[0];
    const r = out[1] || out[0];

    if (!this.started) {
      if (this.queued >= this.preroll) this.started = true;
      else {
        l.fill(0);
        if (r !== l) r.fill(0);
        this.report();
        return true;
      }
    }

    for (let i = 0; i < l.length; i++) {
      if (this.pop()) {
        l[i] = this.lastL;
        r[i] = this.lastR;
        this.fade = 0;
      } else {
        if (this.started) this.underruns++;
        // fade the held sample out over ~2 ms instead of cutting it: a short tail is inaudible, a hard cut clicks
        const g = this.fade < 96 ? (1.0 - this.fade / 96.0) : 0.0;
        l[i] = this.lastL * g;
        r[i] = this.lastR * g;
        this.fade++;
        if (this.fade > 1024 && this.queued < this.lowWater) this.started = false;
      }
    }

    this.report();
    return true;
  }

  report() {
    if (this.consumed >= 1024 || this.underruns) {
      this.port.postMessage({
        type: 'stat',
        queued: this.queued,
        consumed: this.consumed,
        underruns: this.underruns,
        started: this.started,
        peak: this.peak,
      });
      this.consumed = 0;
      this.underruns = 0;
      this.peak = 0;
    }
  }
}

registerProcessor('saber-audio-processor', SaberAudioProcessor);
