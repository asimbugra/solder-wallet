#!/usr/bin/env python3
"""Solder simulator: the device and a small fake Solana network in one process.

    python3 tools/simulator.py            # wallet:  http://localhost:8080
                                          # device:  http://localhost:8080/device
    python3 tools/simulator.py --ready    # skip the first-start steps (password set, wallet made, PIN 123456)

The /device page is a virtual Solder board: its screen shows what the real OLED
would show, and you hold its BOOT button with the mouse (or the space bar).
Nothing here touches a real network or real keys.
"""
import argparse
import base64
import json
import random
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
HOLD_MS = 1500
TIMEOUT_S = 45
FACTORY_PASS = "admin123"
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
WORDS = ("orbit velvet canyon harbor lantern meadow pilot ribbon silver tunnel wagon anchor "
         "breeze cactus dragon ember falcon glacier hollow island jungle kettle marble nectar").split()
ADDRESS = "SoLdR7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJ"
USDC = "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU"


def b58(n):
    return "".join(random.choice(B58) for _ in range(n))


class Chain:
    """Just enough of a Solana RPC for the wallet page."""

    def __init__(self):
        self.lamports = 0
        self.history = []          # (signature, time, err)
        self.pending = {}          # signature -> confirm time

    def call(self, method, params):
        now = time.time()
        if method == "getBalance":
            return {"context": {"slot": 1}, "value": self.lamports}
        if method == "getTokenAccountsByOwner":
            if params[1].get("programId", "").startswith("Tokenkeg") and self.lamports:
                info = {"mint": USDC, "tokenAmount": {"amount": "125500000", "decimals": 6, "uiAmountString": "125.5"}}
                return {"value": [{"account": {"data": {"parsed": {"info": info}}}}]}
            return {"value": []}
        if method == "getSignaturesForAddress":
            return [{"signature": s, "blockTime": int(t), "err": e} for s, t, e in reversed(self.history)][:10]
        if method == "requestAirdrop":
            sig = b58(87)
            self.lamports += int(params[1])
            self.history.append((sig, now, None))
            return sig
        if method == "getLatestBlockhash":
            return {"context": {"slot": 1}, "value": {"blockhash": b58(44), "lastValidBlockHeight": 1000}}
        if method == "sendTransaction":
            meta = json.loads(base64.b64decode(params[0]).decode())
            self.pending[meta["signature"]] = now + 1.4
            if meta.get("lamports"):
                self.lamports -= meta["lamports"] + 5000
            self.history.append((meta["signature"], now, None))
            return meta["signature"]
        if method == "getSignatureStatuses":
            out = []
            for s in params[0]:
                t = self.pending.get(s)
                out.append(None if t is None else {"confirmationStatus": "confirmed" if now >= t else "processed", "err": None})
            return {"value": out}
        raise ValueError("method not simulated: " + method)


