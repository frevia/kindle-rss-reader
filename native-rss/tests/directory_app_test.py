"""Run native directory UI against a local OPML fixture, never a personal database."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import subprocess
import sys
from threading import Thread
import time


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/slow.opml':
            time.sleep(0.4)
        outlines = ''.join(
            f'<outline title="博客 {i}" xmlUrl="https://example.com/{i}/rss"/>'
            for i in range(12)
        )
        body = f'<opml version="2.0"><body><outline text="科技">{outlines}</outline></body></opml>'.encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}'], check=True, timeout=45)
finally:
    server.shutdown()
    server.server_close()
    thread.join()
