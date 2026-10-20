#!/usr/bin/env python3
"""A tiny HTTP(S) mock for tools/test_net.sh (stdlib only).
Usage: net_mock.py --port N [--tls cert.pem key.pem] [--offline] [--slow-ms N]
  GET /hello         -> 200 "hello"
  GET /echo-headers  -> 200, the request headers as received
Nextendo OAuth (the sign-in tests):
  GET /oauth/authorize  -> 302 to redirect_uri?code=C1&state=.. (error=access_denied with deny=1); the Location is
                           also the body, since the test client does not follow redirects
  POST /oauth/token     -> authorization_code (code C1, PKCE checked) gives A1 / R1; refresh_token accepts only the
                           latest refresh token and rotates R1 -> R2 -> ...; --offline: sleeps 30 s first;
                           --slow-ms N: waits N ms on refresh_token grants and on /api/nex-token
  GET /api/nex-token    -> the game token for a Bearer A* access token
  GET /debug/latest-refresh -> the refresh token the mock would accept now
Prints "ready" once it listens."""
import argparse, base64, hashlib, http.server, ssl, sys, time, urllib.parse

CLIENT_ID = "nxc_test_client"  # (tools/test_net.sh builds nextendo.c with this id)
oauth = {"challenge": None, "redirect": None, "n": 0}
offline = False
slow_ms = 0


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self, code, body):
        b = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b)

    def redirect(self, url):
        b = url.encode()
        self.send_response(302)
        self.send_header("Location", url)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b)

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        form = {k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()).items()}
        if u.path != "/oauth/token":
            return self.reply(404, "not found")
        if offline:
            time.sleep(30)
        if form.get("client_id") != CLIENT_ID:
            return self.reply(400, '{"error":"invalid_client"}')
        if form.get("grant_type") == "authorization_code":
            d = base64.urlsafe_b64encode(hashlib.sha256(form.get("code_verifier", "").encode()).digest()).rstrip(b"=").decode()
            if form.get("code") != "C1" or d != oauth["challenge"] or form.get("redirect_uri") != oauth["redirect"]:
                return self.reply(400, '{"error":"invalid_grant"}')
            oauth["n"] = 1
        elif form.get("grant_type") == "refresh_token":
            time.sleep(slow_ms / 1000)
            if form.get("refresh_token") != "R%d" % oauth["n"] or oauth["n"] == 0:
                return self.reply(400, '{"error":"invalid_grant"}')
            oauth["n"] += 1
        else:
            return self.reply(400, '{"error":"unsupported_grant_type"}')
        n = oauth["n"]
        self.reply(200, '{"access_token":"A%d","refresh_token":"R%d","token_type":"Bearer","expires_in":3600}' % (n, n))

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        if u.path == "/oauth/authorize":
            if q.get("client_id") != CLIENT_ID or q.get("code_challenge_method") != "S256":
                return self.reply(400, "bad authorize request")
            oauth["challenge"], oauth["redirect"] = q.get("code_challenge"), q.get("redirect_uri")
            if q.get("deny"):
                return self.redirect(q["redirect_uri"] + "?" + urllib.parse.urlencode({"error": "access_denied", "state": q["state"]}))
            return self.redirect(q["redirect_uri"] + "?" + urllib.parse.urlencode({"code": "C1", "state": q["state"]}))
        if u.path == "/api/nex-token":
            time.sleep(slow_ms / 1000)
            if not self.headers.get("Authorization", "").startswith("Bearer A"):
                return self.reply(401, '{"error":"invalid_token"}')
            return self.reply(200, '{"nex_token":"nx2.test.sig","pid":1234567,"friend_code":"SW-0000-0000-0000","username":"tester","expires_in":86400}')
        if u.path == "/debug/latest-refresh":
            return self.reply(200, "R%d" % oauth["n"])
        if self.path == "/hello":
            self.reply(200, "hello")
        elif self.path == "/echo-headers":
            self.reply(200, "".join(f"{k}: {v}\r\n" for k, v in self.headers.items()))
        else:
            self.reply(404, "not found")

    def log_message(self, *a):
        pass


ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"))
ap.add_argument("--offline", action="store_true")
ap.add_argument("--slow-ms", type=int, default=0)
a = ap.parse_args()
offline = a.offline
slow_ms = a.slow_ms
srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), H)
if a.tls:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(*a.tls)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print("ready", flush=True)
srv.serve_forever()
