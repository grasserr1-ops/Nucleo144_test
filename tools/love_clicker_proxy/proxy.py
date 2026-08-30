#!/usr/bin/env python3
"""
Reverse HTTP proxy for STM32 board on direct Ethernet.

Phone (Wi-Fi) -> http://love_clicker.local:8080/ -> PC proxy -> board :80

Requires: pip install -r requirements.txt
Allow inbound TCP on LISTEN_PORT and UDP 5353 in Windows Firewall (private network).
"""

from __future__ import annotations

import argparse
import http.client
import socket
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from zeroconf import ServiceInfo, Zeroconf
except ImportError:
    print("Missing dependency: pip install -r requirements.txt", file=sys.stderr)
    sys.exit(1)

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

# STM32 HTTP server handles one connection at a time; serialize upstream access.
_board_lock = threading.Lock()


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


def board_request_headers(board_host: str) -> dict[str, str]:
    """Minimal request to the board — never forward browser headers."""
    return {
        "Host": board_host,
        "Connection": "close",
        "Accept": "*/*",
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

            length = int(self.headers.get("Content-Length", "0") or "0")
            body = self.rfile.read(length) if length > 0 else None

            upstream = http.client.HTTPConnection(
                board_host, board_port, timeout=10
            )
            headers_sent = False
            try:
                with _board_lock:
                    upstream.request(
                        self.command,
                        self.path,
                        body=body,
                        headers=board_request_headers(board_host),
                    )
                    response = upstream.getresponse()
                    self.send_response(response.status, response.reason)

                    for key, value in response.getheaders():
                        if key.lower() in HOP_BY_HOP:
                            continue
                        self.send_header(key, value)
                    self.end_headers()
                    headers_sent = True

                    while True:
                        chunk = response.read(4096)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="HTTP reverse proxy to STM32 board + mDNS .local name"
    )
    parser.add_argument(
        "--board-ip",
        default="192.168.11.101",
        help="Board static IP on direct Ethernet (default: 192.168.11.101)",
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
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    board_prefix = ".".join(args.board_ip.split(".")[:3]) + "."
    advertise_ip = args.advertise_ip or pick_advertise_ip(board_prefix)

    handler = make_handler(args.board_ip, args.board_port)
    server = ThreadingHTTPServer(
        (args.listen_host, args.listen_port),
        handler,
    )

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

    print("Love Clicker proxy")
    print(f"  board (Ethernet): http://{args.board_ip}:{args.board_port}/")
    print(f"  listen:           {args.listen_host}:{args.listen_port}")
    print(f"  mDNS:             {url}")
    print(f"  fallback (Wi-Fi): {fallback}")
    print("Press Ctrl+C to stop.")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        server.shutdown()
        if mdns is not None:
            mdns.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
