#!/usr/bin/env python3
"""UnrealGamePatcher - CDN file server (Range-capable).

Serves a CDN root directory over plain HTTP with byte-range support, plus
fault injection so client tests can exercise interruption / retries / limits
against a REAL network stack (no in-process server in the patcher itself).

Usage:
  python cdn_server.py --root <cdn-dir> [--port 18080]
      [--fail-once N]            # respond 500 to the Nth request (0-based)
      [--truncate-after N]       # send only the first TRUNCATE_BYTES of the
      [--truncate-bytes <n>]     #   Nth response then close (mid-transfer cut)

On startup prints:  READY port=<port> root=<root>
Requests are logged as:  LOG <code> <bytes> <range> <path>
"""

import argparse
import os
import re
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class CdnHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "UnrealGamePatcherCDN/1.0"
    sys_version = ""

    _lock = threading.Lock()
    _request_index = 0
    _active = 0
    _peak = 0
    _file_requests = 0
    _range_count = 0
    _error_count = 0
    _max_range_bytes = 0

    def log_message(self, fmt, *args):
        pass  # silent by default; we log structured lines ourselves

    def _log(self, code, size, rng, path):
        with CdnHandler._lock:
            if not path.startswith("/__ctl/"):
                CdnHandler._file_requests += 1
                if code == 206:
                    CdnHandler._range_count += 1
                    CdnHandler._max_range_bytes = max(CdnHandler._max_range_bytes, size)
                if code >= 500:
                    CdnHandler._error_count += 1
            print(f"LOG {code} {size} {rng or '-'} active={CdnHandler._active}/{CdnHandler._peak} {path}", flush=True)

    def do_GET(self):
        server = self.server
        with CdnHandler._lock:
            idx = CdnHandler._request_index
            CdnHandler._request_index += 1
            CdnHandler._active += 1
            CdnHandler._peak = max(CdnHandler._peak, CdnHandler._active)

        try:
            self._handle(idx, path := self.path)
        finally:
            with CdnHandler._lock:
                CdnHandler._active -= 1

    def _handle(self, idx, path):
        server = self.server
        # Control endpoint: /__ctl/fail_next  or  /__ctl/truncate_next?bytes=N
        # arms a fault for the NEXT request (tests drive faults at runtime).
        if path.startswith("/__ctl/"):
            if path.startswith("/__ctl/stats"):
                with CdnHandler._lock:
                    stats = (f"file={CdnHandler._file_requests} ranges={CdnHandler._range_count} "
                             f"errors={CdnHandler._error_count} peak={CdnHandler._peak} "
                             f"maxrange={CdnHandler._max_range_bytes}")
                self._log(200, 0, "-", path)
                body = stats.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            if path.startswith("/__ctl/fail_next"):
                with CdnHandler._lock:
                    server.fail_once = CdnHandler._request_index
                self._log(200, 0, "-", path)
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if path.startswith("/__ctl/truncate_next"):
                m = re.search(r"bytes=(\d+)", path)
                with CdnHandler._lock:
                    server.truncate_after = CdnHandler._request_index
                    server.truncate_bytes = int(m.group(1)) if m else 100
                self._log(200, 0, "-", path)
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if path.startswith("/__ctl/throttle"):
                # Per-connection rate cap in KB/s (ThreadingHTTPServer runs
                # each connection in its own thread, so concurrent connections
                # multiply the aggregate throughput).
                m = re.search(r"kbps=(\d+)", path)
                server.throttle_bps = (int(m.group(1)) * 1024) if m else 0
                self._log(200, 0, "-", path)
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self._log(404, 0, "-", path)
            self.send_error(404)
            return

        # Map URL path onto the root dir (strip leading '/', normalize).
        rel = path.lstrip("/")
        full = os.path.normpath(os.path.join(server.root, rel))
        root_norm = os.path.normpath(server.root)
        if not full.startswith(root_norm + os.sep) and full != root_norm:
            self._log(404, 0, "-", path)
            self.send_error(404)
            return

        # Fault: one request fails with 500.
        if idx == server.fail_once:
            self._log(500, 0, "-", path)
            self.send_error(500)
            return

        if not os.path.isfile(full):
            self._log(404, 0, "-", path)
            self.send_error(404)
            return

        total = os.path.getsize(full)
        start, end = 0, total - 1
        has_range = False
        range_header = self.headers.get("Range")
        if range_header and range_header.startswith("bytes="):
            m = re.match(r"bytes=(\d*)-(\d*)", range_header)
            if m:
                s, e = m.group(1), m.group(2)
                if s != "":
                    start = int(s)
                    has_range = True
                if e != "":
                    end = int(e)
                    has_range = True

        if start >= total:
            self._log(416, 0, range_header or "-", path)
            self.send_error(416)
            return
        end = min(end, total - 1)
        length = end - start + 1

        # Fault: truncate the response body (simulated interruption).
        trunc = 0
        if idx == server.truncate_after:
            trunc = server.truncate_bytes

        # Log BEFORE responding so a client that finishes reading the body is
        # guaranteed to see the LOG line when it inspects the log file.
        self._log(206 if has_range else 200, length, range_header or "-", path)

        if has_range:
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Connection", "close")
        self.end_headers()

        sent = 0
        with open(full, "rb") as f:
            f.seek(start)
            remaining = length
            if trunc:
                remaining = min(remaining, trunc)
            while remaining > 0:
                chunk = f.read(min(1 << 16, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    break
                sent += len(chunk)
                remaining -= len(chunk)
                if server.throttle_bps > 0:
                    time.sleep(len(chunk) / server.throttle_bps)

        (sent)  # body sent (kept for readability)


class CdnServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, root, fail_once, truncate_after, truncate_bytes):
        self.root = os.path.abspath(root)
        self.fail_once = fail_once
        self.truncate_after = truncate_after
        self.truncate_bytes = truncate_bytes
        self.throttle_bps = 0  # per-connection byte/s cap (0 = unlimited)
        super().__init__(addr, CdnHandler)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--port", type=int, default=18080)
    ap.add_argument("--fail-once", type=int, default=-1)
    ap.add_argument("--truncate-after", type=int, default=-1)
    ap.add_argument("--truncate-bytes", type=int, default=0)
    ap.add_argument("--log", default=None, help="write the LOG lines to a file")
    args = ap.parse_args()

    if args.log:
        sys.stdout = open(args.log, "w", encoding="utf-8", buffering=1)

    host = "127.0.0.1"
    server = CdnServer(
        (host, args.port),
        args.root,
        args.fail_once,
        args.truncate_after,
        args.truncate_bytes,
    )
    print(f"READY port={server.server_address[1]} root={server.root}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
