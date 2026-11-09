#!/usr/bin/env python3
"""Static server for the configurator, same as `python -m http.server` but with caching switched off so that
edited ES modules are always reloaded. localhost counts as a secure context, which WebHID needs.

    python serve.py [port]      # default 8765, serves the folder this file lives in
"""
import functools
import http.server
import sys
from pathlib import Path


class NoCache(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    handler = functools.partial(NoCache, directory=str(Path(__file__).resolve().parent))
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), handler) as srv:
        print(f"http://localhost:{port}/")
        srv.serve_forever()
