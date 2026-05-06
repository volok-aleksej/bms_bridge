'use strict';

// ── Gauge drawing ─────────────────────────────────────────────────────────────

function drawGauge(canvasId, value, min, max, unit, zones) {
  const cv = document.getElementById(canvasId);
  if (!cv) return;
  const ctx = cv.getContext('2d');
  const W = cv.width, H = cv.height;
  const cx = W / 2, cy = H - 10;
  const r = Math.min(cx - 10, H - 20);

  ctx.clearRect(0, 0, W, H);

  const startA = Math.PI;
  const endA   = 0;
  const range  = endA - startA; // = -π  (going clockwise via negative)

  // Arc zones
  for (const z of zones) {
    const a0 = startA + (z.from - min) / (max - min) * Math.PI;
    const a1 = startA + (z.to   - min) / (max - min) * Math.PI;
    ctx.beginPath();
    ctx.arc(cx, cy, r, a0, a1);
    ctx.strokeStyle = z.color;
    ctx.lineWidth   = 8;
    ctx.stroke();
  }

  // Tick marks
  ctx.lineWidth = 1;
  for (let i = 0; i <= 10; i++) {
    const a = startA + (i / 10) * Math.PI;
    const isMaj = i % 2 === 0;
    const r0 = isMaj ? r - 16 : r - 10;
    ctx.beginPath();
    ctx.moveTo(cx + r0  * Math.cos(a), cy + r0  * Math.sin(a));
    ctx.lineTo(cx + (r+2) * Math.cos(a), cy + (r+2) * Math.sin(a));
    ctx.strokeStyle = isMaj ? '#57606a' : '#d0d7de';
    ctx.stroke();
  }

  // Min / max labels
  ctx.font = 'bold 11px monospace';
  ctx.fillStyle = '#1f2328';
  ctx.textAlign = 'left';
  ctx.fillText(min + unit, 4, H - 2);
  ctx.textAlign = 'right';
  ctx.fillText(max + unit, W - 4, H - 2);

  // Needle
  const clamped = Math.min(max, Math.max(min, value != null ? value : min));
  const needleA = startA + (clamped - min) / (max - min) * Math.PI;

  // Animated target stored on canvas element
  if (cv._targetA === undefined) cv._targetA = needleA;
  cv._targetA = needleA;

  _animateNeedle(cv, ctx, cx, cy, r, unit);
}

const _rafMap = new WeakMap();

function _animateNeedle(cv, ctx, cx, cy, r, unit) {
  if (_rafMap.get(cv)) return; // already animating

  function step() {
    if (cv._currentA === undefined) cv._currentA = cv._targetA;
    const diff = cv._targetA - cv._currentA;
    if (Math.abs(diff) < 0.001) {
      cv._currentA = cv._targetA;
      _rafMap.set(cv, null);
      _drawNeedle(ctx, cx, cy, r, cv._currentA);
      return;
    }
    cv._currentA += diff * 0.12;
    _drawNeedle(ctx, cx, cy, r, cv._currentA);
    _rafMap.set(cv, requestAnimationFrame(step));
  }

  _rafMap.set(cv, requestAnimationFrame(step));
}

function _drawNeedle(ctx, cx, cy, r, angle) {
  // Redraw needle area only (overdraw with bg colour is simpler)
  ctx.save();
  ctx.globalCompositeOperation = 'source-atop';
  // erase inner disc
  ctx.beginPath();
  ctx.arc(cx, cy, r - 20, 0, Math.PI * 2);
  ctx.fillStyle = '#ffffff';
  ctx.fill();
  ctx.restore();

  // needle line
  ctx.beginPath();
  ctx.moveTo(cx, cy);
  ctx.lineTo(cx + (r - 22) * Math.cos(angle), cy + (r - 22) * Math.sin(angle));
  ctx.strokeStyle = '#1f2328';
  ctx.lineWidth   = 2;
  ctx.lineCap     = 'round';
  ctx.stroke();

  // pivot dot
  ctx.beginPath();
  ctx.arc(cx, cy, 5, 0, Math.PI * 2);
  ctx.fillStyle = '#0969da';
  ctx.fill();
}

