#!/usr/bin/env python3
"""
Reverse HTTP proxy for STM32 board on direct Ethernet.

Phone / Internet -> VPS:8080 -ssh-> PC proxy:8080 -> board :80
Also advertises love_clicker.local via mDNS on LAN.

On board link loss (e.g. reset), stops HTTP proxy + SSH and brings them
back up once the board answers again — no manual script restart needed.

Requires: pip install -r requirements.txt, OpenSSH client, VPS key.
"""

from __future__ import annotations

import argparse
import http.client
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

try:
    from zeroconf import ServiceInfo, Zeroconf
except ImportError:
    print("Missing dependency: pip install -r requirements.txt", file=sys.stderr)
    sys.exit(1)


def app_dir() -> Path:
    """Directory with the script or frozen .exe (not PyInstaller _MEI* extract dir)."""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def default_ssh_key_path() -> Path:
    name = "privatekey-1122907.pem"
    candidates = [
        app_dir() / name,
        Path.cwd() / name,
    ]
    for path in candidates:
        if path.is_file():
            return path
    return candidates[0]


DEFAULT_SSH_HOST = "195.209.218.245"
DEFAULT_SSH_USER = "ubuntu"
DEFAULT_SSH_KEY = str(default_ssh_key_path())
DEFAULT_SSH_REMOTE_PORT = 8080
BOARD_HEALTH_INTERVAL_S = 2.0
BOARD_HEALTH_FAILS = 3
BOARD_WAIT_INTERVAL_S = 2.0

HOP_BY_HOP = {
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailers",
    "transfer-encoding",
    "upgrade",
}

# Browser noise — answer locally so the board sees one request at a time.
NOISE_PATHS = {
    "/favicon.ico",
    "/robots.txt",
}
NOISE_PREFIXES = ("/apple-touch-icon",)

DEFAULT_BOARD_IP = "192.168.11.1"


class ReusableThreadingHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def probe_tcp(host: str, port: int, timeout: float = 0.4) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def probe_board_http(host: str, port: int, timeout: float = 1.5) -> bool:
    """True if board HTTP stack answers (used for reboot recovery)."""
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request(
            "GET",
            "/api/love",
            headers={"Host": host, "Connection": "close", "Cache-Control": "no-store"},
        )
        resp = conn.getresponse()
        resp.read()
        return resp.status < 500
    except OSError:
        return False
    finally:
        try:
            conn.close()
        except OSError:
            pass


def board_ip_candidates() -> list[str]:
    """Prefer board DHCP server (.1), then .1 of any local Ethernet-like address."""
    seen: list[str] = []

    def add(ip: str) -> None:
        if ip and ip not in seen:
            seen.append(ip)

    add(DEFAULT_BOARD_IP)
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip.startswith("127."):
                continue
            parts = ip.split(".")
            if len(parts) == 4:
                add(".".join(parts[:3] + ["1"]))
    except OSError:
        pass
    return seen


def discover_board_ip(explicit: str | None, port: int) -> str:
    if explicit:
        return explicit
    for ip in board_ip_candidates():
        if probe_tcp(ip, port):
            return ip
    return DEFAULT_BOARD_IP


def wait_for_board(
    explicit: str | None,
    port: int,
    stop: threading.Event,
) -> str | None:
    """Block until board HTTP is up (or stop). Returns board IP."""
    while not stop.is_set():
        ip = discover_board_ip(explicit, port)
        if probe_board_http(ip, port):
            return ip
        print(f"[watch] board not ready ({ip}:{port}), retrying...")
        if stop.wait(BOARD_WAIT_INTERVAL_S):
            break
    return None