class Device:
    def __init__(self, ready):
        self.ap_pass = "my-solder-pass" if ready else FACTORY_PASS
        self.has_wallet = ready
        self.pin = "123456"
        self.unlocked = False
        self.token = ""
        self.network = "devnet"
        self.req = None
        self.press_start = None
        self.pressed = False
        self.pending_words = ""
        self.fails = 0
        self.down_until = 0       # simulated restart
        self.screen = ["Solder", "WiFi: Solder-3F2A", "Pass: " + self.ap_pass, "Open 192.168.4.1"]
        self.led = False
        self.lock = threading.Lock()
        self.chain = Chain()
        if ready:
            self.chain.lamports = 2_000_000_000

    # ---- what the OLED would show
    def paint(self):
        r = self.req
        if r and r["state"] == "waiting":
            if r["kind"] in ("send", "send-token"):
                self.screen = ["Send", r["amount"] + " " + r["symbol"], "to " + r["to"][:5] + ".." + r["to"][-5:], "Hold BOOT to approve"]
            elif r["kind"] == "sign-message":
                self.screen = ["Sign message", r["message"][:21], r["message"][21:42], "Hold BOOT to approve"]
            else:
                self.screen = ["ERASE WALLET", "Keys are deleted.", "Only the words restore.", "Hold BOOT to approve"]
        elif self.unlocked:
            self.screen = ["Solder - unlocked", ADDRESS[:5] + ".." + ADDRESS[-5:], "WiFi password set", "http://192.168.4.1"]
        elif self.has_wallet:
            self.screen = ["Solder - locked", "-", "WiFi password set", "http://192.168.4.1"]

    def tick(self):
        r = self.req
        now = time.time()
        self.led = False
        if not r or r["state"] != "waiting":
            return
        age = now - r["_t"]
        r["expiresIn"] = max(0, int(TIMEOUT_S - age))
        if age > TIMEOUT_S:
            r["state"] = "expired"
            self.paint()
            return
        if self.pressed:
            if self.press_start is None:
                self.press_start = now
            h = (now - self.press_start) * 1000 / HOLD_MS
            r["hold"] = min(1.0, h)
            if h >= 1:
                self.press_start = None
                self.execute()
        else:
            self.press_start = None
            r["hold"] = 0
        self.led = int(now / (0.08 if self.pressed else 0.4)) % 2 == 1

    def execute(self):
        r = self.req
        if r["kind"] == "wipe":
            self.has_wallet = False
            self.unlocked = False
            self.token = ""
        elif r["kind"] == "sign-message":
            r["signature"] = b58(88)
        else:
            sig = b58(88)
            r["signature"] = sig
            lamports = int(round(float(r["amount"]) * 1e9)) if r["symbol"] == "SOL" else 0
            # a stand-in for the signed transaction: the fake chain reads it back
            r["tx"] = base64.b64encode(json.dumps({"signature": sig, "lamports": lamports}).encode()).decode()
        r["state"] = "done"
        self.paint()

    def state(self, host):
        return {
            "version": "0.2.0-sim", "apName": "Solder-3F2A", "defaultPassword": self.ap_pass == FACTORY_PASS,
            "hasWallet": self.has_wallet, "unlocked": self.unlocked, "address": ADDRESS if self.unlocked else "",
            "network": self.network, "rpc": "http://%s/rpc" % host, "autoLockMin": 5, "screen": True,
            "holdMs": HOLD_MS, "timeoutS": TIMEOUT_S,
        }


