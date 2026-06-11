#!/usr/bin/env python3
"""
TokenGenie — host usage bridge

Fetches the REAL per-agent (Claude Code / Codex) rate-limit utilization from
each vendor's own usage endpoint and serves a compact JSON snapshot over the
LAN (via the Cloudflare tunnel) for the ESP32-S3 round display to poll.

Data sources — both official and real-time (the same numbers the CLIs' own
`/usage` / `/status` views show):
  - Claude: GET https://api.anthropic.com/api/oauth/usage
            Bearer from ~/.claude/.credentials.json (claudeAiOauth.accessToken).
            Needs User-Agent: claude-code/<ver> + anthropic-beta header, else 429.
  - Codex:  GET https://chatgpt.com/backend-api/wham/usage
            Bearer + account_id from ~/.codex/auth.json (tokens.*).
            Needs User-Agent: codex_cli_rs/<ver> + chatgpt-account-id header.

Each agent reports a 5-hour window and a 7-day window as {util (used %),
reset_at (epoch seconds)}. The firmware shows util on the arcs and computes the
reset countdown locally (the board has its own PCF85063 RTC + NTP clock), so the
host sends no clock/timezone — only usage plus an `updated` stamp.

Endpoints:
  GET /usage    -> snapshot JSON (see build_snapshot)
  GET /healthz  -> {"ok": true, "ready": <bool>}

Tokens are re-read from disk every refresh, so whenever the Claude Code / Codex
CLIs refresh their own OAuth tokens on this machine, we pick up the new ones.
"""

import json
import os
import re
import subprocess
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser("~")

CLAUDE_CRED = os.path.join(HOME, ".claude", ".credentials.json")
CODEX_AUTH = os.path.join(HOME, ".codex", "auth.json")
CLAUDE_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
CODEX_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage"


def load_config():
    with open(os.path.join(HERE, "config.json"), "r", encoding="utf-8") as f:
        return json.load(f)


CONFIG = load_config()

# Shared snapshot, guarded by a lock. None until the first refresh completes.
_lock = threading.Lock()
_snapshot = None


# ---- helpers ---------------------------------------------------------------

def _cli_version(cmd, default):
    """Best-effort `<cli> --version` -> 'X.Y.Z'; falls back to default.

    The exact version only needs to look like a real CLI build; the endpoints
    gate on the User-Agent *prefix* (claude-code/ , codex_cli_rs/), not the
    number. Resolved once at startup.
    """
    try:
        out = subprocess.run([cmd, "--version"], capture_output=True,
                             text=True, timeout=10).stdout
        m = re.search(r"(\d+\.\d+\.\d+)", out)
        if m:
            return m.group(1)
    except Exception:
        pass
    return default


_CLAUDE_VER = _cli_version("claude", "2.0.0")
_CODEX_VER = _cli_version("codex", "0.98.0")


def _get_json(url, headers, timeout=20):
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def _rfc3339_to_epoch(s):
    """'2026-06-10T06:30:00.115857+00:00' -> epoch seconds (int), or None."""
    if not s:
        return None
    try:
        return int(datetime.fromisoformat(s).timestamp())
    except (ValueError, TypeError):
        return None


def _window(util, reset_at):
    """Normalize one window to {util: int 0-100, reset_at: epoch|None}."""
    if util is None:
        return None
    return {"util": int(round(util)), "reset_at": reset_at}


# ---- Claude: official OAuth usage endpoint ---------------------------------

def fetch_claude():
    """Return {'five_hour': win|None, 'seven_day': win|None} or None on failure."""
    try:
        with open(CLAUDE_CRED, encoding="utf-8") as f:
            cred = json.load(f)
        tok = cred.get("claudeAiOauth", {}).get("accessToken")
        if not tok:
            print("[claude] no accessToken in credentials", flush=True)
            return None
        data = _get_json(CLAUDE_USAGE_URL, {
            "Authorization": f"Bearer {tok}",
            "anthropic-beta": "oauth-2025-04-20",
            "User-Agent": f"claude-code/{_CLAUDE_VER}",
            "Content-Type": "application/json",
        })
    except urllib.error.HTTPError as e:
        print(f"[claude] HTTP {e.code} {e.reason}", flush=True)
        return None
    except Exception as e:  # network / json / file
        print(f"[claude] {type(e).__name__}: {e}", flush=True)
        return None

    def win(obj):
        if not obj:
            return None
        return _window(obj.get("utilization"), _rfc3339_to_epoch(obj.get("resets_at")))

    # seven_day == "All models" weekly window (per product decision).
    return {"five_hour": win(data.get("five_hour")),
            "seven_day": win(data.get("seven_day"))}