def pick_advertise_ip(exclude_prefix: str) -> str:
    """Prefer default-route interface (usually Wi-Fi), skip board Ethernet subnet."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        if not ip.startswith(exclude_prefix):
            return ip
    except OSError:
        pass
    finally:
        sock.close()

    for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
        ip = info[4][0]
        if ip.startswith("127.") or ip.startswith(exclude_prefix):
            continue
        return ip

    raise RuntimeError(
        "Could not detect Wi-Fi IP. Set --advertise-ip manually."
    )


def request_path(path: str) -> str:
    return path.split("?", 1)[0]


def is_noise_path(path: str) -> bool:
    p = request_path(path)
    if p in NOISE_PATHS:
        return True
    return any(p.startswith(prefix) for prefix in NOISE_PREFIXES)


def board_request_headers(board_host: str, *, keep_alive: bool = False) -> dict[str, str]:
    """Minimal request to the board — never forward browser headers."""
    return {
        "Host": board_host,
        "Connection": "keep-alive" if keep_alive else "close",
        "Accept": "text/event-stream" if keep_alive else "*/*",
    }


def make_handler(board_host: str, board_port: int) -> type[BaseHTTPRequestHandler]:
    class BoardProxyHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt: str, *args) -> None:
            print(f"[proxy] {self.address_string()} - {fmt % args}")

        def _send_bad_gateway(self, exc: BaseException) -> None:
            print(f"[proxy] upstream error: {exc}", file=sys.stderr)
            if self.wfile.closed:
                return
            try:
                self.send_error(502, "Bad Gateway")
            except OSError:
                pass

        def _respond_noise(self) -> None:
            if self.command == "HEAD":
                self.send_response(204)
                self.end_headers()
                return
            self.send_response(204)
            self.end_headers()

        def _forward(self) -> None:
            if is_noise_path(self.path):
                self._respond_noise()
                return

            is_sse = request_path(self.path) == "/api/events"
            length = int(self.headers.get("Content-Length", "0") or "0")
            body = self.rfile.read(length) if length > 0 else None

            # SSE streams forever — no short socket timeout
            upstream = http.client.HTTPConnection(
                board_host, board_port, timeout=None if is_sse else 10
            )
            headers_sent = False
            try:
                upstream.request(
                    self.command,
                    self.path,
                    body=body,
                    headers=board_request_headers(board_host, keep_alive=is_sse),
                )
                response = upstream.getresponse()
                self.send_response(response.status, response.reason)

                for key, value in response.getheaders():
                    if key.lower() in HOP_BY_HOP:
                        continue
                    self.send_header(key, value)
                if is_sse:
                    self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                headers_sent = True

                while True:
                    if is_sse and hasattr(response, "read1"):
                        chunk = response.read1(4096)
                    else:
                        chunk = response.read(4096)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    try:
                        self.wfile.flush()
                    except OSError:
                        break
            except OSError as exc:
                if headers_sent:
                    print(
                        f"[proxy] upstream dropped mid-response: {exc}",
                        file=sys.stderr,
                    )
                else:
                    self._send_bad_gateway(exc)
            finally:
                upstream.close()

        def do_GET(self) -> None:
            self._forward()

        def do_HEAD(self) -> None:
            self._forward()

        def do_POST(self) -> None:
            self._forward()

    return BoardProxyHandler


class MdnsAdvertiser:
    def __init__(
        self,
        hostname: str,
        ip: str,
        port: int,
        service_name: str,
    ) -> None:
        self._zc = Zeroconf()
        fqdn = f"{hostname}.local."
        self._info = ServiceInfo(
            "_http._tcp.local.",
            f"{service_name}._http._tcp.local.",
            addresses=[socket.inet_aton(ip)],
            port=port,
            properties={},
            server=fqdn,
        )

    def register(self) -> None:
        self._zc.register_service(self._info)

    def close(self) -> None:
        self._zc.unregister_service(self._info)
        self._zc.close()


class SshReverseTunnel:
    """Background `ssh -R` tunnel; killed when stop() is called."""

    def __init__(
        self,
        *,
        host: str,
        user: str,
        identity_file: str,
        local_port: int,
        remote_port: int,
        remote_bind: str = "0.0.0.0",
        local_host: str = "127.0.0.1",
    ) -> None:
        self._host = host
        self._user = user
        self._identity_file = identity_file
        self._local_port = local_port
        self._remote_port = remote_port
        self._remote_bind = remote_bind
        self._local_host = local_host
        self._stop = threading.Event()
        self._proc: subprocess.Popen[str] | None = None
        self._thread: threading.Thread | None = None

    @property
    def public_url(self) -> str:
        return f"http://{self._host}:{self._remote_port}/"

    def start(self) -> None:
        if shutil.which("ssh") is None:
            raise RuntimeError("ssh not found in PATH (install OpenSSH client)")
        if not Path(self._identity_file).is_file():
            raise RuntimeError(
                f"SSH key not found: {self._identity_file} "
                "(put privatekey-1122907.pem next to the .exe / proxy.py)"
            )

        self._stop.clear()
        self._thread = threading.Thread(
            target=self._run,
            name="SshReverseTunnel",
            daemon=True,
        )
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._kill_proc()
        if self._thread is not None:
            self._thread.join(timeout=5)
            self._thread = None

    def _kill_proc(self) -> None:
        proc = self._proc
        if proc is None:
            return
        if proc.poll() is None:
            try:
                proc.terminate()
                proc.wait(timeout=3)
            except Exception:
                try:
                    proc.kill()
                except OSError:
                    pass
        self._proc = None

    def _build_cmd(self) -> list[str]:
        remote = f"{self._remote_bind}:{self._remote_port}:{self._local_host}:{self._local_port}"
        return [
            "ssh",
            "-N",
            "-i",
            self._identity_file,
            "-o",
            "BatchMode=yes",
            "-o",
            "ExitOnForwardFailure=yes",
            "-o",
            "ServerAliveInterval=30",
            "-o",
            "ServerAliveCountMax=3",
            "-o",
            "StrictHostKeyChecking=accept-new",
            "-R",
            remote,
            f"{self._user}@{self._host}",
        ]

    def _run(self) -> None:
        backoff = 2.0
        while not self._stop.is_set():
            cmd = self._build_cmd()
            print(
                f"[ssh] tunnel: {self._remote_bind}:{self._remote_port}"
                f" -> {self._local_host}:{self._local_port} via {self._user}@{self._host}"
            )
            try:
                self._proc = subprocess.Popen(
                    cmd,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=None,
                )
            except OSError as exc:
                print(f"[ssh] failed to start: {exc}", file=sys.stderr)
                if self._stop.wait(backoff):
                    break
                backoff = min(backoff * 2, 30.0)
                continue

            assert self._proc is not None
            while not self._stop.is_set():
                code = self._proc.poll()
                if code is not None:
                    print(f"[ssh] exited ({code})", file=sys.stderr)
                    self._proc = None
                    break
                time.sleep(0.3)

            if self._stop.is_set():
                self._kill_proc()
                break

            if self._stop.wait(backoff):
                break
            backoff = min(backoff * 2, 30.0)
            print("[ssh] reconnecting...")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="HTTP reverse proxy to STM32 board + mDNS .local name"
    )
    parser.add_argument(
        "--board-ip",
        default="",
        help="Board IP (default: auto-discover, usually 192.168.11.1)",
    )
    parser.add_argument(
        "--board-port",
        type=int,
        default=80,
        help="Board HTTP port (default: 80)",
    )
    parser.add_argument(
        "--listen-host",
        default="0.0.0.0",
        help="Listen address (default: all interfaces / Wi-Fi)",
    )
    parser.add_argument(
        "--listen-port",
        type=int,
        default=8080,
        help="Local HTTP port for phone (default: 8080)",
    )
    parser.add_argument(
        "--hostname",
        default="love_clicker",
        help="mDNS hostname without .local (default: love_clicker)",
    )
    parser.add_argument(
        "--service-name",
        default="love_clicker",
        help="mDNS service instance name (default: love_clicker)",
    )
    parser.add_argument(
        "--advertise-ip",
        default="",
        help="Wi-Fi IP to advertise (auto-detect if omitted)",
    )
    parser.add_argument(
        "--no-mdns",
        action="store_true",
        help="Do not register love_clicker.local via mDNS",
    )
    parser.add_argument(
        "--no-ssh",
        action="store_true",
        help="Do not open SSH reverse tunnel to VPS",
    )
    parser.add_argument(
        "--ssh-host",
        default=DEFAULT_SSH_HOST,
        help=f"VPS host (default: {DEFAULT_SSH_HOST})",
    )
    parser.add_argument(
        "--ssh-user",
        default=DEFAULT_SSH_USER,
        help=f"SSH user (default: {DEFAULT_SSH_USER})",
    )
    parser.add_argument(
        "--ssh-key",
        default=DEFAULT_SSH_KEY,
        help=f"SSH private key path (default: {DEFAULT_SSH_KEY})",
    )
    parser.add_argument(
        "--ssh-remote-port",
        type=int,
        default=DEFAULT_SSH_REMOTE_PORT,
        help=f"Remote port on VPS (default: {DEFAULT_SSH_REMOTE_PORT})",
    )
    return parser.parse_args()


def run_session(
    *,
    args: argparse.Namespace,
    board_ip: str,
    stop: threading.Event,
) -> None:
    """Run HTTP proxy + SSH until board is lost or global stop."""
    handler = make_handler(board_ip, args.board_port)
    server = ReusableThreadingHTTPServer(
        (args.listen_host, args.listen_port),
        handler,
    )

    ssh: SshReverseTunnel | None = None
    if not args.no_ssh:
        ssh = SshReverseTunnel(
            host=args.ssh_host,
            user=args.ssh_user,
            identity_file=os.path.expanduser(args.ssh_key),
            local_port=args.listen_port,
            remote_port=args.ssh_remote_port,
        )
        try:
            ssh.start()
        except RuntimeError as exc:
            print(f"[ssh] disabled: {exc}", file=sys.stderr)
            ssh = None

    session_done = threading.Event()

    def watchdog() -> None:
        fails = 0
        while not stop.is_set() and not session_done.is_set():
            if probe_board_http(board_ip, args.board_port):
                fails = 0
            else:
                fails += 1
                print(
                    f"[watch] board health fail {fails}/{BOARD_HEALTH_FAILS}",
                    file=sys.stderr,
                )
                if fails >= BOARD_HEALTH_FAILS:
                    print(
                        "[watch] board lost — restarting proxy + SSH",
                        file=sys.stderr,
                    )
                    session_done.set()
                    server.shutdown()
                    return
            if stop.wait(BOARD_HEALTH_INTERVAL_S) or session_done.wait(0):
                break

    wd = threading.Thread(target=watchdog, name="BoardWatchdog", daemon=True)
    wd.start()

    print(f"[watch] session up — board http://{board_ip}:{args.board_port}/")
    try:
        server.serve_forever()
    finally:
        session_done.set()
        if ssh is not None:
            print("[watch] stopping SSH...")
            ssh.stop()
        server.server_close()
        wd.join(timeout=BOARD_HEALTH_INTERVAL_S + 1.0)
        print("[watch] session stopped")


def main() -> int:
    args = parse_args()
    stop = threading.Event()

    board_guess = discover_board_ip(args.board_ip or None, args.board_port)
    board_prefix = ".".join(board_guess.split(".")[:3]) + "."
    advertise_ip = args.advertise_ip or pick_advertise_ip(board_prefix)

    mdns: MdnsAdvertiser | None = None
    if not args.no_mdns:
        mdns = MdnsAdvertiser(
            hostname=args.hostname,
            ip=advertise_ip,
            port=args.listen_port,
            service_name=args.service_name,
        )
        mdns.register()

    url = f"http://{args.hostname}.local:{args.listen_port}/"
    fallback = f"http://{advertise_ip}:{args.listen_port}/"
    public = f"http://{args.ssh_host}:{args.ssh_remote_port}/"

    print("Love Clicker proxy (auto-restart on board loss)")
    print(f"  listen:           {args.listen_host}:{args.listen_port}")
    print(f"  mDNS:             {url}")
    print(f"  fallback (Wi-Fi): {fallback}")
    if not args.no_ssh:
        print(f"  public (VPS):     {public}")
    print("Press Ctrl+C to stop.")

    try:
        while not stop.is_set():
            print("[watch] waiting for board...")
            board_ip = wait_for_board(args.board_ip or None, args.board_port, stop)
            if board_ip is None or stop.is_set():
                break
            try:
                run_session(args=args, board_ip=board_ip, stop=stop)
            except OSError as exc:
                print(f"[watch] proxy bind/error: {exc}", file=sys.stderr)
            if stop.is_set():
                break
            # Brief pause before rebinding listen port / SSH
            if stop.wait(1.0):
                break
    except KeyboardInterrupt:
        print("\nStopping...")
        stop.set()
    finally:
        stop.set()
        if mdns is not None:
            mdns.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
