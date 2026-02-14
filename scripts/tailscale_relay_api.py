#!/usr/bin/env python3
"""Small LAN API for relay-side Tailscale login/logout/status.

Endpoints (default base path: /api/tailscale):
  GET  /status
  POST /login   {"authKey":"...","loginServer":"https://..."}
  POST /logout  {}

Optional protection:
  export RELAY_API_TOKEN='your-token'
  send header: X-Relay-Token: your-token
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


def run_cmd(args: list[str], timeout: int = 25) -> tuple[int, str, str]:
    proc = subprocess.run(
        args,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def json_response(handler: BaseHTTPRequestHandler, status: int, payload: dict[str, Any]) -> None:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


def build_handler(base_path: str, expected_token: str | None):
    normalized = "/" + base_path.strip("/")

    class RelayHandler(BaseHTTPRequestHandler):
        server_version = "tailscale-relay-api/0.1"

        def _path(self, suffix: str) -> str:
            return f"{normalized}/{suffix.lstrip('/')}"

        def _authorized(self) -> bool:
            if not expected_token:
                return True
            provided = self.headers.get("X-Relay-Token", "")
            return provided == expected_token

        def _read_json(self) -> dict[str, Any]:
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length) if length > 0 else b"{}"
            if not raw:
                return {}
            return json.loads(raw.decode("utf-8"))

        def do_GET(self):
            if not self._authorized():
                json_response(self, HTTPStatus.UNAUTHORIZED, {"ok": False, "error": "unauthorized"})
                return

            if self.path != self._path("status"):
                json_response(self, HTTPStatus.NOT_FOUND, {"ok": False, "error": "not found"})
                return

            code, out, err = run_cmd(["tailscale", "status", "--json"])
            if code != 0:
                json_response(
                    self,
                    HTTPStatus.BAD_GATEWAY,
                    {"ok": False, "error": "tailscale status failed", "stderr": err},
                )
                return

            try:
                status_obj = json.loads(out)
            except json.JSONDecodeError:
                status_obj = {"raw": out}

            json_response(self, HTTPStatus.OK, {"ok": True, "status": status_obj})

        def do_POST(self):
            if not self._authorized():
                json_response(self, HTTPStatus.UNAUTHORIZED, {"ok": False, "error": "unauthorized"})
                return

            if self.path == self._path("login"):
                self._handle_login()
                return

            if self.path == self._path("logout"):
                self._handle_logout()
                return

            json_response(self, HTTPStatus.NOT_FOUND, {"ok": False, "error": "not found"})

        def _handle_login(self):
            try:
                payload = self._read_json()
            except json.JSONDecodeError:
                json_response(self, HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid json"})
                return

            auth_key = str(payload.get("authKey", "")).strip()
            login_server = str(payload.get("loginServer", "")).strip()

            if not auth_key:
                json_response(self, HTTPStatus.BAD_REQUEST, {"ok": False, "error": "authKey is required"})
                return

            cmd = ["tailscale", "up", "--auth-key", auth_key, "--reset"]
            if login_server:
                cmd += ["--login-server", login_server]

            code, out, err = run_cmd(cmd, timeout=40)
            if code != 0:
                json_response(
                    self,
                    HTTPStatus.BAD_GATEWAY,
                    {
                        "ok": False,
                        "error": "tailscale up failed",
                        "stderr": err,
                        "stdout": out,
                        "command": shlex.join(cmd),
                    },
                )
                return

            json_response(
                self,
                HTTPStatus.OK,
                {
                    "ok": True,
                    "message": "tailscale login requested",
                    "stdout": out,
                    "command": shlex.join(cmd),
                },
            )

        def _handle_logout(self):
            code, out, err = run_cmd(["tailscale", "down"], timeout=20)
            if code != 0:
                json_response(
                    self,
                    HTTPStatus.BAD_GATEWAY,
                    {"ok": False, "error": "tailscale down failed", "stderr": err, "stdout": out},
                )
                return

            json_response(self, HTTPStatus.OK, {"ok": True, "message": "tailscale down completed", "stdout": out})

        def log_message(self, fmt: str, *args: Any) -> None:
            sys.stderr.write("[relay-api] " + (fmt % args) + "\n")

    return RelayHandler


def main() -> int:
    parser = argparse.ArgumentParser(description="Relay-side HTTP API for Tailscale login")
    parser.add_argument("--host", default="0.0.0.0", help="bind host (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=9080, help="bind port (default: 9080)")
    parser.add_argument(
        "--base-path",
        default="/api/tailscale",
        help="API base path (default: /api/tailscale)",
    )
    parser.add_argument(
        "--token-file",
        default="",
        help="optional file containing expected X-Relay-Token",
    )
    args = parser.parse_args()

    if args.port < 1 or args.port > 65535:
        raise SystemExit("--port must be 1..65535")

    token = os.environ.get("RELAY_API_TOKEN", "").strip()
    if args.token_file:
        token = Path(args.token_file).read_text(encoding="utf-8").strip()

    handler = build_handler(args.base_path, token if token else None)
    server = ThreadingHTTPServer((args.host, args.port), handler)

    print(f"[relay-api] listening on http://{args.host}:{args.port}{'/' + args.base_path.strip('/')}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
