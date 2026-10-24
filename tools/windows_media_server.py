#!/usr/bin/env python3
"""Loopback-only WinPE installation-source and diagnostic endpoint.

Streams install.wim from the mounted source ISO without making a local copy.
The project's user network maps guest 10.0.2.2 to host loopback.
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/media-tools'))
import pycdlib


def make_handler(source, reports):
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def do_GET(self):
            if self.path == '/probe':
                body = b'PCEMU_WINDOWS_HTTP_OK\r\n'
                self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers()
                self.wfile.write(body)
                return
            if self.path != '/install.wim':
                self.send_error(404)
                return
            iso = pycdlib.PyCdlib()
            try:
                iso.open(str(source))
                path = '/sources/install.wim'
                size = iso.get_record(udf_path=path).get_data_length()
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Length', str(size)); self.end_headers()
                with iso.open_file_from_iso(udf_path=path) as stream:
                    left = size
                    while left:
                        data = stream.read(min(left, 1024 * 1024))
                        if not data:
                            raise RuntimeError('Truncated installation source')
                        self.wfile.write(data); left -= len(data)
            except (BrokenPipeError, ConnectionResetError):
                self.log_error('Guest disconnected during installation-image transfer')
            finally:
                iso.close()

        def do_POST(self):
            if self.path != '/report':
                self.send_error(404); return
            try:
                count = int(self.headers.get('Content-Length', '-1'))
            except ValueError:
                count = -1
            if not 0 <= count <= 8 * 1024 * 1024:
                self.send_error(413); return
            self.connection.settimeout(30)
            data = self.rfile.read(count)
            if len(data) != count:
                self.send_error(400); return
            reports.mkdir(parents=True, exist_ok=True)
            with lock:
                name = f'report-{time.time_ns()}.log'
                (reports / name).write_bytes(data)
                event = {'file': name, 'bytes': count, 'sha256': hashlib.sha256(data).hexdigest()}
                with (reports / 'reports.jsonl').open('a') as out:
                    out.write(json.dumps(event) + '\n')
            print(json.dumps(event), flush=True)
            self.send_response(200); self.send_header('Content-Length', '2'); self.end_headers()
            self.wfile.write(b'OK')

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=ROOT / 'media/windows11-arm64/Windows11_Client_arm64_en-us_26300_9457.iso')
    parser.add_argument('--reports', type=Path, default=ROOT / 'build/windows11-install/reports')
    parser.add_argument('--port', type=int, default=8080)
    args = parser.parse_args()
    source = args.iso.resolve(strict=True)
    with ThreadingHTTPServer(('127.0.0.1', args.port), make_handler(source, args.reports)) as server:
        print(f'WinPE media endpoint on 127.0.0.1:{server.server_port}', flush=True)
        server.serve_forever()


if __name__ == '__main__':
    main()