// ── Chart ─────────────────────────────────────────────────────────────────────

const TIP = document.getElementById('tip');

function p2(n) { return String(n).padStart(2, '0'); }

function fmtTs(ts, span) {
  const d = new Date(ts);
  if (span <= 6 * 36e5)
    return p2(d.getHours()) + ':' + p2(d.getMinutes()) + ':' + p2(d.getSeconds());
  if (span <= 48 * 36e5)
    return p2(d.getHours()) + ':' + p2(d.getMinutes());
  return p2(d.getDate()) + '.' + p2(d.getMonth() + 1) + ' ' +
         p2(d.getHours()) + ':' + p2(d.getMinutes());
}

function fmtFull(ts) {
  const d = new Date(ts);
  return d.toLocaleDateString() + ' ' + d.toLocaleTimeString();
}

function dsample(a, max) {
  if (a.length <= max) return a;
  const k = a.length / max;
  return Array.from({length: max}, (_, i) => a[Math.round(i * k)]);
}

class Chart {
  constructor(id, title, yfmt) {
    this.cv    = document.getElementById(id);
    this.title = title;
    this.yfmt  = yfmt;
    this.ss    = [];
    this.m     = null;
    if (!this.cv) return;
    new ResizeObserver(() => {
      const w = this.cv.clientWidth;
      if (this.cv.width !== w) { this.cv.width = w; this.draw(); }
    }).observe(this.cv);
    this.cv.addEventListener('mousemove',  e => this._tip(e));
    this.cv.addEventListener('mouseleave', () => { TIP.style.display = 'none'; });
  }

  set(ss) { this.ss = ss; this.draw(); }

  draw() {
    const cv = this.cv;
    if (!cv) return;
    const ctx = cv.getContext('2d'), W = cv.width, H = cv.height;
    const p = {t: 22, r: 14, b: 42, l: 64};
    const cw = W - p.l - p.r, ch = H - p.t - p.b;
    ctx.fillStyle = '#ffffff'; ctx.fillRect(0, 0, W, H);
    if (!this.ss.length) return;

    let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
    for (const s of this.ss)
      for (const pt of s.d) {
        if (pt.x < x0) x0 = pt.x; if (pt.x > x1) x1 = pt.x;
        if (pt.y < y0) y0 = pt.y; if (pt.y > y1) y1 = pt.y;
      }
    if (!isFinite(x0)) return;

    const yr = y1 - y0 || 1; y0 -= yr * .05; y1 += yr * .05;
    const tx = x => p.l + (x - x0) / (x1 - x0) * cw;
    const ty = y => p.t + ch - (y - y0) / (y1 - y0) * ch;
    const span = x1 - x0;

    ctx.font = 'bold 11px monospace';
    for (let i = 0; i <= 4; i++) {
      const y = y0 + (y1 - y0) * i / 4, py = ty(y);
      ctx.fillStyle = '#1f2328'; ctx.textAlign = 'right';
      ctx.fillText(this.yfmt(y), p.l - 3, py + 3);
    }
    const nx = Math.max(3, Math.floor(cw / 100));
    for (let i = 0; i <= nx; i++) {
      const x = x0 + (x1 - x0) * i / nx, px = tx(x);
      ctx.strokeStyle = '#eaeef2'; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(px, p.t); ctx.lineTo(px, p.t + ch); ctx.stroke();
      ctx.fillStyle = '#1f2328';
      ctx.textAlign = i === 0 ? 'left' : i === nx ? 'right' : 'center';
      ctx.fillText(fmtTs(x, span), px, p.t + ch + 14);
    }
    ctx.strokeStyle = '#d0d7de'; ctx.lineWidth = 1;
    ctx.strokeRect(p.l, p.t, cw, ch);

    ctx.save(); ctx.beginPath(); ctx.rect(p.l, p.t, cw, ch); ctx.clip();
    for (const s of this.ss) {
      const pts = dsample(s.d, cw * 2);
      ctx.beginPath(); ctx.strokeStyle = s.c; ctx.lineWidth = 1.5;
      if (pts.length === 1) {
        ctx.moveTo(tx(pts[0].x), ty(pts[0].y));
      } else if (pts.length >= 2) {
        ctx.moveTo(tx(pts[0].x), ty(pts[0].y));
        for (let i = 1; i < pts.length - 1; i++) {
          const cpx = tx(pts[i].x), cpy = ty(pts[i].y);
          const ex  = tx((pts[i].x + pts[i+1].x) / 2);
          const ey  = ty((pts[i].y + pts[i+1].y) / 2);
          ctx.quadraticCurveTo(cpx, cpy, ex, ey);
        }
        const last = pts[pts.length - 1];
        ctx.lineTo(tx(last.x), ty(last.y));
      }
      ctx.stroke();
    }
    ctx.restore();

    ctx.fillStyle = '#1f2328'; ctx.font = 'bold 11px monospace'; ctx.textAlign = 'left';
    ctx.fillText(this.title, p.l, 15);

    if (this.ss.length <= 8) {
      let lx = p.l + cw;
      ctx.font = 'bold 11px monospace'; ctx.textAlign = 'right';
      for (const s of [...this.ss].reverse()) {
        const tw = ctx.measureText(s.n).width;
        ctx.fillStyle = '#1f2328'; ctx.fillText(s.n, lx, p.t + ch + 26);
        lx -= tw + 3;
        ctx.fillStyle = s.c; ctx.fillRect(lx, p.t + ch + 32, 10, 3);
        lx -= 14;
      }
    }
    this.m = {x0, x1, p, cw, ch};
  }