# ---- Codex: ChatGPT backend usage endpoint ---------------------------------

def fetch_codex():
    """Return {'five_hour': win|None, 'seven_day': win|None} or None on failure."""
    try:
        with open(CODEX_AUTH, encoding="utf-8") as f:
            auth = json.load(f)
        toks = auth.get("tokens", {})
        tok = toks.get("access_token")
        acct = toks.get("account_id", "")
        if not tok:
            print("[codex] no access_token in auth.json", flush=True)
            return None
        data = _get_json(CODEX_USAGE_URL, {
            "Authorization": f"Bearer {tok}",
            "chatgpt-account-id": acct,
            "User-Agent": f"codex_cli_rs/{_CODEX_VER}",
            "originator": "codex_cli_rs",
            "Content-Type": "application/json",
            "Accept": "application/json",
        })
    except urllib.error.HTTPError as e:
        print(f"[codex] HTTP {e.code} {e.reason}", flush=True)
        return None
    except Exception as e:
        print(f"[codex] {type(e).__name__}: {e}", flush=True)
        return None

    rl = (data or {}).get("rate_limit") or {}

    def win(obj):
        if not obj:
            return None
        # Codex reset_at is already epoch seconds.
        return _window(obj.get("used_percent"), obj.get("reset_at"))

    return {"five_hour": win(rl.get("primary_window")),
            "seven_day": win(rl.get("secondary_window"))}


# ---- snapshot --------------------------------------------------------------

def build_snapshot():
    """Assemble the JSON the firmware consumes (see module docstring)."""
    with _lock:
        prev = _snapshot or {}
    claude = fetch_claude()
    codex = fetch_codex()
    ok = claude is not None and codex is not None
    # On a failed fetch (e.g. expired token), keep the last good value so the
    # display shows slightly stale data rather than blanking.
    if claude is None:
        claude = prev.get("claude")
    if codex is None:
        codex = prev.get("codex")
    return {"updated": int(time.time()), "claude": claude, "codex": codex, "ok": ok}


def refresh_loop():
    global _snapshot
    interval = CONFIG.get("refresh_seconds", 120)
    while True:
        start = time.time()
        try:
            snap = build_snapshot()
            with _lock:
                _snapshot = snap

            def fmt(a):
                if not a:
                    return "?"
                h, d = a.get("five_hour"), a.get("seven_day")
                return (f"5h={h['util'] if h else '?'}% "
                        f"7d={d['util'] if d else '?'}%")

            print(f"[refresh] ok={snap['ok']} "
                  f"claude({fmt(snap.get('claude'))}) "
                  f"codex({fmt(snap.get('codex'))}) "
                  f"({time.time()-start:.1f}s)", flush=True)
        except Exception as e:  # never let the loop die
            print(f"[refresh] unexpected error: {e}", flush=True)
        time.sleep(max(5, interval - (time.time() - start)))


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0].rstrip("/")
        if path in ("", "/usage"):
            # When auth_key is set, /usage requires ?key=<auth_key>. The tunnel
            # makes this endpoint public, so this is the only gate on the data.
            required = CONFIG.get("auth_key", "")
            if required:
                got = parse_qs(urlparse(self.path).query).get("key", [""])[0]
                if got != required:
                    self._send(401, {"error": "unauthorized"})
                    return
            with _lock:
                snap = _snapshot
            if snap is None:
                self._send(503, {"status": "warming"})
            else:
                self._send(200, snap)
        elif path == "/healthz":
            with _lock:
                ready = _snapshot is not None
            self._send(200, {"ok": True, "ready": ready})
        else:
            self._send(404, {"error": "not found"})

    def log_message(self, *args):  # quiet default access logging
        pass


def main():
    t = threading.Thread(target=refresh_loop, daemon=True)
    t.start()
    port = CONFIG.get("port", 8787)
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"[serve] TokenGenie bridge on 0.0.0.0:{port} "
          f"(/usage, /healthz) claude-code/{_CLAUDE_VER} codex_cli_rs/{_CODEX_VER}",
          flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
