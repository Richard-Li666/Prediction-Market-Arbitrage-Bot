#!/usr/bin/env python3
"""
无显示器服务器（如 AWS EC2）上的实时曲线：仅依赖 Python **标准库** HTTP 服务，
浏览器里看动态图（与 ``live_plot_dashboard.py`` 同一套 jsonl 数据）。

推荐：SSH 本地端口转发（不把端口暴露公网）::

  # EC2 上（与策略同机、同 cwd）::
  python3 tools/live_web_dashboard.py --host 127.0.0.1 --port 8765 --data-dir data --prefix live

  # 你笔记本上::
  ssh -L 8765:127.0.0.1:8765 ec2-user@<实例IP或DNS>

  浏览器打开: http://127.0.0.1:8765/

若必须公网访问：``--host 0.0.0.0`` + 安全组放行 TCP ``--port``，并自行评估风险。

页面通过 CDN 加载 Chart.js；若实例无外网，需改用内网镜像或把 chart.umd.min.js 放到本目录并改 HTML。
"""
from __future__ import annotations

import argparse
import json
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sys

_TOOLS_DIR = Path(__file__).resolve().parent
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))

from live_plot_common import (
    JsonlTail,
    finite,
    iter_trade_markers_from_file,
    parse_chainlink_line,
    parse_series_line,
    parse_trade_line,
    rel_s_in_bucket,
    resolve_series_chain_paths,
    resolve_trades_path,
)