  _tip(e) {
    if (!this.m || !this.ss.length) return;
    const m = this.m, r = this.cv.getBoundingClientRect();
    const mx = (e.clientX - r.left) * (this.cv.width / r.width);
    const x  = m.x0 + (mx - m.p.l) / m.cw * (m.x1 - m.x0);
    const ref = this.ss[0].d;
    let bi = 0, bd = Infinity;
    for (let i = 0; i < ref.length; i++) {
      const d = Math.abs(ref[i].x - x);
      if (d < bd) { bd = d; bi = i; }
    }
    const rows = this.ss.map(s =>
      `<div><span style="color:${s.c}">&#9632;</span> ${s.n}: ${this.yfmt(s.d[bi]?.y ?? 0)}</div>`
    ).join('');
    TIP.innerHTML = '<div style="color:#484f58;font-size:10px">' + fmtFull(ref[bi].x) + '</div>' + rows;
    TIP.style.display = 'block';
    const tipX = e.clientX > window.innerWidth / 2 ? e.clientX - TIP.offsetWidth - 14 : e.clientX + 14;
    const tipY = Math.min(e.clientY - 10, window.innerHeight - TIP.offsetHeight - 10);
    TIP.style.left = tipX + 'px';
    TIP.style.top  = tipY + 'px';
  }
}

// ── Info rows helper ──────────────────────────────────────────────────────────