DEVICE_PAGE = r"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Solder board</title><style>
body{margin:0;min-height:100vh;display:grid;place-items:center;background:#0b1f17;font-family:system-ui,sans-serif;color:#eceee3}
.pcb{width:360px;padding:26px 26px 30px;border-radius:22px;background:radial-gradient(circle at 1px 1px,rgba(236,238,227,.05) 1px,transparent 1.4px) 0 0/16px 16px,#0e3a2c;box-shadow:0 0 0 2px #2d6b51,0 30px 80px rgba(0,0,0,.5);position:relative}
.hole{position:absolute;width:12px;height:12px;border-radius:50%;border:3px solid #a8873a}
.oled{background:#000;border-radius:6px;padding:12px 14px;height:118px;box-shadow:0 0 0 6px #1b1b1b;font:15px/1.55 ui-monospace,Menlo,monospace;color:#bfe9ff;white-space:pre;overflow:hidden}
.oled b{display:block;border-bottom:1px solid #bfe9ff;margin-bottom:4px;font-weight:600}
.bar{height:8px;border:1px solid #bfe9ff;margin-top:6px}.bar i{display:block;height:100%;background:#bfe9ff;width:0}
.row{display:flex;align-items:center;justify-content:space-between;margin-top:26px}
.chip{width:120px;height:78px;border-radius:6px;background:#13271f;border:2px solid #2d6b51;display:grid;place-items:center;font-weight:600;letter-spacing:.04em;color:#eceee3}
.btn{width:86px;height:86px;border-radius:10px;background:#cfd3c9;border:0;display:grid;place-items:center;cursor:pointer;touch-action:none}
.btn span{width:50px;height:50px;border-radius:50%;background:#2a2f2c;transition:transform .1s}
.btn.down span{transform:scale(.84)}
.lbl{font-size:12px;color:#a3b8aa;text-align:center;margin-top:6px;letter-spacing:.08em}
.led{width:14px;height:14px;border-radius:50%;background:#5a2a24;margin:0 auto}.led.on{background:#ff5b4a;box-shadow:0 0 14px #ff5b4a}
.cap{margin-top:22px;font-size:13px;color:#a3b8aa;text-align:center}
</style></head><body><div class="pcb">
<i class="hole" style="left:10px;top:10px"></i><i class="hole" style="right:10px;top:10px"></i>
<div class="oled" id="oled"></div>
<div class="row"><div class="chip">ESP32</div><div><div class="led" id="led"></div><div class="lbl">LED</div></div>
<div><button class="btn" id="btn" aria-label="BOOT"><span></span></button><div class="lbl">BOOT</div></div></div>
<div class="cap">Hold BOOT with the mouse or the space bar</div></div>
<script>
if(location.search.includes("embed"))document.body.style.background="transparent";
const oled=document.getElementById("oled"),led=document.getElementById("led"),btn=document.getElementById("btn");
function press(d){btn.classList.toggle("down",d);fetch("/sim/button",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({down:d})})}
btn.onpointerdown=e=>{btn.setPointerCapture(e.pointerId);press(true)};btn.onpointerup=()=>press(false);btn.onpointercancel=()=>press(false);
let sp=false;addEventListener("keydown",e=>{if(e.code==="Space"&&!sp){sp=true;press(true);e.preventDefault()}});addEventListener("keyup",e=>{if(e.code==="Space"){sp=false;press(false)}});
async function poll(){try{const s=await (await fetch("/sim/state")).json();
oled.innerHTML="<b></b>"+s.screen.slice(1).map(()=>"<div></div>").join("")+(s.hold!==null?'<div class="bar"><i></i></div>':"");
oled.querySelector("b").textContent=s.screen[0];[...oled.querySelectorAll("div:not(.bar)")].forEach((d,i)=>d.textContent=s.screen[i+1]);
if(s.hold!==null)oled.querySelector(".bar i").style.width=(s.hold*100)+"%";led.classList.toggle("on",s.led)}catch(e){oled.innerHTML="<b>restarting...</b>"}setTimeout(poll,90)}poll();
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    dev = None
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send_body(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, code, obj):
        self.send_body(code, json.dumps(obj).encode(), "application/json")

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError:
            return {}

    def session(self):
        d = self.dev
        if d.unlocked and self.headers.get("X-Solder-Session") == d.token:
            return True
        self.send_json(401, {"error": "Session ended, unlock again" if d.unlocked else "Wallet is locked"})
        return False

    def restarting(self):
        if time.time() < self.dev.down_until:
            # the real device simply disappears from the WiFi while it restarts
            self.close_connection = True
            self.send_json(503, {"error": "Restarting"})
            return True
        return False

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        d = self.dev
        path = self.path.split("?")[0]
        with d.lock:
            d.tick()
            if path == "/":
                self.send_body(200, (WEB / "index.html").read_bytes(), "text/html; charset=utf-8")
            elif path == "/device":
                self.send_body(200, DEVICE_PAGE.encode(), "text/html; charset=utf-8")
            elif path.startswith("/vendor/"):
                f = WEB / path.lstrip("/")
                if f.exists():
                    self.send_body(200, f.read_bytes(), "font/woff2" if f.suffix == ".woff2" else "text/javascript")
                else:
                    self.send_json(404, {"error": "not found"})
            elif path == "/sim/state":
                if time.time() < d.down_until:
                    self.send_json(200, {"screen": ["restarting...", "", "", ""], "hold": None, "led": False})
                else:
                    waiting = d.req and d.req["state"] == "waiting"
                    self.send_json(200, {"screen": d.screen, "hold": d.req["hold"] if waiting else None, "led": d.led})
            elif self.restarting():
                return
            elif path == "/api/state":
                self.send_json(200, d.state(self.headers.get("Host", "localhost:8080")))
            elif path == "/api/request":
                if d.has_wallet and not self.session():
                    return
                if not d.req:
                    self.send_json(404, {"error": "No request"})
                else:
                    self.send_json(200, {"request": {k: v for k, v in d.req.items() if not k.startswith("_")}})
            else:
                self.send_json(404, {"error": "Unknown endpoint"})

    def do_POST(self):
        d = self.dev
        b = self.body()
        p = self.path
        with d.lock:
            d.tick()
            if p == "/sim/button":
                d.pressed = bool(b.get("down"))
                d.tick()
                self.send_json(200, {"ok": True})
                return
            if p == "/rpc":
                try:
                    time.sleep(0.12)
                    self.send_json(200, {"jsonrpc": "2.0", "id": b.get("id"), "result": d.chain.call(b["method"], b.get("params", []))})
                except Exception as e:  # noqa: BLE001
                    self.send_json(200, {"jsonrpc": "2.0", "id": b.get("id"), "error": {"code": -32000, "message": str(e)}})
                return
            if self.restarting():
                return
            if p == "/api/ap-password":
                pw = b.get("password", "")
                if len(pw) < 8 or pw == FACTORY_PASS:
                    self.send_json(400, {"error": "Use 8 to 63 characters" if len(pw) < 8 else "Choose a password other than the factory one"})
                    return
                d.ap_pass = pw
                d.unlocked, d.token = False, ""
                d.screen = ["Solder", "WiFi password set", "Restarting...", ""]
                self.send_json(200, {"restarting": True})
                d.down_until = time.time() + 6
                threading.Timer(6.2, lambda: setattr(d, "screen", ["Solder", "WiFi: Solder-3F2A", "Pass: (your password)", "Open 192.168.4.1"])).start()
                return
            if d.ap_pass == FACTORY_PASS and p in ("/api/setup/generate", "/api/setup/finish"):
                self.send_json(428, {"error": "Change the WiFi password first"})
                return
            if p == "/api/setup/generate":
                n = 24 if b.get("words") == 24 else 12
                d.pending_words = " ".join(random.sample(WORDS, n))
                self.send_json(200, {"words": d.pending_words})
            elif p == "/api/setup/finish":
                if len(b.get("pin", "")) < 6:
                    self.send_json(400, {"error": "Use a PIN with at least 6 digits"})
                    return
                time.sleep(1.2)  # PBKDF2 on the ESP32 takes about this long
                d.has_wallet, d.pin, d.unlocked, d.token = True, b["pin"], True, secrets.token_hex(16)
                d.paint()
                self.send_json(200, {"token": d.token, "address": ADDRESS})
            elif p == "/api/unlock":
                time.sleep(0.9)
                if b.get("pin") != d.pin:
                    d.fails += 1
                    self.send_json(403, {"error": "Wrong PIN, %d tries left" % (10 - d.fails)})
                    return
                d.unlocked, d.token, d.fails = True, secrets.token_hex(16), 0
                d.paint()
                self.send_json(200, {"token": d.token, "address": ADDRESS})
            elif p == "/api/lock":
                d.unlocked, d.token = False, ""
                d.paint()
                self.send_json(200, {"ok": True})
            elif p in ("/api/send", "/api/sign", "/api/wipe"):
                if not self.session():
                    return
                if d.req and d.req["state"] == "waiting":
                    self.send_json(409, {"error": "Another request is waiting for the button"})
                    return
                if p == "/api/send":
                    to, amt, bh = b.get("to", ""), b.get("amount", ""), b.get("blockhash", "")
                    if not (32 <= len(to) <= 44) or any(c not in B58 for c in to):
                        self.send_json(400, {"error": "That is not a Solana address"})
                        return
                    if not bh:
                        self.send_json(400, {"error": "Missing recent blockhash, is your phone online?"})
                        return
                    try:
                        assert float(amt.replace(",", ".")) > 0
                    except (ValueError, AssertionError):
                        self.send_json(400, {"error": "Enter an amount with at most 9 decimals"})
                        return
                    mint = b.get("mint") or ""
                    req = {"kind": "send-token" if mint else "send", "to": to, "amount": amt.replace(",", "."), "symbol": mint[:4] if mint else "SOL"}
                elif p == "/api/sign":
                    if not b.get("message"):
                        self.send_json(400, {"error": "Write a message up to 1024 characters"})
                        return
                    req = {"kind": "sign-message", "message": b["message"]}
                else:
                    req = {"kind": "wipe"}
                req.update({"id": random.randint(1, 99999), "state": "waiting", "hold": 0, "expiresIn": TIMEOUT_S, "_t": time.time()})
                d.req = req
                d.paint()
                self.send_json(200, {"request": {k: v for k, v in req.items() if not k.startswith("_")}})
            elif p == "/api/request/reject":
                if d.req and d.req["state"] == "waiting":
                    d.req["state"] = "rejected"
                d.paint()
                self.send_json(200, {"request": {k: v for k, v in d.req.items() if not k.startswith("_")}})
            elif p == "/api/network":
                d.network = b.get("network", "devnet")
                self.send_json(200, {"ok": True})
            else:
                self.send_json(404, {"error": "Unknown endpoint"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--ready", action="store_true", help="password set, wallet made, PIN 123456")
    a = ap.parse_args()
    Handler.dev = Device(a.ready)
    print("Solder simulator")
    print("  wallet: http://localhost:%d" % a.port)
    print("  device: http://localhost:%d/device   (hold BOOT here)" % a.port)
    print("  factory WiFi password: %s, PIN after setup: what you choose%s" % (FACTORY_PASS, " (ready mode: 123456)" if a.ready else ""))
    ThreadingHTTPServer(("", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
