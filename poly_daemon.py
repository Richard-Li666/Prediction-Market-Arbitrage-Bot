import json
import os
import sys
import time
import traceback
from pathlib import Path

import numpy as np

try:
    from arch import arch_model as _arch_model
except ImportError:
    _arch_model = None

from py_clob_client_v2.client import ClobClient
from py_clob_client_v2.clob_types import (
    ApiCreds,
    AssetType,
    BalanceAllowanceParams,
    MarketOrderArgsV2,
    OrderArgsV2,
    OrderType,
)


def _require_env(k: str) -> str:
    v = os.getenv(k)
    if not v:
        raise RuntimeError(f"missing required env: {k}")
    return v


def _load_dotenv_if_present(path: str = ".env") -> None:
    """
    Best-effort .env loader (no external deps).
    - Supports lines like KEY=VALUE and optional `export KEY=VALUE`
    - Ignores blank lines and comments (#...)
    - Only sets env var if it is not already present
    """
    p = Path(path)
    if not p.exists() or not p.is_file():
        return
    try:
        for raw in p.read_text().splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("export "):
                line = line[len("export ") :].strip()
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            k = k.strip()
            v = v.strip()
            if not k or os.getenv(k) is not None:
                continue
            if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
                v = v[1:-1]
            os.environ[k] = v
    except Exception:
        # best-effort; do not crash daemon for dotenv parsing
        return


HOST = os.getenv("POLY_HOST", "https://clob.polymarket.com")
CHAIN_ID = int(os.getenv("POLY_CHAIN_ID", os.getenv("POLY_CHAIN", "137")))

_load_dotenv_if_present(".env")

HOST = os.getenv("POLY_HOST", "https://clob.polymarket.com")
CHAIN_ID = int(os.getenv("POLY_CHAIN_ID", os.getenv("POLY_CHAIN", "137")))

PRIVATE_KEY = _require_env("PRIVATE_KEY")
FUNDER = _require_env("POLY_FUNDER")
SIG_TYPE = int(os.getenv("POLY_SIGNATURE_TYPE", "1"))  # 1 = POLY_PROXY


