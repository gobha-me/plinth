"""Owned HTTP fixture reproducing legacy cache headers around a real kernel."""

from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, HTTPServer
import threading


class CacheTransition:
    """Keep one browser origin while forwarding to a restarting production kernel.

    The first installed test package omits the versioned-base opt-in, exactly
    like the historical document. Only its mutable asset cache headers change
    here; MIME, CSP, body bytes and auth responses come from the real kernel.
    After replacement all headers pass through unchanged. This is an HTTP
    server fixture, never browser routing (which disables Chromium's cache).
    """

    def __init__(self, kernel_port):
        self.legacy = threading.Event()
        self.legacy.set()
        legacy = self.legacy

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_GET(self):
                upstream = HTTPConnection("127.0.0.1", kernel_port, timeout=5)
                response_started = False
                try:
                    headers = {name: value for name, value in self.headers.items()
                               if name.lower() not in ("connection", "accept-encoding")}
                    upstream.request("GET", self.path, headers=headers)
                    response = upstream.getresponse()
                    body = response.read()
                    response_started = True
                    self.send_response(response.status)
                    path = self.path.split("?", 1)[0]
                    old_asset = (legacy.is_set() and path.startswith("/app/")
                                 and "." in path.rsplit("/", 1)[-1]
                                 and not path.endswith(".html") and response.status == 200)
                    for name, value in response.getheaders():
                        if name.lower() in ("connection", "transfer-encoding", "content-length"):
                            continue
                        if old_asset and name.lower() == "cache-control":
                            continue
                        self.send_header(name, value)
                    if old_asset:
                        self.send_header("Cache-Control", "public, max-age=31536000, immutable")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except (ConnectionError, TimeoutError):
                    if not response_started:
                        self.send_error(502, "kernel unavailable")
                    self.close_connection = True
                finally:
                    upstream.close()

        class BoundedServer(HTTPServer):
            def get_request(self):
                connection, address = super().get_request()
                connection.settimeout(5)
                return connection, address

        self.server = BoundedServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.05},
                                       name="legacy-cache-forwarder")

    @property
    def origin(self):
        return f"http://127.0.0.1:{self.server.server_port}"

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_args):
        try:
            self.server.shutdown()
        finally:
            self.server.server_close()
            self.thread.join(timeout=15)
        if self.thread.is_alive():
            raise RuntimeError("legacy cache forwarding thread did not stop")