function buildInfoRows(info) {
  const fV   = v => (v / 1000).toFixed(3) + ' V';
  const fA   = v => (v / 1000).toFixed(3) + ' A';
  const fdC  = v => (v / 10).toFixed(1) + ' °C';
  const fSec = v => v + ' s';
  const fmS  = v => (v / 1000).toFixed(3) + ' ms';
  const fAh  = v => (v / 1000).toFixed(1) + ' Ah';
  const fBool= v => v ? 'ON' : 'OFF';
  return [
    ['Cell count',              info.cell_count,               v => String(v)],
    ['Nominal capacity',        info.nominal_capacity_mah,     fAh],
    ['UVP',                     info.cell_uvp_mv,              fV],
    ['UVPR',                    info.cell_uvpr_mv,             fV],
    ['OVP',                     info.cell_ovp_mv,              fV],
    ['OVPR',                    info.cell_ovpr_mv,             fV],
    ['SoC 100%',                info.soc_100_mv,               fV],
    ['SoC 0%',                  info.soc_0_mv,                 fV],
    ['Balance trigger',         info.balance_trigger_mv,       fV],
    ['Balance start',           info.start_balance_mv,         fV],
    ['Max charge current',      info.max_charge_current_ma,    fA],
    ['Max discharge current',   info.max_discharge_current_ma, fA],
    ['Max balance current',     info.max_balance_current_ma,   fA],
    ['Charge OCP delay',        info.charge_ocp_delay_s,       fSec],
    ['Charge OCP recovery',     info.charge_ocp_recovery_s,    fSec],
    ['Discharge OCP delay',     info.discharge_ocp_delay_s,    fSec],
    ['Discharge OCP recovery',  info.discharge_ocp_recovery_s, fSec],
    ['SCP delay',               info.scp_delay_us,             fmS],
    ['SCP recovery',            info.scp_recovery_s,           fSec],
    ['Charge OTP',              info.charge_otp_dC,            fdC],
    ['Charge OTP recovery',     info.charge_otp_recovery_dC,   fdC],
    ['Discharge OTP',           info.discharge_otp_dC,         fdC],
    ['Discharge OTP recovery',  info.discharge_otp_recovery_dC,fdC],
    ['Charge UTP',              info.charge_utp_dC,            fdC],
    ['Charge UTP recovery',     info.charge_utp_recovery_dC,   fdC],
    ['MOS OTP',                 info.mosfet_otp_dC,            fdC],
    ['MOS OTP recovery',        info.mosfet_otp_recovery_dC,   fdC],
    ['Smart sleep',             info.smart_sleep_mv,           fV],
    ['Power off voltage',       info.power_off_mv,             fV],
    ['Request charge voltage',  info.request_charge_mv,        fV],
    ['Request float voltage',   info.request_float_mv,         fV],
    ['Charge switch',           info.charging_switch_on,       fBool],
    ['Discharge switch',        info.discharging_switch_on,    fBool],
    ['Balancer switch',         info.balancer_switch_on,       fBool],
  ].map(([label, val, fmt]) => [label, fmt(val)]);
}

// ── Alpine component ──────────────────────────────────────────────────────────