def reply(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def _json_truthy(x) -> bool:
    if x is True:
        return True
    if x is False or x is None:
        return False
    if isinstance(x, (int, float)):
        return x != 0
    return str(x).strip().lower() in ("1", "true", "yes", "y", "on")


def main():
    # L2 creds: prefer POLY_API_KEY / POLY_SECRET / POLY_PASSPHRASE from env; else derive via L1.
    api_key = os.getenv("POLY_API_KEY")
    api_secret = os.getenv("POLY_SECRET")
    api_passphrase = os.getenv("POLY_PASSPHRASE")

    if api_key and api_secret and api_passphrase:
        creds = ApiCreds(api_key=api_key, api_secret=api_secret, api_passphrase=api_passphrase)
    else:
        tmp = ClobClient(host=HOST, chain_id=CHAIN_ID, key=PRIVATE_KEY)
        derived = tmp.create_or_derive_api_key()
        # py_clob_client_v2.ApiCreds: api_key, api_secret, api_passphrase
        creds = ApiCreds(
            api_key=derived.api_key,
            api_secret=derived.api_secret,
            api_passphrase=derived.api_passphrase,
        )

    client = ClobClient(
        host=HOST,
        chain_id=CHAIN_ID,
        key=PRIVATE_KEY,
        creds=creds,
        signature_type=SIG_TYPE,
        funder=FUNDER,
    )

    reply(
        {
            "ok": True,
            "event": "ready",
            "host": HOST,
            "chain_id": CHAIN_ID,
            "signature_type": SIG_TYPE,
            "funder": FUNDER,
        }
    )

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            cmd = req.get("cmd")

            if cmd == "place_market_order":
                token_id = req["token_id"]
                side = req["side"]
                amount = float(req["amount"])
                # Default to IOC to allow partial fills; FOK will often fail on thin books.
                ot_name = req.get("order_type") or os.getenv("POLY_MARKET_ORDER_TYPE", "IOC")
                ot = getattr(OrderType, ot_name, OrderType.FOK)

                if _json_truthy(req.get("dry_run")):
                    reply(
                        {
                            "ok": True,
                            "cmd": cmd,
                            "dry_run": True,
                            "would": {
                                "token_id": token_id,
                                "side": side,
                                "amount": amount,
                                "order_type": ot_name,
                            },
                            "ts_ms": int(time.time() * 1000),
                        }
                    )
                    continue

                moa = MarketOrderArgsV2(token_id=token_id, amount=amount, side=side, order_type=ot)
                resp = client.create_and_post_market_order(moa, order_type=ot)
                reply({"ok": True, "cmd": cmd, "resp": resp, "ts_ms": int(time.time() * 1000)})
                continue

            if cmd == "place_order":
                token_id = req["token_id"]
                side = req["side"]
                price = float(req["price"])
                size = float(req["size"])
                order_type = req.get("order_type", "GTC")

                if _json_truthy(req.get("dry_run")):
                    reply(
                        {
                            "ok": True,
                            "cmd": cmd,
                            "dry_run": True,
                            "would": {
                                "token_id": token_id,
                                "side": side,
                                "price": price,
                                "size": size,
                                "order_type": order_type,
                            },
                            "ts_ms": int(time.time() * 1000),
                        }
                    )
                    continue

                oa = OrderArgsV2(token_id=token_id, price=price, size=size, side=side)
                resp = client.create_and_post_order(oa, order_type=order_type)
                # Keep raw resp for now; C++ extracts order id if present.
                reply({"ok": True, "cmd": cmd, "resp": resp, "ts_ms": int(time.time() * 1000)})
                continue

            if cmd == "get_conditional_balance":
                token_id = str(req["token_id"])
                params = BalanceAllowanceParams(
                    asset_type=AssetType.CONDITIONAL,
                    token_id=token_id,
                )
                raw = client.get_balance_allowance(params)
                balance_shares = None
                if isinstance(raw, dict):
                    b = raw.get("balance")
                    if b is not None:
                        try:
                            # CLOB returns balance in 1e6-scale fixed point for conditional tokens (see API errors).
                            balance_shares = float(b) / 1.0e6
                        except (TypeError, ValueError):
                            try:
                                balance_shares = float(b)
                            except (TypeError, ValueError):
                                balance_shares = None
                if balance_shares is None:
                    reply(
                        {
                            "ok": False,
                            "cmd": cmd,
                            "error": "could not parse conditional balance",
                            "raw": raw,
                            "ts_ms": int(time.time() * 1000),
                        }
                    )
                    continue
                reply(
                    {
                        "ok": True,
                        "cmd": cmd,
                        "balance": balance_shares,
                        "raw": raw,
                        "ts_ms": int(time.time() * 1000),
                    }
                )
                continue

            if cmd == "cancel_order":
                # Not wired for now: py-clob-client-v2 expects an OrderPayload object, not an order_id string.
                reply({"ok": False, "cmd": cmd, "error": "cancel_order not implemented in poly_daemon.py yet"})
                continue

            if cmd == "garch_forecast":
                mids = req["mids"]
                step_ms = int(req.get("step_ms", 300))
                p = int(req.get("p", 1))
                q = int(req.get("q", 1))

                if _arch_model is None:
                    reply({"ok": False, "cmd": cmd, "error": "arch package not installed", "ts_ms": int(time.time() * 1000)})
                    continue

                mids_arr = np.array(mids, dtype=float)
                if len(mids_arr) < 50:
                    reply({"ok": False, "cmd": cmd, "error": f"too few data points: {len(mids_arr)}", "ts_ms": int(time.time() * 1000)})
                    continue

                rets = np.diff(np.log(mids_arr))
                rets = rets[np.isfinite(rets)]
                if len(rets) < 50:
                    reply({"ok": False, "cmd": cmd, "error": f"too few valid returns: {len(rets)}", "ts_ms": int(time.time() * 1000)})
                    continue

                y = rets * 100.0
                try:
                    am = _arch_model(y, mean="Zero", vol="GARCH", p=p, q=q, dist="normal")
                    res = am.fit(disp="off")
                    fc = res.forecast(horizon=1, reindex=False)
                    var_next_pct2 = float(fc.variance.values[-1, 0])
                    sigma_next_step = np.sqrt(var_next_pct2) / 100.0
                    steps_per_year = (365.0 * 24.0 * 3600.0 * 1000.0) / step_ms
                    sigma_annual = float(sigma_next_step * np.sqrt(steps_per_year))
                    reply({"ok": True, "cmd": cmd, "sigma_annual": sigma_annual, "n_returns": len(rets), "ts_ms": int(time.time() * 1000)})
                except Exception as e:
                    reply({"ok": False, "cmd": cmd, "error": str(e), "trace": traceback.format_exc(), "ts_ms": int(time.time() * 1000)})
                continue

            if cmd == "ping":
                reply({"ok": True, "cmd": cmd, "ts_ms": int(time.time() * 1000)})
                continue

            reply({"ok": False, "error": f"unknown cmd: {cmd}", "ts_ms": int(time.time() * 1000)})
        except Exception as e:
            reply(
                {
                    "ok": False,
                    "error": str(e),
                    "trace": traceback.format_exc(),
                    "ts_ms": int(time.time() * 1000),
                }
            )


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        reply(
            {
                "ok": False,
                "event": "fatal",
                "error": str(e),
                "trace": traceback.format_exc(),
                "ts_ms": int(time.time() * 1000),
            }
        )
        raise SystemExit(1)

