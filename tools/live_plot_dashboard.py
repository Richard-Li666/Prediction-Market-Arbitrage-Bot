#!/usr/bin/env python3
"""
实时可视化：尾随 `live_trader` / `paper_trader` 写入的 JSONL（需要图形界面 + matplotlib）。

窗口 1 — 当前 5m slug 内概率曲线：
  - Up：市场 mid（实际） vs theo_up（理论）
  - Down：市场 mid vs theo_down
  - 若存在 ``*_trades.jsonl``：叠加 **BUY ▲ / SELL ▼**（仅 ``event_type=trade`` 的成交）
窗口 2 — BTC（Chainlink / 与策略一致的 S）价格，成交点用 **S** 标在纵轴

无显示器服务器请用 ``live_web_dashboard.py`` + SSH 端口转发。

用法::

  python3 tools/live_plot_dashboard.py --data-dir data --prefix live

依赖：matplotlib
"""
from __future__ import annotations

import argparse
import sys
from collections import deque
from pathlib import Path

import numpy as np

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


def main() -> int:
    ap = argparse.ArgumentParser(description="Live series + chainlink plot dashboard")
    ap.add_argument("--data-dir", type=Path, default=None)
    ap.add_argument("--prefix", default="live")
    ap.add_argument("--series", type=Path, default=None)
    ap.add_argument("--chainlink", type=Path, default=None)
    ap.add_argument("--trades", type=Path, default=None, help="覆盖 trades 路径；默认 data/<prefix>_trades.jsonl")
    ap.add_argument("--max-points", type=int, default=4000)
    ap.add_argument("--poll", type=float, default=0.15)
    args = ap.parse_args()

    series_path, chain_path = resolve_series_chain_paths(
        series=args.series,
        chainlink=args.chainlink,
        data_dir=args.data_dir,
        prefix=args.prefix,
        cwd=Path.cwd(),
    )

    trades_path = resolve_trades_path(
        trades=args.trades, data_dir=args.data_dir, prefix=args.prefix, cwd=Path.cwd()
    )

    if not series_path.is_file():
        print(f"找不到 series 文件: {series_path.resolve()}", file=sys.stderr)
        return 1

    use_chain = chain_path is not None
    if not use_chain:
        cand = (
            args.data_dir / f"{args.prefix}_chainlink.jsonl"
            if args.data_dir
            else (Path.cwd() / "data" / f"{args.prefix}_chainlink.jsonl")
        )
        print(f"[WARN] 无 chainlink 文件，BTC 窗口用 series S: {cand}", file=sys.stderr)
    if trades_path:
        print(f"Trades (买卖点): {trades_path.resolve()}", file=sys.stderr)
    else:
        print("[WARN] 无 trades 文件，不显示买卖点", file=sys.stderr)

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("需要 matplotlib: pip install matplotlib", file=sys.stderr)
        return 2

    tail_ser = JsonlTail(series_path)
    tail_ch = JsonlTail(chain_path) if use_chain else None
    tail_tr = JsonlTail(trades_path) if trades_path else None

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

    def merge_trade(m: dict) -> None:
        if last_bucket_key is None or m["bucket"] != last_bucket_key:
            return
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

    fig1, (ax_up, ax_dn) = plt.subplots(2, 1, sharex=True, figsize=(11, 7), num="Polymarket 概率 (Up / Down)")
    fig1.subplots_adjust(hspace=0.25)

    dq_x: deque[float] = deque(maxlen=args.max_points)
    dq_tu: deque[float] = deque(maxlen=args.max_points)
    dq_um: deque[float] = deque(maxlen=args.max_points)
    dq_td: deque[float] = deque(maxlen=args.max_points)
    dq_dm: deque[float] = deque(maxlen=args.max_points)
    cur_slug = ""

    (ln_theo_u,) = ax_up.plot([], [], "b--", lw=1.4, label="理论 Up (theo_up)")
    (ln_mid_u,) = ax_up.plot([], [], color="tab:orange", lw=1.2, label="市场 Up mid")
    sc_bu = ax_up.scatter([], [], c="lime", s=95, marker="^", zorder=10, edgecolors="black", linewidths=0.5, label="BUY")
    sc_su = ax_up.scatter([], [], c="red", s=95, marker="v", zorder=10, edgecolors="black", linewidths=0.5, label="SELL")
    ax_up.set_ylabel("Up 概率")
    ax_up.set_ylim(-0.02, 1.02)
    ax_up.legend(loc="upper right", fontsize=8)
    ax_up.grid(True, alpha=0.3)

    (ln_theo_d,) = ax_dn.plot([], [], "g--", lw=1.4, label="理论 Down (theo_down)")
    (ln_mid_d,) = ax_dn.plot([], [], color="tab:red", lw=1.2, label="市场 Down mid")
    sc_bd = ax_dn.scatter([], [], c="lime", s=95, marker="^", zorder=10, edgecolors="black", linewidths=0.5, label="BUY")
    sc_sd = ax_dn.scatter([], [], c="red", s=95, marker="v", zorder=10, edgecolors="black", linewidths=0.5, label="SELL")
    ax_dn.set_ylabel("Down 概率")
    ax_dn.set_xlabel("本窗内秒数 (距 bucket 起点)")
    ax_dn.set_ylim(-0.02, 1.02)
    ax_dn.legend(loc="upper right", fontsize=8)
    ax_dn.grid(True, alpha=0.3)

    fig2, ax_b = plt.subplots(figsize=(11, 4), num="BTC 价格")
    (ln_btc,) = ax_b.plot([], [], color="0.2", lw=1.0, label="BTC / USD")
    sc_bb = ax_b.scatter([], [], c="lime", s=85, marker="^", zorder=10, edgecolors="black", linewidths=0.4, label="BUY @S")
    sc_sb = ax_b.scatter([], [], c="red", s=85, marker="v", zorder=10, edgecolors="black", linewidths=0.4, label="SELL @S")
    ax_b.set_xlabel("本窗内秒数 (距 bucket 起点)")
    ax_b.set_ylabel("价格 (USD)")
    ax_b.legend(loc="upper left", fontsize=8)
    ax_b.grid(True, alpha=0.3)

    dq_cx: deque[float] = deque(maxlen=args.max_points)
    dq_cpx: deque[float] = deque(maxlen=args.max_points)
    dq_sx: deque[float] = deque(maxlen=args.max_points)
    dq_ss: deque[float] = deque(maxlen=args.max_points)
    last_bucket_key: int | None = None

    def reset_bucket(new_bucket: int, slug: str) -> None:
        nonlocal last_bucket_key
        last_bucket_key = new_bucket
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
        for d in (dq_x, dq_tu, dq_um, dq_td, dq_dm, dq_cx, dq_cpx, dq_sx, dq_ss):
            d.clear()
        if trades_path:
            for m in iter_trade_markers_from_file(trades_path, new_bucket):
                merge_trade(m)
        fig1.suptitle(slug, fontsize=11)
        src = "Chainlink" if use_chain else "series S"
        fig2.suptitle(f"{slug} | BTC ({src})", fontsize=11)

    plt.ion()
    fig1.show()
    fig2.show()

    print("Dashboard 已打开。关闭任一窗口后退出。", file=sys.stderr)
    print(f"Series : {series_path.resolve()}", file=sys.stderr)
    if use_chain:
        print(f"Chainlink : {chain_path.resolve()}", file=sys.stderr)

    def _scatter_xy(sc, xs: list[float], ys: list[float]) -> None:
        if xs:
            sc.set_offsets(np.column_stack([xs, ys]))
        else:
            sc.set_offsets(np.empty((0, 2)))

    while plt.fignum_exists(fig1.number) and plt.fignum_exists(fig2.number):
        for line in tail_ser.lines_since_last():
            row = parse_series_line(line)
            if not row:
                continue
            slug = f"btc-updown-5m-{row['bucket']}"
            if last_bucket_key is None or row["bucket"] != last_bucket_key:
                reset_bucket(row["bucket"], slug)
            cur_slug = slug

            _, rs = rel_s_in_bucket(row["wall_ms"])
            dq_x.append(rs)
            dq_tu.append(row["theo_up"])
            dq_um.append(row["up_mid"])
            dq_td.append(row["theo_dn"])
            dq_dm.append(row["dn_mid"])
            if not use_chain and finite(row["S"]):
                dq_sx.append(rs)
                dq_ss.append(row["S"])

        if use_chain and tail_ch:
            for line in tail_ch.lines_since_last():
                rw = parse_chainlink_line(line)
                if not rw:
                    continue
                b, rs = rel_s_in_bucket(rw["wall_ms"])
                if last_bucket_key is None or b != last_bucket_key:
                    continue
                dq_cx.append(rs)
                dq_cpx.append(rw["px"])

        if tail_tr:
            for line in tail_tr.lines_since_last():
                m = parse_trade_line(line)
                if m:
                    merge_trade(m)

        if dq_x:
            xs = list(dq_x)
            ln_theo_u.set_data(xs, list(dq_tu))
            ln_mid_u.set_data(xs, list(dq_um))
            ln_theo_d.set_data(xs, list(dq_td))
            ln_mid_d.set_data(xs, list(dq_dm))
            ax_up.set_xlim(max(0, min(xs) - 2), min(305, max(xs) + 2))
            ax_dn.set_xlim(ax_up.get_xlim())

        _scatter_xy(sc_bu, bu_x, bu_y)
        _scatter_xy(sc_su, su_x, su_y)
        _scatter_xy(sc_bd, bd_x, bd_y)
        _scatter_xy(sc_sd, sd_x, sd_y)
        _scatter_xy(sc_bb, btc_bx, btc_by)
        _scatter_xy(sc_sb, btc_sx, btc_sy)

        if dq_cx and dq_cpx:
            xc = list(dq_cx)
            ln_btc.set_data(xc, list(dq_cpx))
            ln_btc.set_label("Chainlink (jsonl)")
            ax_b.set_xlim(max(0, min(xc) - 2), min(305, max(xc) + 2))
            lo, hi = min(dq_cpx), max(dq_cpx)
            pad = max(1.0, (hi - lo) * 0.02)
            ax_b.set_ylim(lo - pad, hi + pad)
            ax_b.legend(loc="upper left", fontsize=8)
        elif dq_sx and dq_ss:
            xs = list(dq_sx)
            ln_btc.set_data(xs, list(dq_ss))
            ln_btc.set_label("S from series.jsonl")
            ax_b.set_xlim(max(0, min(xs) - 2), min(305, max(xs) + 2))
            lo, hi = min(dq_ss), max(dq_ss)
            pad = max(1.0, (hi - lo) * 0.02)
            ax_b.set_ylim(lo - pad, hi + pad)
            ax_b.legend(loc="upper left", fontsize=8)
        elif cur_slug:
            ax_b.set_title("(等待 BTC 价位数据…)", fontsize=9)

        fig1.canvas.draw_idle()
        fig1.canvas.flush_events()
        fig2.canvas.draw_idle()
        fig2.canvas.flush_events()
        plt.pause(args.poll)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
