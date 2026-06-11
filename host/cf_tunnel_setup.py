#!/usr/bin/env python3
"""
Provision a Cloudflare Named Tunnel for the token-meter host bridge, entirely
via the Cloudflare API (no interactive `cloudflared tunnel login` / cert.pem).

Idempotent: re-running reuses an existing tunnel/DNS record instead of dupes.

Reads the API token from ~/.cloudflared/api_token (chmod 600).
Writes the connector run-token to ~/.cloudflared/run_token (chmod 600).

After this succeeds, run the tunnel with:
    cloudflared tunnel run --protocol http2 --token "$(cat ~/.cloudflared/run_token)"
"""

import json
import os
import sys
import urllib.request
import urllib.error

API = "https://api.cloudflare.com/client/v4"

# Fill these in with your own Cloudflare account/zone (or export as env vars).
ACCOUNT_ID = os.environ.get("CF_ACCOUNT_ID", "your-cloudflare-account-id")
ZONE_ID = os.environ.get("CF_ZONE_ID", "your-cloudflare-zone-id")
ZONE_NAME = os.environ.get("CF_ZONE_NAME", "example.com")
SUBDOMAIN = "ccusage"
HOSTNAME = f"{SUBDOMAIN}.{ZONE_NAME}"
TUNNEL_NAME = "token-meter"
LOCAL_SERVICE = "http://localhost:8787"

HOME = os.path.expanduser("~")
TOKEN_FILE = os.path.join(HOME, ".cloudflared", "api_token")
RUN_TOKEN_FILE = os.path.join(HOME, ".cloudflared", "run_token")

with open(TOKEN_FILE) as f:
    TOKEN = f.read().strip()


def api(method, path, body=None):
    url = API + path
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"Bearer {TOKEN}")
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req) as r:
            payload = json.load(r)
    except urllib.error.HTTPError as e:
        payload = json.load(e)
    if not payload.get("success"):
        print(f"  API ERROR {method} {path}: {payload.get('errors')}")
        sys.exit(1)
    return payload["result"]


def find_tunnel():
    res = api("GET", f"/accounts/{ACCOUNT_ID}/cfd_tunnel?name={TUNNEL_NAME}&is_deleted=false")
    return res[0] if res else None


def main():
    # 1) Create or reuse the tunnel (Cloudflare-managed config).
    t = find_tunnel()
    if t:
        tid = t["id"]
        print(f"  [tunnel] reuse existing '{TUNNEL_NAME}' id={tid}")
    else:
        t = api("POST", f"/accounts/{ACCOUNT_ID}/cfd_tunnel",
                {"name": TUNNEL_NAME, "config_src": "cloudflare"})
        tid = t["id"]
        print(f"  [tunnel] created '{TUNNEL_NAME}' id={tid}")

    # 2) Connector run-token -> 600 file (never printed).
    run_token = api("GET", f"/accounts/{ACCOUNT_ID}/cfd_tunnel/{tid}/token")
    fd = os.open(RUN_TOKEN_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(run_token)
    print(f"  [token] connector token written to {RUN_TOKEN_FILE}")

    # 3) Ingress: hostname -> local daemon, everything else -> 404.
    api("PUT", f"/accounts/{ACCOUNT_ID}/cfd_tunnel/{tid}/configurations",
        {"config": {"ingress": [
            {"hostname": HOSTNAME, "service": LOCAL_SERVICE},
            {"service": "http_status:404"},
        ]}})
    print(f"  [ingress] {HOSTNAME} -> {LOCAL_SERVICE}")

    # 4) DNS CNAME hostname -> <tid>.cfargotunnel.com (proxied).
    content = f"{tid}.cfargotunnel.com"
    existing = api("GET", f"/zones/{ZONE_ID}/dns_records?name={HOSTNAME}")
    rec = {"type": "CNAME", "name": HOSTNAME, "content": content,
           "proxied": True, "ttl": 1}
    if existing:
        api("PUT", f"/zones/{ZONE_ID}/dns_records/{existing[0]['id']}", rec)
        print(f"  [dns] updated CNAME {HOSTNAME} -> {content}")
    else:
        api("POST", f"/zones/{ZONE_ID}/dns_records", rec)
        print(f"  [dns] created CNAME {HOSTNAME} -> {content}")

    print(f"\n  DONE. Public URL: https://{HOSTNAME}")


if __name__ == "__main__":
    main()
