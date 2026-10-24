"""Exercise the WinPE endpoint without proprietary installation media."""
import http.client
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
try:
    import windows_media_server as media
except ImportError:
    media = None


@unittest.skipIf(media is None, 'Optional pycdlib tooling is unavailable')
class MediaServerTest(unittest.TestCase):
    def test_probe_report_and_rejected_requests(self):
        with tempfile.TemporaryDirectory() as tmp:
            reports = Path(tmp) / 'reports'
            server = ThreadingHTTPServer(('127.0.0.1', 0), media.make_handler(Path(tmp) / 'unused.iso', reports))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            def request(method, path, body=None, headers=None):
                connection = http.client.HTTPConnection('127.0.0.1', server.server_port, timeout=5)
                try:
                    connection.request(method, path, body, headers or {})
                    response = connection.getresponse()
                    return response.status, response.read()
                finally:
                    connection.close()
            try:
                self.assertEqual(request('GET', '/probe'), (200, b'PCEMU_WINDOWS_HTTP_OK\r\n'))
                self.assertEqual(request('GET', '/../../etc/passwd')[0], 404)
                self.assertEqual(request('POST', '/arbitrary', b'no')[0], 404)
                self.assertEqual(request('POST', '/report', headers={'Content-Length': 'invalid'})[0], 413)
                self.assertEqual(request('POST', '/report', headers={'Content-Length': str(8 * 1024 * 1024 + 1)})[0], 413)
                self.assertFalse(reports.exists())
                self.assertEqual(request('POST', '/report', b'guest diagnostic\r\n'), (200, b'OK'))
                event = json.loads((reports / 'reports.jsonl').read_text())
                self.assertEqual((reports / event['file']).read_bytes(), b'guest diagnostic\r\n')
                self.assertEqual(event['bytes'], 18)
            finally:
                server.shutdown(); server.server_close(); thread.join()


if __name__ == '__main__':
    unittest.main()
