"""Shared JSONL tail + parsers for live_plot_dashboard / live_web_dashboard."""
from __future__ import annotations

import json
import os
from pathlib import Path


def bucket_epoch_from_wall_ms(wall_ms: int) -> int:
    sec = int(wall_ms) // 1000
    return (sec // 300) * 300


def rel_s_in_bucket(wall_ms: int) -> tuple[int, float]:
    b = bucket_epoch_from_wall_ms(wall_ms)
    sec = int(wall_ms) / 1000.0
    return b, sec - float(b)


def finite(x: float) -> bool:
    return x == x and abs(x) != float("inf")


class JsonlTail:
    def __init__(self, path: Path):
        self.path = path
        self._fno: int | None = None
        self._pos = 0

    def lines_since_last(self) -> list[str]:
        out: list[str] = []
        try:
            st = os.stat(self.path)
        except OSError:
            self._reset()
            return out

        if not st.st_size:
            self._reset()
            return out

        cur_ino = getattr(st, "st_ino", None)
        if cur_ino is not None and self._fno is not None and cur_ino != self._fno:
            self._reset()
        if st.st_size < self._pos:
            self._reset()
        self._fno = cur_ino

        with self.path.open("r", errors="replace") as f:
            f.seek(self._pos)
            for line in f:
                line = line.strip()
                if line:
                    out.append(line)
            self._pos = f.tell()
        return out

    def _reset(self) -> None:
        self._pos = 0
        self._fno = None


def parse_series_line(line: str) -> dict | None:
    try:
        r = json.loads(line)
    except json.JSONDecodeError:
        return None
    if r.get("event_type") != "series":
        return None
    up = r.get("up") or {}
    dn = r.get("down") or {}
    try:
        return {
            "wall_ms": int(r["local_ts_wall_ms"]),
            "bucket": int(r["active_epoch"]),
            "theo_up": float(r["theo_up"]),
            "theo_dn": float(r["theo_down"]),
            "up_mid": float(up.get("mid", float("nan"))),
            "dn_mid": float(dn.get("mid", float("nan"))),
            "S": float(r.get("S", float("nan"))),
        }
    except (KeyError, TypeError, ValueError):
        return None


def parse_chainlink_line(line: str) -> dict | None:
    try:
        r = json.loads(line)
    except json.JSONDecodeError:
        return None
    if r.get("event_type") != "chainlink":
        return None
    p = r.get("payload") or {}
    try:
        return {"wall_ms": int(r["local_ts_wall_ms"]), "px": float(p["price"])}
    except (KeyError, TypeError, ValueError):
        return None


def parse_trade_line(line: str) -> dict | None:
    """成功成交的 trade 行（BUY / SELL / FORCE_SELL），用于打点。"""
    try:
        r = json.loads(line)
    except json.JSONDecodeError:
        return None
    if r.get("event_type") != "trade":
        return None
    act = r.get("action")
    if act not in ("BUY", "SELL", "FORCE_SELL"):
        return None
    slug = r.get("slug")
    if not slug or not isinstance(slug, str):
        return None
    try:
        bucket = int(slug.rsplit("-", 1)[-1])
    except (ValueError, IndexError):
        return None
    try:
        wall_ms = int(r["local_ts_wall_ms"])
        side = str(r.get("side", ""))
        mid = float(r.get("mid", float("nan")))
        S = float(r.get("S", float("nan")))
    except (KeyError, TypeError, ValueError):
        return None
    kind = "SELL" if act in ("SELL", "FORCE_SELL") else "BUY"
    return {
        "bucket": bucket,
        "wall_ms": wall_ms,
        "side": side,
        "kind": kind,
        "mid": mid,
        "S": S,
        "slug": slug,
    }


def iter_trade_markers_from_file(trades_path: Path, bucket_epoch: int) -> list[dict]:
    """扫描整个 trades 文件，收集属于某 bucket 的成交（启动或换窗时回填）。"""
    out: list[dict] = []
    if not trades_path.is_file():
        return out
    try:
        with trades_path.open("r", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                m = parse_trade_line(line)
                if m and m["bucket"] == bucket_epoch:
                    out.append(m)
    except OSError:
        pass
    return out


def resolve_trades_path(
    *,
    trades: Path | None,
    data_dir: Path | None,
    prefix: str,
    cwd: Path,
) -> Path | None:
    if trades is not None:
        return trades if trades.is_file() else None
    if data_dir:
        p = data_dir / f"{prefix}_trades.jsonl"
    else:
        if cwd.name == "data":
            p = cwd / f"{prefix}_trades.jsonl"
        else:
            p = cwd / "data" / f"{prefix}_trades.jsonl"
    return p if p.is_file() else None


def resolve_series_chain_paths(
    *,
    series: Path | None,
    chainlink: Path | None,
    data_dir: Path | None,
    prefix: str,
    cwd: Path,
) -> tuple[Path, Path | None]:
    if series:
        series_path = series
    elif data_dir:
        series_path = data_dir / f"{prefix}_series.jsonl"
    else:
        if cwd.name == "data":
            series_path = cwd / f"{prefix}_series.jsonl"
        else:
            series_path = cwd / "data" / f"{prefix}_series.jsonl"

    if chainlink is not None:
        chain_path = chainlink
    elif data_dir:
        chain_path = data_dir / f"{prefix}_chainlink.jsonl"
    else:
        if cwd.name == "data":
            chain_path = cwd / f"{prefix}_chainlink.jsonl"
        else:
            chain_path = cwd / "data" / f"{prefix}_chainlink.jsonl"
    return series_path, chain_path if chain_path.is_file() else None
