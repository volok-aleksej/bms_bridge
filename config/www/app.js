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

// ── Chart.js wrapper ──────────────────────────────────────────────────────────
// The vendored Chart.js exposes a global `Chart`. It is resolved lazily inside
// makeChart() (not at module load): app.js runs at end of <body>, before the
// deferred chart.umd.min.js executes, so window.Chart is not yet defined here.
// Nothing in this file declares a symbol named `Chart` itself (the old custom
// chart class did, and it collided with the library global).

const AXIS_FONT = { family: 'monospace', size: 11, weight: 'bold' };
const AXIS_CLR  = '#1f2328';
const GRID_CLR  = '#eaeef2';

// Format an epoch-ms x value for an axis tick; granularity depends on the
// visible span (no date-adapter library — plain Date arithmetic).
function fmtAxisTime(ms, spanMs) {
  const d = new Date(ms);
  const p = n => String(n).padStart(2, '0');
  if (spanMs <= 6  * 3600e3)
    return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
  if (spanMs <= 48 * 3600e3)
    return p(d.getHours()) + ':' + p(d.getMinutes());
  return p(d.getDate()) + '.' + p(d.getMonth() + 1) + ' ' +
         p(d.getHours()) + ':' + p(d.getMinutes());
}

// Distinct colours assigned per battery (history charts draw one line per
// battery). Cycles if there are more batteries than colours.
const BATT_COLORS = ['#0969da', '#1a7f37', '#9a6700', '#8250df',
                     '#cf222e', '#bc4c00', '#1b7c83', '#a40e26'];

// Chart.js instances live OUTSIDE the Alpine component on purpose. A Chart
// instance is a huge, deeply self-referential object (chart↔canvas↔ctx↔
// scales↔chart); if it were stored on reactive component data, Alpine's
// deep Proxy wrap would recurse forever → "Maximum call stack size
// exceeded". This module-scoped holder is never seen by Alpine reactivity.
let CHARTS = null;

// Create an empty Chart.js line chart. Lines are (re)built later via
// setSeries() so one chart can carry one line per battery.
function makeChart(id, title, yfmt) {
  const cv = document.getElementById(id);
  if (!cv) return null;
  const ChartJS = window.Chart; // resolved at call time (deferred script ran)
  if (!ChartJS) { console.error('Chart.js not loaded'); return null; }
  return new ChartJS(cv, {
    type: 'line',
    data: { datasets: [] },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      parsing: false,
      normalized: true,
      // 'nearest' (not 'index'): per-battery series are decimated
      // independently and don't share x samples (legacy and the new
      // battery don't even overlap in time), so index-matching would
      // pair unrelated far-apart points and show a wrong tooltip time.
      interaction: { mode: 'nearest', intersect: false, axis: 'x' },
      elements: {
        point: { radius: 0, hitRadius: 6 },
        line:  { borderWidth: 1.5, tension: 0.25 },
      },
      plugins: {
        title:  { display: true, text: title, align: 'start',
                  color: AXIS_CLR, font: AXIS_FONT },
        legend: { display: true,
                  labels: { boxWidth: 12, boxHeight: 3,
                            color: AXIS_CLR, font: AXIS_FONT } },
        tooltip: {
          titleFont: AXIS_FONT, bodyFont: AXIS_FONT,
          callbacks: {
            title: items => new Date(items[0].parsed.x).toLocaleString(),
            label: ctx   => ctx.dataset.label + ': ' + yfmt(ctx.parsed.y),
          },
        },
      },
      scales: {
        // Linear (numeric ms) x-axis with manual label formatting — avoids
        // pulling in a Chart.js date adapter library.
        x: {
          type: 'linear',
          bounds: 'data',
          ticks: {
            color: AXIS_CLR, font: AXIS_FONT,
            maxRotation: 0, autoSkip: true, maxTicksLimit: 8,
            callback(value, _i, ticks) {
              const span = ticks.length
                ? ticks[ticks.length - 1].value - ticks[0].value : 0;
              return fmtAxisTime(value, span);
            },
          },
          grid:  { color: GRID_CLR },
        },
        y: {
          ticks: { color: AXIS_CLR, font: AXIS_FONT, callback: v => yfmt(v) },
          grid:  { color: GRID_CLR },
        },
      },
    },
  });
}