function bms() {
  return {
    tab:       'realtime',
    live:      {},
    info:      null,
    status:    'connecting…',
    histHours: 24,
    histStatus:'',

    _charts: null,
    _vMin: 40, _vMax: 70,   // voltage gauge range (V)
    _iMin: -100, _iMax: 100, // current gauge range (A)

    init() {
      this._initCharts();
      this._fetchInfo();
      this._startLivePolling();
      this._histLastLoad = 0;
      setInterval(() => {
        if (this.tab !== 'history') return;
        const intervalMs = this.histHours * 6000; // window / 600 points
        if (Date.now() - this._histLastLoad >= intervalMs) this.loadHistory();
      }, 1000);
    },

    _initCharts() {
      const fV = v => (v / 1000).toFixed(2) + 'V';
      const fA = v => (v >= 0 ? '+' : '') + (v / 1000).toFixed(2) + 'A';
      const fP = v => v.toFixed(0) + '%';
      const fT = v => (v / 10).toFixed(1) + '°C';
      this._charts = {
        v: new Chart('cv', 'Pack Voltage', fV),
        c: new Chart('cc', 'Current  (+ charge / − discharge)', fA),
        s: new Chart('cs', 'SoC', fP),
        t: new Chart('ct', 'Temperature', fT),
      };
    },

    async _fetchInfo() {
      try {
        const r = await fetch('/info');
        if (r.ok) this.info = await r.json();
      } catch (_) {}
    },

    _startLivePolling() {
      const poll = async () => {
        try {
          const now = Math.floor(Date.now() / 1000);
          const r = await fetch(`/history?time_start=${now - 10}&time_end=${now}&count=1`);
          if (!r.ok) throw new Error(r.status);
          const j = await r.json();
          if (j.data && j.data.length) {
            this.live = j.data[0];
            this.status = new Date().toLocaleTimeString();
            this._updateGauges();
          }
        } catch (e) {
          this.status = 'error: ' + e.message;
        }
      };
      poll();
      setInterval(poll, 500);
    },

    _updateGauges() {
      if (this.live.voltage_mv != null) {
        const v = this.live.voltage_mv / 1000;
        drawGauge('gauge-v', v, this._vMin, this._vMax, 'V', [
          {from: this._vMin, to: 48,           color: '#f85149'},
          {from: 48,         to: 58,            color: '#3fb950'},
          {from: 58,         to: this._vMax,    color: '#d29922'},
        ]);
      }
      if (this.live.current_ma != null) {
        const i = this.live.current_ma / 1000;
        drawGauge('gauge-i', i, this._iMin, this._iMax, 'A', [
          {from: this._iMin, to: -20,           color: '#f85149'},
          {from: -20,        to: 0,             color: '#d29922'},
          {from: 0,          to: 20,            color: '#3fb950'},
          {from: 20,         to: this._iMax,    color: '#d29922'},
        ]);
      }
    },

    async loadHistory() {
      this.histStatus = 'loading…';
      this._histLastLoad = Date.now();
      try {
        const now = Date.now(), t0 = now - this.histHours * 36e5, per = 600;
        const url = `/history?time_start=${Math.floor(t0/1e3)}&time_end=${Math.floor(now/1e3)}&count=${per}`;
        let r = await fetch(url), j = await r.json();
        let all = [...j.data];
        while (j.data.length === per) {
          r = await fetch('/history?next&req_id=' + j.req_id);
          if (!r.ok) break;
          j = await r.json();
          if (j.error || !j.data.length) break;
          all = all.concat(j.data);
        }
        all = all.filter(d => d.ts >= t0 && d.ts <= now);
        all.sort((a, b) => a.ts - b.ts);

        // wait for DOM to be visible so canvases have width
        await this.$nextTick();
        document.querySelectorAll('.charts canvas').forEach(cv => {
          if (!cv.width || cv.width < 10) cv.width = cv.clientWidth || 800;
        });

        const mk = (fn, c, n) => ({d: all.map(d => ({x: d.ts, y: fn(d)})), c, n});
        this._charts.v.set([mk(d => d.voltage_mv,  '#0969da', 'U')]);
        this._charts.c.set([mk(d => d.current_ma,  '#1a7f37', 'I')]);
        this._charts.s.set([mk(d => d.soc_pct,     '#9a6700', 'SoC')]);
        this._charts.t.set([
          mk(d => d.temp1_dc,     '#cf222e', 'T1'),
          mk(d => d.temp2_dc,     '#8250df', 'T2'),
          mk(d => d.mos_temp_dc,  '#bc4c00', 'MOS'),
        ]);
        this.histStatus = all.length + ' pts · ' + new Date().toLocaleTimeString();
      } catch (e) {
        this.histStatus = 'error: ' + e.message;
      }
    },

    cellClass(mv, i) {
      const active = (this.live.cells_mv || []).filter(v => v > 0);
      if (!active.length) return '';
      const max = Math.max(...active), min = Math.min(...active);
      if (mv === max) return 'hi';
      if (mv > 0 && mv === min) return 'lo';
      return '';
    },

    infoRows() {
      return this.info ? buildInfoRows(this.info) : [];
    },
  };
}