INDEX_HTML = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>Live — 概率 & BTC</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
<style>
 body { font-family: system-ui, sans-serif; margin: 16px; background: #121212; color: #e0e0e0; }
 h1 { font-size: 1.1rem; margin: 0 0 12px; word-break: break-all; }
 .hint { font-size: 0.85rem; color: #888; margin-bottom: 20px; }
 .wrap { max-width: 1100px; }
 canvas { background: #1e1e1e; border-radius: 8px; }
</style>
</head>
<body>
<div class="wrap">
<h1 id="title">加载中…</h1>
<p class="hint">横轴：当前 5m bucket 内已过秒数 (0–300)。绿▲买、红▼卖（来自 <code>*_trades.jsonl</code> 的 trade）。换窗会清空重画。</p>
<div><canvas id="cProb" height="220"></canvas></div>
<div style="height:16px"></div>
<div><canvas id="cBtc" height="160"></canvas></div>
</div>
<script>
const commonOpts = {
  responsive: true,
  animation: false,
  interaction: { mode: 'nearest', intersect: false },
  elements: { line: { borderWidth: 2 }, point: { radius: 0 } },
};
const probChart = new Chart(document.getElementById('cProb'), {
  type: 'line',
  data: {
    datasets: [
      { label: '理论 Up (theo_up)', borderColor: '#64b5f6', data: [] },
      { label: '市场 Up mid', borderColor: '#ffb74d', data: [] },
      { label: '理论 Down (theo_down)', borderColor: '#81c784', borderDash: [6,4], data: [] },
      { label: '市场 Down mid', borderColor: '#e57373', borderDash: [6,4], data: [] },
      { type: 'scatter', label: 'BUY Up', backgroundColor: 'rgba(130,255,130,0.95)', borderColor: '#000', borderWidth: 1, pointStyle: 'triangle', pointRadius: 9, data: [] },
      { type: 'scatter', label: 'SELL Up', backgroundColor: 'rgba(255,80,80,0.95)', borderColor: '#000', borderWidth: 1, pointStyle: 'triangle', rotation: 180, pointRadius: 9, data: [] },
      { type: 'scatter', label: 'BUY Down', backgroundColor: 'rgba(100,220,100,0.95)', borderColor: '#000', borderWidth: 1, pointStyle: 'triangle', pointRadius: 9, data: [] },
      { type: 'scatter', label: 'SELL Down', backgroundColor: 'rgba(220,60,60,0.95)', borderColor: '#000', borderWidth: 1, pointStyle: 'triangle', rotation: 180, pointRadius: 9, data: [] },
    ],
  },
  options: {
    ...commonOpts,
    scales: {
      x: { type: 'linear', title: { display: true, text: '本窗内秒' }, min: 0, max: 300 },
      y: { min: 0, max: 1, title: { display: true, text: '概率' } },
    },
  },
});
const btcChart = new Chart(document.getElementById('cBtc'), {
  type: 'line',
  data: {
    datasets: [
      { label: 'BTC', borderColor: '#cfd8dc', data: [] },
      { type: 'scatter', label: 'BUY @S', backgroundColor: 'rgba(130,255,130,0.95)', borderColor: '#000', borderWidth: 1, pointStyle: 'triangle', pointRadius: 8, data: [] },
      { type: 'scatter', label: 'SELL @S', backgroundColor: 'rgba(255,80,80,0.95)', borderColor: '#000', borderWidth: 1, pointStyle: 'triangle', rotation: 180, pointRadius: 8, data: [] },
    ],
  },
  options: {
    ...commonOpts,
    scales: {
      x: { type: 'linear', title: { display: true, text: '本窗内秒' }, min: 0, max: 300 },
      y: { title: { display: true, text: 'USD' } },
    },
  },
});
function toXY(xs, ys) {
  const out = [];
  for (let i = 0; i < xs.length; i++) out.push({ x: xs[i], y: ys[i] });
  return out;
}
async function poll() {
  try {
    const r = await fetch('/data.json', { cache: 'no-store' });
    const j = await r.json();
    document.getElementById('title').textContent = j.slug || '(无数据)';
    const xs = j.xs || [];
    probChart.data.datasets[0].data = toXY(xs, j.theo_up || []);
    probChart.data.datasets[1].data = toXY(xs, j.up_mid || []);
    probChart.data.datasets[2].data = toXY(xs, j.theo_dn || []);
    probChart.data.datasets[3].data = toXY(xs, j.dn_mid || []);
    probChart.data.datasets[4].data = toXY(j.buy_up_x || [], j.buy_up_y || []);
    probChart.data.datasets[5].data = toXY(j.sell_up_x || [], j.sell_up_y || []);
    probChart.data.datasets[6].data = toXY(j.buy_dn_x || [], j.buy_dn_y || []);
    probChart.data.datasets[7].data = toXY(j.sell_dn_x || [], j.sell_dn_y || []);
    probChart.update('none');
    const bx = j.btc_x || [];
    btcChart.data.datasets[0].label = j.btc_label || 'BTC';
    btcChart.data.datasets[0].data = toXY(bx, j.btc_y || []);
    btcChart.data.datasets[1].data = toXY(j.btc_buy_x || [], j.btc_buy_y || []);
    btcChart.data.datasets[2].data = toXY(j.btc_sell_x || [], j.btc_sell_y || []);
    btcChart.update('none');
  } catch (e) {
    console.warn(e);
  }
  setTimeout(poll, 80);
}
poll();
</script>
</body>
</html>
"""


def main() -> int:
    ap = argparse.ArgumentParser(description="Headless live chart web UI (AWS / SSH friendly)")
    ap.add_argument("--host", default="127.0.0.1", help="监听地址；公网用 0.0.0.0（注意安全组）")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--data-dir", type=Path, default=None)
    ap.add_argument("--prefix", default="live")
    ap.add_argument("--series", type=Path, default=None)
    ap.add_argument("--chainlink", type=Path, default=None)
    ap.add_argument("--trades", type=Path, default=None, help="覆盖 trades；默认 data/<prefix>_trades.jsonl")
    ap.add_argument("--max-points", type=int, default=4000)
    ap.add_argument(
        "--poll",
        type=float,
        default=0.05,
        help="读盘间隔（秒）；越小曲线越跟手，磁盘/CPU 略增。可与浏览器轮询(~80ms)配合",
    )
    args = ap.parse_args()

    cwd = Path.cwd()
    series_path, chain_path = resolve_series_chain_paths(
        series=args.series,
        chainlink=args.chainlink,
        data_dir=args.data_dir,
        prefix=args.prefix,
        cwd=cwd,
    )
    if not series_path.is_file():
        print(f"找不到 series: {series_path.resolve()}", file=sys.stderr)
        return 1
    use_chain = chain_path is not None
    trades_path = resolve_trades_path(
        trades=args.trades, data_dir=args.data_dir, prefix=args.prefix, cwd=cwd
    )

    tail_s = JsonlTail(series_path)
    tail_c = JsonlTail(chain_path) if use_chain else None
    tail_t = JsonlTail(trades_path) if trades_path else None

    marker_seen: set[tuple[int, str, str]] = set()
    bu_x: list[float] = []
    bu_y: list[float] = []
    su_x: list[float] = []
    su_y: list[float] = []
    bd_x: list[float] = []
    bd_y: list[float] = []
    sd_x: list[float] = []
    sd_y: list[float] = []
    btc_bx: list[float] = []
    btc_by: list[float] = []
    btc_sx: list[float] = []
    btc_sy: list[float] = []

    def clear_trade_markers() -> None:
        marker_seen.clear()
        bu_x.clear()
        bu_y.clear()
        su_x.clear()
        su_y.clear()
        bd_x.clear()
        bd_y.clear()
        sd_x.clear()
        sd_y.clear()
        btc_bx.clear()
        btc_by.clear()
        btc_sx.clear()
        btc_sy.clear()

    def apply_trade_marker(m: dict) -> None:
        key = (int(m["wall_ms"]), str(m["kind"]), str(m["side"]))
        if key in marker_seen:
            return
        marker_seen.add(key)
        _, rs = rel_s_in_bucket(int(m["wall_ms"]))
        mid = float(m["mid"])
        S = float(m["S"])
        side = m["side"]
        kind = m["kind"]
        if side == "Up" and finite(mid):
            if kind == "BUY":
                bu_x.append(rs)
                bu_y.append(mid)
            else:
                su_x.append(rs)
                su_y.append(mid)
        elif side == "Down" and finite(mid):
            if kind == "BUY":
                bd_x.append(rs)
                bd_y.append(mid)
            else:
                sd_x.append(rs)
                sd_y.append(mid)
        if finite(S):
            if kind == "BUY":
                btc_bx.append(rs)
                btc_by.append(S)
            else:
                btc_sx.append(rs)
                btc_sy.append(S)

    def backfill_trades(bucket_epoch: int) -> None:
        if not trades_path:
            return
        for m in iter_trade_markers_from_file(trades_path, bucket_epoch):
            apply_trade_marker(m)

    dq_x: deque[float] = deque(maxlen=args.max_points)
    dq_tu: deque[float] = deque(maxlen=args.max_points)
    dq_um: deque[float] = deque(maxlen=args.max_points)
    dq_td: deque[float] = deque(maxlen=args.max_points)
    dq_dm: deque[float] = deque(maxlen=args.max_points)
    dq_cx: deque[float] = deque(maxlen=args.max_points)
    dq_cy: deque[float] = deque(maxlen=args.max_points)
    dq_sx: deque[float] = deque(maxlen=args.max_points)
    dq_sy: deque[float] = deque(maxlen=args.max_points)
    last_bucket: int | None = None
    slug = ""
    lock = threading.Lock()

    def tail_loop() -> None:
        nonlocal last_bucket, slug
        while True:
            for line in tail_s.lines_since_last():
                row = parse_series_line(line)
                if not row:
                    continue
                with lock:
                    if last_bucket is None or row["bucket"] != last_bucket:
                        last_bucket = row["bucket"]
                        slug = f"btc-updown-5m-{last_bucket}"
                        for q in (dq_x, dq_tu, dq_um, dq_td, dq_dm, dq_cx, dq_cy, dq_sx, dq_sy):
                            q.clear()
                        clear_trade_markers()
                        backfill_trades(last_bucket)
                    _, rs = rel_s_in_bucket(row["wall_ms"])
                    dq_x.append(rs)
                    dq_tu.append(row["theo_up"])
                    dq_um.append(row["up_mid"])
                    dq_td.append(row["theo_dn"])
                    dq_dm.append(row["dn_mid"])
                    if not use_chain and finite(row["S"]):
                        dq_sx.append(rs)
                        dq_sy.append(row["S"])
            if use_chain and tail_c:
                for line in tail_c.lines_since_last():
                    rw = parse_chainlink_line(line)
                    if not rw:
                        continue
                    with lock:
                        if last_bucket is None:
                            continue
                        b, rs = rel_s_in_bucket(rw["wall_ms"])
                        if b != last_bucket:
                            continue
                        dq_cx.append(rs)
                        dq_cy.append(rw["px"])
            if tail_t:
                for line in tail_t.lines_since_last():
                    m = parse_trade_line(line)
                    if not m:
                        continue
                    with lock:
                        if last_bucket is None or m["bucket"] != last_bucket:
                            continue
                        apply_trade_marker(m)
            time.sleep(args.poll)

    threading.Thread(target=tail_loop, daemon=True).start()

    def build_json() -> bytes:
        with lock:
            btc_x = list(dq_cx)
            btc_y = list(dq_cy)
            if btc_x and btc_y:
                btc_label = "BTC (Chainlink jsonl)"
            else:
                btc_x = list(dq_sx)
                btc_y = list(dq_sy)
                btc_label = "BTC (series S)" if btc_y else ""
            return json.dumps(
                {
                    "slug": slug,
                    "xs": list(dq_x),
                    "theo_up": list(dq_tu),
                    "up_mid": list(dq_um),
                    "theo_dn": list(dq_td),
                    "dn_mid": list(dq_dm),
                    "btc_x": btc_x,
                    "btc_y": btc_y,
                    "btc_label": btc_label,
                    "buy_up_x": list(bu_x),
                    "buy_up_y": list(bu_y),
                    "sell_up_x": list(su_x),
                    "sell_up_y": list(su_y),
                    "buy_dn_x": list(bd_x),
                    "buy_dn_y": list(bd_y),
                    "sell_dn_x": list(sd_x),
                    "sell_dn_y": list(sd_y),
                    "btc_buy_x": list(btc_bx),
                    "btc_buy_y": list(btc_by),
                    "btc_sell_x": list(btc_sx),
                    "btc_sell_y": list(btc_sy),
                }
            ).encode("utf-8")

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args) -> None:
            return

        def do_GET(self) -> None:
            if self.path == "/data.json":
                raw = build_json()
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return
            if self.path in ("/", "/index.html"):
                b = INDEX_HTML.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)
                return
            self.send_error(404)

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(
        f"Web 仪表盘: http://{args.host}:{args.port}/\n"
        f"  series    : {series_path.resolve()}\n"
        f"  chainlink : {chain_path.resolve() if chain_path else '(无)'}\n"
        f"  trades    : {trades_path.resolve() if trades_path else '(无，不画买卖点)'}\n"
        "若只本机 SSH 转发，笔记本执行: ssh -L "
        f"{args.port}:127.0.0.1:{args.port} user@服务器\n"
        f"然后打开 http://127.0.0.1:{args.port}/",
        file=sys.stderr,
    )
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