// Replace every line on a chart. `series` = [{ label, color, points }] where
// points is [{x: tsMs, y}] already sorted ascending by x.
function setSeries(chart, series) {
  if (!chart) return;
  chart.data.datasets = series.map(s => ({
    label:           s.label,
    borderColor:     s.color,
    backgroundColor: s.color,
    data:            s.points,
    spanGaps:        true,
  }));
  chart.update('none');
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
    batteries: [],   // from /batteries; history draws one line per battery
    battery:   0,    // selected batteries.id for Realtime/Settings

    _vMin: 40, _vMax: 70,   // voltage gauge range (V)
    _iMin: -100, _iMax: 100, // current gauge range (A)

    async init() {
      // Charts are created lazily on the first History view: Chart.js v4
      // throws "Cannot set properties of undefined (setting 'fullSize')"
      // when built inside a display:none container (zero-size canvas).
      await this._fetchBatteryList();
      this._pickDefaultBattery();
      this._fetchInfo();
      // The selectable set differs per tab (reserve only in Settings); if
      // the current pick falls out of it on a tab switch, re-pick.
      this.$watch('tab', () => {
        const sel = this.selectableBatteries();
        if (sel.length && !sel.some(b => b.id === this.battery))
          this.selectBattery(sel[0].id);
      });
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
      CHARTS = {
        v: makeChart('cv', 'Pack Voltage', fV),
        c: makeChart('cc', 'Current  (+ charge / − discharge)', fA),
        s: makeChart('cs', 'SoC', fP),
        t: makeChart('ct', 'MOSFET Temperature', fT),
      };
    },

    // Default Realtime/Settings battery: first monitored, else first
    // non-legacy, else whatever is first (legacy archive only).
    // Batteries shown in the Realtime/Settings selector: monitored, non-
    // legacy only. Reserve mirrors a sibling (no own telemetry) and legacy
    // is history-only — neither has anything live to show here.
    selectableBatteries() {
      // Realtime: only real monitored batteries. Settings: also the
      // reserve (its "settings" come from config). Legacy never.
      const withReserve = this.tab === 'settings';
      return this.batteries.filter(b =>
        !b.legacy && (b.monitored || (withReserve && b.link === 'reserve')));
    },

    _pickDefaultBattery() {
      const sel = this.selectableBatteries();
      this.battery = sel.length ? sel[0].id : 0;
    },

    selectBattery(id) {
      if (id === this.battery) return;
      this.battery = id;
      this.live = {};
      this.info = null;
      this._fetchInfo();
      // History is battery-independent (all lines), no reload needed.
    },

    async _fetchInfo() {
      try {
        const r = await fetch('/info?battery=' + this.battery);
        if (r.ok) this.info = await r.json();
      } catch (_) {}
    },

    _startLivePolling() {
      const poll = async () => {
        try {
          const now = Math.floor(Date.now() / 1000);
          const q = '&battery=' + this.battery;
          const r = await fetch(`/history?time_start=${now - 10}&time_end=${now}&count=1${q}`);
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

    async _fetchBatteryList() {
      try {
        const r = await fetch('/batteries');
        if (r.ok) {
          const list = await r.json();
          if (Array.isArray(list)) this.batteries = list;
        }
      } catch (_) { /* old binary without /batteries: fall back to legacy */ }
    },

    // Paginated history fetch for one batteries.id (0 = no id → old binary
    // ignores the param and returns its single battery). The server keeps
    // the battery bound to req_id, so ?next needs no param.
    async _fetchHistoryFor(id, t0s, t1s, per) {
      const q = '&battery=' + id;
      let r = await fetch(`/history?time_start=${t0s}&time_end=${t1s}&count=${per}${q}`);
      let j = await r.json();
      let all = [...j.data];
      while (j.data.length === per) {
        r = await fetch('/history?next=1&req_id=' + j.req_id);
        if (!r.ok) break;
        j = await r.json();
        if (j.error || !j.data.length) break;
        all = all.concat(j.data);
      }
      all.sort((a, b) => a.ts - b.ts); // Chart.js wants ascending x
      return all;
    },

    async loadHistory() {
      this.histStatus = 'loading…';
      this._histLastLoad = Date.now();
      try {
        if (!this.batteries.length) await this._fetchBatteryList();
        const now = Date.now(), t0 = now - this.histHours * 36e5, per = 600;
        const t0s = Math.floor(t0 / 1e3), t1s = Math.floor(now / 1e3);

        // One line per battery in /batteries (that table IS the list, so
        // every entry has an id). Reserve isn't there (no samples → no row);
        // legacy is, with name "". When /batteries is unavailable (old
        // binary) fall back to a single id-less query it will ignore.
        const list = this.batteries.length
          ? this.batteries
          : [{ id: 0, name: '', legacy: true }];
        const series = [];
        let total = 0;
        for (let i = 0; i < list.length; i++) {
          const b = list[i];
          const data = await this._fetchHistoryFor(b.id, t0s, t1s, per);
          if (!data.length) continue;
          const label = b.legacy || b.name === '' ? 'Архив' : b.name;
          series.push({ label, color: BATT_COLORS[i % BATT_COLORS.length], data });
          total += data.length;
        }

        // Wait for the History tab to actually be visible (Alpine applies
        // x-show on the next tick), then create the charts in a sized
        // container, or just resize them on subsequent loads.
        await this.$nextTick();
        if (!CHARTS) this._initCharts();
        Object.values(CHARTS).forEach(c => c && c.resize());

        const S = (chart, fn) => setSeries(chart, series.map(s => ({
          label:  s.label,
          color:  s.color,
          points: s.data.map(d => ({ x: d.ts, y: fn(d) })),
        })));
        S(CHARTS.v, d => d.voltage_mv);
        S(CHARTS.c, d => d.current_ma);
        S(CHARTS.s, d => d.soc_pct);
        S(CHARTS.t, d => d.mos_temp_dc); // one line per battery

        this.histStatus = series.length
          ? `${series.length} bat · ${total} pts · ${new Date().toLocaleTimeString()}`
          : 'no data';
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
      const i = this.info;
      if (!i) return [];
      if (i.reserve) {
        const fA = v => (v / 1000).toFixed(1) + ' A';
        return [
          ['Capacity',                (i.capacity_mah / 1000).toFixed(1) + ' Ah'],
          ['Charge current limit',    fA(i.charge_current_limit_ma)],
          ['Discharge current limit', fA(i.discharge_current_limit_ma)],
          ['SoC (fixed)',             i.soc_pct + ' %'],
          ['Current (fixed)',         fA(i.current_ma)],
        ];
      }
      return buildInfoRows(i);
    },
  };
}
