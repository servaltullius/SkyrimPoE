"""Serve the existing tree UI with a local pack and a simulated bridge."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
UI = ROOT / "ui"
PACKS = {"poe1": "tree_3_30.json", "poe2": "gen1/tree.json"}
JS_TYPE = "text/javascript; charset=utf-8"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()

    html = (UI / "index.html").read_text(encoding="utf-8").replace(
        '<script src="app.js"></script>',
        '<script src="preview.js"></script>\n  <script src="app.js"></script>',
    )
    assets = {
        "/style.css": ("text/css; charset=utf-8", (UI / "style.css").read_bytes()),
        "/preview.js": (JS_TYPE, (HERE / "preview.js").read_bytes()),
        **{f"/{name}.js": (JS_TYPE, (UI / f"{name}.js").read_bytes())
           for name in ("app", "tree", "gear", "maps")},
    }
    tree_scripts = {
        game: b"window.POE_TREE = " + (ROOT / "data" / "packs" / pack).read_bytes() + b";\n"
        for game, pack in PACKS.items()
    }

    class PreviewHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.serve_asset(send_body=True)

        def do_HEAD(self):
            self.serve_asset(send_body=False)

        def serve_asset(self, send_body):
            url = urlsplit(self.path)
            game = parse_qs(url.query).get("game", ["poe2"])[0]
            if url.path in ("/", "/index.html", "/tree-data.js") and game not in PACKS:
                self.send_error(400, "game must be poe1 or poe2")
                return
            if url.path in ("/", "/index.html"):
                content_type = "text/html; charset=utf-8"
                body = html.replace('src="tree-data.js"', f'src="tree-data.js?game={game}"').encode("utf-8")
            elif url.path == "/tree-data.js":
                content_type, body = JS_TYPE, tree_scripts[game]
            elif url.path == "/favicon.ico":
                self.send_response(204)
                self.end_headers()
                return
            elif url.path in assets:
                content_type, body = assets[url.path]
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            if send_body:
                self.wfile.write(body)

    with ThreadingHTTPServer(("127.0.0.1", args.port), PreviewHandler) as server:
        print(f"http://127.0.0.1:{server.server_port}/?game=poe2", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
