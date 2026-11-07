"""Serve one file over HTTP on the local network, with Range requests.

For getting the disc image onto a 3DS with a downloader such as 3dsDL,
which splits a download over several connections and resumes after a
dropped one; both need Range. Only the given file is served, at
/<its name>; every other path is a 404.

    python tools/serve_disc.py "path/to/Pokemon Colosseum (USA).iso" --name GC6E01.iso
"""
import argparse
import os
import re
import socket
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

CHUNK = 256 * 1024


def lan_address():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.1", 9))      # no packet is sent; picks the LAN interface
        return s.getsockname()[0]
    finally:
        s.close()


def make_handler(path, name):
    size = path.stat().st_size

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            sys.stdout.write("%s %s - %s\n" % (time.strftime("%H:%M:%S"), self.client_address[0], fmt % args))
            sys.stdout.flush()

        def range(self):
            m = re.fullmatch(r"bytes=(\d*)-(\d*)", self.headers.get("Range", "").strip())
            if not m or (not m.group(1) and not m.group(2)):
                return None
            if not m.group(1):                          # the last n bytes
                n = int(m.group(2))
                return max(0, size - n), size - 1
            start = int(m.group(1))
            end = int(m.group(2)) if m.group(2) else size - 1
            return start, min(end, size - 1)

        def head(self):
            if self.path.split("?")[0] != "/" + name:
                self.send_error(404)
                return None
            r = self.range() if "Range" in self.headers else None
            if r and r[0] > r[1]:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None
            start, end = r if r else (0, size - 1)
            self.send_response(206 if r else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(end - start + 1))
            if r:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
            self.send_header("Content-Disposition", 'attachment; filename="%s"' % name)
            self.end_headers()
            return start, end

        def do_HEAD(self):
            self.head()

        def do_GET(self):
            span = self.head()
            if not span:
                return
            start, end = span
            with open(path, "rb") as f:
                f.seek(start)
                left = end - start + 1
                try:
                    while left > 0:
                        data = f.read(min(CHUNK, left))
                        if not data:
                            break
                        self.wfile.write(data)
                        left -= len(data)
                except (ConnectionError, socket.timeout):
                    pass

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path)
    ap.add_argument("--name", help="name to serve it as (default: the file's name)")
    ap.add_argument("--port", type=int, default=8000)
    a = ap.parse_args()
    name = a.name or a.file.name
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), make_handler(a.file, name))
    srv.daemon_threads = True
    print("serving %s (%d bytes) at http://%s:%d/%s" % (a.file, a.file.stat().st_size, lan_address(), a.port, name))
    sys.stdout.flush()
    srv.serve_forever()


if __name__ == "__main__":
    main()
