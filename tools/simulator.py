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
import hashlib
import json
import random
import re
import secrets
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
HOLD_MS = 1500
TIMEOUT_S = 45
FACTORY_PASS = "".join(secrets.choice("23456789ABCDEFGHJKLMNPQRSTUVWXYZ") for _ in range(16))
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
WORDS = ("orbit velvet canyon harbor lantern meadow pilot ribbon silver tunnel wagon anchor "
         "breeze cactus dragon ember falcon glacier hollow island jungle kettle marble nectar").split()
ADDRESS = "SoLdR7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJ"
USDC = "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU"
TOKEN_2022_MINT = "3PUZhUoTb8vtVBWAQcB8U79RR5JrahfZhAxB537aGM5o"
TOKEN_PROGRAM = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
TOKEN_2022_PROGRAM = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"
SYSTEM_PROGRAM = "1" * 32
NETWORK_FEE = 5000
TOKEN_FIXTURES = {TOKEN_PROGRAM: (USDC, 165), TOKEN_2022_PROGRAM: (TOKEN_2022_MINT, 170)}
ASSETS = {"/vendor/" + f.name: f for f in (WEB / "vendor").iterdir()
          if f.is_file() and not f.is_symlink() and f.suffix in (".woff2", ".js")}


def b58(n):
    return "".join(random.choice(B58) for _ in range(n))


def simulated_ata(owner, mint, program):
    # A stable stand-in for an associated account on this local chain.
    digest = hashlib.sha256(f"{owner}/{mint}/{program}".encode()).digest()
    number = int.from_bytes(digest, "big")
    encoded = ""
    while number:
        number, digit = divmod(number, 58)
        encoded = B58[digit] + encoded
    return "1" * (len(digest) - len(digest.lstrip(b"\0"))) + encoded


def pack_message(message):
    return base64.b64encode(json.dumps(message, separators=(",", ":")).encode()).decode()


def rent_exempt_minimum(size):
    # Fixed simulator rent schedule, shared by the quote and fake ledger.
    return (size + 128) * 6960


def simulated_account(owner, lamports, size=0):
    return {"owner": owner, "lamports": lamports, "space": size, "data": ["", "base64"], "executable": False}


class Chain:
    """Just enough of a Solana RPC for the wallet page."""

    def __init__(self):
        self.lamports = 0
        self.history = deque(maxlen=100)     # (signature, time, err)
        self.pending = {}          # signature -> (confirm time, submitted transaction)
        self.signed = None         # most recent unbroadcast simulated transaction
        self.accounts = {}         # funded recipients, including associated token accounts
        self.tokens = {USDC: 125_500_000, TOKEN_2022_MINT: 50_000_000}  # 6 decimals for both fixtures

    def call(self, method, params):
        now = time.time()
        if method == "getBalance":
            balance = self.lamports if params[0] == ADDRESS else self.accounts.get(params[0], {}).get("lamports", 0)
            return {"context": {"slot": 1}, "value": balance}
        if method == "getTokenAccountsByOwner":
            program = params[1].get("programId")
            if params[0] == ADDRESS and program in TOKEN_FIXTURES and self.lamports:
                mint, _ = TOKEN_FIXTURES[program]
                units = self.tokens[mint]
                whole, frac = divmod(units, 1_000_000)
                ui_amount = f"{whole}.{frac:06}".rstrip("0").rstrip(".")
                info = {"mint": mint, "tokenAmount": {"amount": str(units), "decimals": 6, "uiAmountString": ui_amount}}
                return {"value": [{"pubkey": simulated_ata(ADDRESS, mint, program),
                                   "account": {"data": {"parsed": {"info": info}}}}]}
            return {"value": []}
        if method == "getFeeForMessage":
            message = json.loads(base64.b64decode(params[0], validate=True))
            if message.get("kind") not in ("send", "send-token"):
                raise ValueError("Unknown simulated transfer")
            return {"context": {"slot": 1}, "value": NETWORK_FEE}
        if method == "getAccountInfo":
            return {"context": {"slot": 1}, "value": self.accounts.get(params[0])}
        if method == "getMinimumBalanceForRentExemption":
            size = params[0]
            if type(size) is not int or not 0 <= size <= 10_485_760:
                raise ValueError("Invalid account size")
            return rent_exempt_minimum(size)
        if method == "simulateTransaction":
            message = json.loads(base64.b64decode(params[0], validate=True))
            if message.get("kind") != "GetAccountDataSize" or message.get("mint") != TOKEN_2022_MINT:
                raise ValueError("Unknown simulated sizing query")
            account_size = TOKEN_FIXTURES[TOKEN_2022_PROGRAM][1]
            size = base64.b64encode(account_size.to_bytes(8, "little")).decode()
            return {"context": {"slot": 1}, "value": {"err": None,
                    "returnData": {"programId": TOKEN_2022_PROGRAM, "data": [size, "base64"]}}}
        if method == "getSignaturesForAddress":
            return [{"signature": s, "blockTime": int(t), "err": e} for s, t, e in reversed(self.history)][:10]
        if method == "requestAirdrop":
            if type(params[1]) is not int or not 0 < params[1] <= 2_000_000_000:
                raise ValueError("Invalid airdrop amount")
            sig = b58(87)
            self.lamports += int(params[1])
            self.history.append((sig, now, None))
            return sig
        if method == "getLatestBlockhash":
            return {"context": {"slot": 1}, "value": {"blockhash": b58(44), "lastValidBlockHeight": 1000}}
        if method == "sendTransaction":
            for sig, (_, submitted) in self.pending.items():
                if params[0] == submitted:
                    return sig
            if self.signed is None or params[0] != self.signed[0]:
                raise ValueError("No matching signed transaction")
            _, sig, transfer = self.signed
            mint, program, dest = transfer["mint"], transfer["program"], transfer["to"]
            units = transfer["units"]
            sol_out = units
            if mint:
                fixture_mint, size = TOKEN_FIXTURES[program]
                if mint != fixture_mint or transfer["decimals"] != 6 or self.tokens[mint] < units:
                    raise ValueError("Invalid token or insufficient token balance")
                dest = simulated_ata(dest, mint, program)
                existing = self.accounts.get(dest, {})
                if existing.get("owner") == program:
                    sol_out = 0
                elif not existing or (existing["owner"] == SYSTEM_PROGRAM and existing["space"] == 0):
                    sol_out = max(0, rent_exempt_minimum(size) - existing.get("lamports", 0))
                else:
                    raise ValueError("Cannot create recipient token account")
            if self.lamports < sol_out + NETWORK_FEE:
                raise ValueError("Insufficient balance")
            self.lamports -= sol_out + NETWORK_FEE
            account = self.accounts.setdefault(dest, simulated_account(SYSTEM_PROGRAM, 0))
            account["lamports"] += sol_out
            if mint:
                self.tokens[mint] -= units
                account.update({"owner": program, "space": size})
            if len(self.pending) >= 100:
                del self.pending[next(iter(self.pending))]
            self.pending[sig] = (now + 1.4, params[0])
            self.history.append((sig, now, None))
            return sig
        if method == "getSignatureStatuses":
            out = []
            for s in params[0]:
                receipt = self.pending.get(s)
                out.append(None if receipt is None else {"confirmationStatus": "confirmed" if now >= receipt[0] else "processed", "err": None})
            return {"value": out}
        raise ValueError("method not simulated: " + method)


class Device:
    def __init__(self, ready, oled=True):
        self.oled = oled
        self.ap_pass = "my-solder-pass" if ready else FACTORY_PASS
        self.has_wallet = ready
        self.pin = "123456"
        self.unlocked = False
        self.token = ""
        self.network = "devnet"
        self.auto_lock_min = 5
        self.last_activity = 0
        self.req = None
        self.next_id = 1
        self.press_start = None
        self.pressed = False
        self.wait_release = False
        self.pending_words = ""
        self.check_state = "idle"
        self.questions = deque()
        self.selection = 0
        self.setup_token = ""
        self.setup_time = 0
        self.fails = 0
        self.pin_delay = 0
        self.pin_gate = False
        self.pin_client = ""
        self.pin_candidate = ""
        self.pin_allowed_until = 0
        self.down_until = 0       # simulated restart
        self.screen = ["Solder", "WiFi: Solder-3F2A", "Pass: " + ("(your password)" if ready else FACTORY_PASS), "Open 192.168.4.1"]
        self.led = False
        self.lock = threading.Lock()
        self.chain = Chain()
        if ready:
            self.chain.lamports = 2_000_000_000
        if not oled:
            self.paint()

    # ---- what the OLED would show
    def paint(self):
        r = self.req
        if not self.oled:
            if r and r["state"] == "waiting":
                r["_pages"] = 1
            self.screen = ["Board only", "No OLED connected"]
        elif r and r["state"] == "waiting":
            if r["kind"] in ("send", "send-token"):
                lines = [r["amount"] + " " + r["symbol"], "To " + r["to"]]
                if r.get("mint"):
                    lines.append("Mint " + r["mint"])
            elif r["kind"] == "sign-message":
                lines = ["".join(chr(b) if 32 <= b < 127 and b != 92 else f"\\x{b:02x}"
                                 for b in r["message"].encode())]
            elif r["kind"] == "restore":
                lines = [f"{i}. {word}" for i, word in enumerate(r["_words"].split(), 1)]
            else:
                lines = ["Keys are deleted.", "Restore with words."]
            rows = [line[i:i + 21] for line in lines for i in range(0, len(line), 21)]
            r["_pages"] = (len(rows) + 1) // 2
            pos = r["_page"] * 2
            title = {"send": "Send", "send-token": "Send token", "sign-message": "Sign message",
                     "wipe": "ERASE WALLET", "restore": "Restore wallet"}[r["kind"]]
            self.screen = [title, rows[pos], rows[pos + 1] if pos + 1 < len(rows) else "",
                           "Hold BOOT to approve" if r["_page"] + 1 == r["_pages"] else "Tap BOOT for next"]
        elif self.pin_gate:
            self.screen = ["PIN retry", "Hold and release BOOT", "then retry your PIN", ""]
        elif self.unlocked:
            self.screen = ["Solder - unlocked", ADDRESS[:5] + ".." + ADDRESS[-5:], "WiFi password set", "http://192.168.4.1"]
        elif self.has_wallet:
            self.screen = ["Solder - locked", "-", "WiFi password set", "http://192.168.4.1"]
        else:
            self.screen = ["Solder", "WiFi: Solder-3F2A", "WiFi password set", "Open 192.168.4.1"]

    def clear_setup(self):
        self.setup_token = self.pending_words = ""
        self.questions.clear()
        self.check_state = "idle"
        if self.req and self.req["kind"] == "restore":
            self.req = None

    def show_words(self, page):
        start = page * 6
        words = self.pending_words.split()[start:start + 6]
        self.screen = ["Backup words"] + [f"{i}. {w}" for i, w in enumerate(words, start + 1)]

    def start_check(self):
        words = self.pending_words.split()
        self.questions.clear()
        for pos in random.sample(range(len(words)), 3):
            start = random.randrange(len(words))
            choices = [words[pos]]
            for candidate in words[start:] + words[:start] + ["ability", "cactus", "yellow"]:
                if candidate not in choices:
                    choices.append(candidate)
                if len(choices) == 3:
                    break
            random.shuffle(choices)
            self.questions.append((pos, choices))
        self.check_state = "checking"
        self.wait_release, self.press_start = self.pressed, None
        self.selection = 0
        self.setup_time = time.time()
        self.show_question()

    def show_question(self):
        pos, choices = self.questions[0]
        self.screen = [f"Backup check {4 - len(self.questions)}/3", f"Word #{pos + 1}?"]
        self.screen.extend(("> " if i == self.selection else "  ") + w for i, w in enumerate(choices))
        self.screen.append("Tap:next  Hold:pick")

    def check_button(self, now):
        if self.wait_release:
            return
        if self.pressed and self.press_start is None:
            self.press_start = now
        if self.press_start is None:
            return
        if self.pressed and now - self.press_start >= HOLD_MS / 1000:
            pos, choices = self.questions.popleft()
            self.wait_release = True
            if choices[self.selection] != self.pending_words.split()[pos]:
                self.check_state = "incorrect"
                self.questions.clear()
                self.screen = ["Check your paper", "Retry from phone"]
            elif not self.questions:
                self.check_state = "verified"
                self.screen = ["Backup checked", "Set PIN on phone"]
            else:
                self.selection = 0
                self.show_question()
        elif not self.pressed:
            self.selection = (self.selection + 1) % 3
            self.show_question()
        else:
            return
        self.press_start = None
        self.setup_time = now

    def end_session(self):
        self.unlocked, self.token = False, ""
        self.req = None
        self.press_start = None
        self.wait_release = self.pressed
        self.clear_pin_gate()
        self.paint()

    def clear_pin_gate(self):
        self.pin_gate = False
        self.pin_allowed_until = 0
        self.pin_client = self.pin_candidate = ""

    def tick(self):
        now = time.time()
        if self.pin_client and now >= self.pin_allowed_until:
            self.clear_pin_gate()
            self.press_start = None
            self.paint()
        if self.unlocked and self.auto_lock_min and now - self.last_activity >= self.auto_lock_min * 60:
            self.end_session()
        if self.setup_token and now - self.setup_time >= 900:
            self.clear_setup()
            self.paint()
        if self.req and self.req["kind"] == "restore" and self.req["state"] == "done" and now >= self.req["_collect_until"]:
            self.req = None
            self.setup_token = ""
            self.paint()
        if self.wait_release:
            if not self.pressed:
                self.wait_release = False
            self.press_start = None
        if self.check_state == "checking":
            self.check_button(now)
            return
        r = self.req
        if self.pin_gate:
            if self.pressed and not self.wait_release and self.press_start is None:
                self.press_start = now
            if not self.pressed and self.press_start is not None:
                if now - self.press_start >= HOLD_MS / 1000:
                    self.pin_allowed_until = now + 30
                    self.pin_gate = False
                    self.paint()
                self.press_start = None
        self.led = False
        if not r or r["state"] != "waiting":
            return
        age = now - r["_t"]
        r["expiresIn"] = max(0, int(TIMEOUT_S - age))
        if age > TIMEOUT_S:
            r["state"] = "expired"
            self.paint()
            return
        if self.pressed and not self.wait_release:
            if self.press_start is None:
                self.press_start = now
            h = (now - self.press_start) * 1000 / HOLD_MS
            last = r["_page"] + 1 == r["_pages"]
            r["hold"] = min(1.0, h) if last else 0
            if last and h >= 1:
                self.press_start = None
                self.execute()
        else:
            if self.press_start is not None and r["_page"] + 1 < r["_pages"] and now - self.press_start < HOLD_MS / 1000:
                r["_page"] += 1
                r["_t"] = now
                self.paint()
            self.press_start = None
            r["hold"] = 0
        self.led = int(now / (0.08 if self.pressed else 0.4)) % 2 == 1

    def execute(self):
        r = self.req
        if r["kind"] == "wipe":
            self.has_wallet = False
            self.pin = ""
            self.end_session()
            self.req = r
        elif r["kind"] == "restore":
            r["_collect_until"] = time.time() + 30
        elif r["kind"] == "sign-message":
            r["signature"] = b58(88)
        else:
            sig = b58(88)
            r["signature"] = sig
            transfer = r["_transfer"]
            r["tx"] = pack_message({"signature": sig, "message": transfer})
            self.chain.signed = (r["tx"], sig, transfer)
        r["state"] = "done"
        self.paint()

    def state(self, owner):
        return {
            "version": "0.2.0-sim", "apName": "Solder-3F2A", "defaultPassword": self.ap_pass == FACTORY_PASS,
            "hasWallet": self.has_wallet, "unlocked": owner, "address": ADDRESS if owner else "",
            "network": self.network, "rpc": "/rpc" if owner else "", "autoLockMin": self.auto_lock_min if owner else None, "screen": self.oled,
            "restorePending": bool(self.req and self.req["kind"] == "restore" and self.req["state"] in ("waiting", "done")),
            "holdMs": HOLD_MS, "timeoutS": TIMEOUT_S,
        }


DEVICE_PAGE = r"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Solder board</title><style>
body{margin:0;min-height:100vh;display:grid;place-items:center;background:#0b1f17;font-family:system-ui,sans-serif;color:#eceee3}
.pcb{width:360px;padding:26px 26px 30px;border-radius:22px;background:radial-gradient(circle at 1px 1px,rgba(236,238,227,.05) 1px,transparent 1.4px) 0 0/16px 16px,#0e3a2c;box-shadow:0 0 0 2px #2d6b51,0 30px 80px rgba(0,0,0,.5);position:relative}
.hole{position:absolute;width:12px;height:12px;border-radius:50%;border:3px solid #a8873a}
.oled{background:#000;border-radius:6px;padding:12px 14px;height:118px;box-shadow:0 0 0 6px #1b1b1b;font:15px/1.55 ui-monospace,Menlo,monospace;color:#bfe9ff;white-space:pre;overflow:hidden}
.oled b{display:block;border-bottom:1px solid #bfe9ff;margin-bottom:4px;font-weight:600}
.oled.compact{font-size:14px;line-height:1.12}
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
oled.classList.toggle("compact",s.screen.length>4);
oled.innerHTML="<b></b>"+s.screen.slice(1).map(()=>"<div></div>").join("")+(s.hold!==null?'<div class="bar"><i></i></div>':"");
oled.querySelector("b").textContent=s.screen[0];[...oled.querySelectorAll("div:not(.bar)")].forEach((d,i)=>d.textContent=s.screen[i+1]);
if(s.hold!==null)oled.querySelector(".bar i").style.width=(s.hold*100)+"%";led.classList.toggle("on",s.led)}catch(e){oled.innerHTML="<b>restarting...</b>"}setTimeout(poll,90)}poll();
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    dev = None
    protocol_version = "HTTP/1.1"
    timeout = 5

    def parse_request(self):
        if not super().parse_request():
            return False
        port = self.server.server_port
        names = {f"localhost:{port}", f"127.0.0.1:{port}"}
        if port == 80:
            names.update(("localhost", "127.0.0.1"))
        host = self.headers.get("Host", "").lower()
        origin = self.headers.get("Origin")
        if (self.headers.get_all("Host") != [self.headers.get("Host")] or host not in names
                or (origin is not None and origin != "http://" + host)
                or self.headers.get("Sec-Fetch-Site") == "cross-site"):
            self.close_connection = True
            self.send_json(403, {"error": "Open the simulator on localhost"})
            return False
        return True

    def log_message(self, *a):
        pass

    def send_body(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Frame-Options", "SAMEORIGIN")
        self.send_header("Content-Security-Policy",
                         "default-src 'none'; script-src 'self' 'unsafe-inline'; style-src 'unsafe-inline'; "
                         "font-src 'self'; connect-src 'self'; frame-ancestors 'self'; base-uri 'none'; form-action 'none'")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, code, obj):
        self.send_body(code, json.dumps(obj).encode(), "application/json")

    def body(self):
        self.close_connection = True
        if self.headers.get_content_type() != "application/json":
            self.send_json(415, {"error": "Send JSON"})
            return None
        lengths = self.headers.get_all("Content-Length", [])
        if "Transfer-Encoding" in self.headers or len(lengths) != 1 or not re.fullmatch(r"[0-9]{1,5}", lengths[0]):
            self.send_json(400, {"error": "Invalid request length"})
            return None
        n = int(lengths[0])
        if n > 16384:
            self.send_json(413, {"error": "Request too large"})
            return None
        try:
            b = json.loads(self.rfile.read(n).decode("utf-8"))
            if not isinstance(b, dict):
                raise ValueError()
            return b
        except (ValueError, RecursionError):
            self.send_json(400, {"error": "Invalid JSON object"})
            return None

    def owns_session(self):
        token = self.headers.get("X-Solder-Session", "")
        return self.dev.unlocked and token.isascii() and secrets.compare_digest(token, self.dev.token)

    def session(self):
        d = self.dev
        if self.owns_session():
            if self.command == "POST":
                d.last_activity = time.time()
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

    def do_GET(self):
        d = self.dev
        path = self.path.split("?")[0]
        with d.lock:
            d.tick()
            if path == "/":
                self.send_body(200, (WEB / "index.html").read_bytes(), "text/html; charset=utf-8")
            elif path == "/device":
                self.send_body(200, DEVICE_PAGE.encode(), "text/html; charset=utf-8")
            elif path in ASSETS:
                f = ASSETS[path]
                self.send_body(200, f.read_bytes(), "font/woff2" if f.suffix == ".woff2" else "text/javascript")
            elif path == "/sim/state":
                if time.time() < d.down_until:
                    self.send_json(200, {"screen": ["restarting...", "", "", ""], "hold": None, "led": False})
                else:
                    waiting = d.req and d.req["state"] == "waiting"
                    self.send_json(200, {"screen": d.screen, "hold": d.req["hold"] if waiting else None, "led": d.led})
            elif self.restarting():
                return
            elif path == "/api/state":
                self.send_json(200, d.state(self.owns_session()))
            elif path == "/api/request":
                wiped = not d.has_wallet and d.req and d.req["kind"] == "wipe" and d.req["state"] == "done"
                if not wiped and not self.session():
                    return
                if not d.req or ("?" in self.path and self.path != f"/api/request?id={d.req['id']}"):
                    self.send_json(404, {"error": "No request"})
                else:
                    self.send_json(200, {"request": {k: v for k, v in d.req.items() if not k.startswith("_")}})
            else:
                self.send_json(404, {"error": "Unknown endpoint"})

    def do_POST(self):
        d = self.dev
        b = self.body()
        if b is None:
            return
        p = self.path
        with d.lock:
            d.tick()
            if p == "/sim/button":
                if type(b.get("down")) is not bool:
                    self.send_json(400, {"error": "Invalid button state"})
                    return
                d.pressed = b["down"]
                d.tick()
                self.send_json(200, {"ok": True})
                return
            if p == "/rpc":
                try:
                    time.sleep(0.12)
                    self.send_json(200, {"jsonrpc": "2.0", "id": b.get("id"), "result": d.chain.call(b["method"], b.get("params", []))})
                except Exception as e:  # noqa: BLE001
                    # A refused transfer has a definite preflight result.
                    code = -32002 if b.get("method") == "sendTransaction" and isinstance(e, ValueError) else -32000
                    self.send_json(200, {"jsonrpc": "2.0", "id": b.get("id"), "error": {"code": code, "message": str(e)}})
                return
            if self.restarting():
                return
            if p == "/api/ap-password":
                if (d.has_wallet or d.ap_pass != FACTORY_PASS) and not self.session():
                    return
                pw = b.get("password", "")
                if not isinstance(pw, str) or not re.fullmatch(r"[ -~]{8,63}", pw) or pw in (FACTORY_PASS, d.ap_pass, "admin123"):
                    self.send_json(400, {"error": "Choose a new password of 8 to 63 printable characters"})
                    return
                d.ap_pass = pw
                d.end_session()
                d.clear_setup()
                d.screen = ["Solder", "WiFi password set", "Restarting...", ""]
                self.send_json(200, {"restarting": True})
                d.down_until = time.time() + 6
                d.paint()
                return
            if p == "/api/setup/check" and not d.oled:
                self.send_json(404, {"error": "Unknown endpoint"})
                return
            if p in ("/api/setup/generate", "/api/setup/words", "/api/setup/check", "/api/setup/finish", "/api/setup/cancel"):
                if d.ap_pass == FACTORY_PASS:
                    self.send_json(428, {"error": "Change the WiFi password first"})
                    return
                if d.has_wallet:
                    self.send_json(409, {"error": "A wallet already exists"})
                    return
                if d.setup_token and d.setup_token != b.get("setupToken"):
                    self.send_json(409, {"error": "Setup is in progress in another tab"})
                    return
            if p == "/api/setup/cancel":
                d.clear_setup()
                d.paint()
                self.send_json(200, {"ok": True})
            elif p == "/api/setup/generate":
                if d.req and d.req["kind"] == "restore":
                    self.send_json(409, {"error": "Cancel the restore first"})
                    return
                n = b.get("words", 12)
                if type(n) is not int or n not in (12, 24):
                    self.send_json(400, {"error": "Choose 12 or 24 words"})
                    return
                d.clear_setup()
                d.pending_words = " ".join(random.sample(WORDS, n))
                d.setup_token, d.setup_time = secrets.token_hex(16), time.time()
                if d.oled:
                    d.show_words(0)
                    self.send_json(200, {"onScreen": True, "count": n, "setupToken": d.setup_token})
                else:
                    self.send_json(200, {"words": d.pending_words, "setupToken": d.setup_token})
            elif p == "/api/setup/words":
                page = b.get("page")
                if not d.pending_words or d.check_state == "checking":
                    self.send_json(409, {"error": "No backup page available"})
                elif type(page) is not int or not 0 <= page < len(d.pending_words.split()) // 6:
                    self.send_json(400, {"error": "Invalid backup page"})
                else:
                    if d.oled:
                        d.show_words(page)
                    d.setup_time = time.time()
                    self.send_json(200, {"ok": True})
            elif p == "/api/setup/check":
                action = b.get("action")
                if not d.pending_words:
                    self.send_json(409, {"error": "No on-device backup to check"})
                elif action not in ("start", "status"):
                    self.send_json(400, {"error": "Choose start or status"})
                else:
                    if action == "start" and d.check_state in ("idle", "incorrect"):
                        d.start_check()
                    self.send_json(200, {"state": d.check_state})
            elif p == "/api/setup/finish":
                staged = d.req and d.req["kind"] == "restore"
                if not d.setup_token and not b.get("mnemonic"):
                    self.send_json(409, {"error": "No setup in progress"})
                    return
                if staged and ("pin" in b or "mnemonic" in b):
                    self.send_json(409, {"error": "A restore is already staged"})
                    return
                if staged and d.req["state"] == "expired":
                    d.req = None
                    d.setup_token = ""
                    self.send_json(409, {"error": "Restore expired"})
                    return
                if staged and d.req["state"] == "waiting":
                    self.send_json(202, {"pending": True, "setupToken": d.setup_token})
                    return
                if not staged and not b.get("pin") and b.get("setupToken"):
                    self.send_json(409, {"error": "No pending restore"})
                    return
                pin = d.req["_pin"] if staged else b.get("pin")
                if not isinstance(pin, str) or not re.fullmatch(r"[0-9]{6}", pin):
                    self.send_json(400, {"error": "Use a six-digit PIN"})
                    return
                if d.pending_words and b.get("mnemonic"):
                    self.send_json(409, {"error": "Cancel the generated backup before restoring"})
                    return
                if d.oled and d.pending_words and d.check_state != "verified":
                    self.send_json(409, {"error": "Check the paper backup on Solder first"})
                    return
                words = d.req["_words"] if staged else b.get("mnemonic") or d.pending_words
                if not isinstance(words, str) or len(words.split()) not in (12, 24):
                    self.send_json(400, {"error": "Enter 12 or 24 words"})
                    return
                if not staged and b.get("mnemonic"):
                    d.setup_token, d.setup_time = secrets.token_hex(16), time.time()
                    d.req = {"kind": "restore", "state": "waiting", "_page": 0, "_words": words, "_pin": pin,
                             "_t": time.time(), "hold": 0}
                    d.wait_release, d.press_start = d.pressed, None
                    d.paint()
                    self.send_json(202, {"pending": True, "setupToken": d.setup_token})
                    return
                time.sleep(1.2)  # PBKDF2 on the ESP32 takes about this long
                d.has_wallet, d.pin, d.unlocked, d.token = True, pin, True, secrets.token_hex(16)
                d.req = None
                d.clear_setup()
                d.last_activity = time.time()
                d.paint()
                self.send_json(200, {"token": d.token, "address": ADDRESS})
            elif p == "/api/unlock":
                if not d.has_wallet:
                    self.send_json(409, {"error": "No wallet"})
                    return
                if time.time() < d.pin_delay:
                    self.send_json(403, {"error": "Wait a few seconds before the next try"})
                    return
                if d.fails >= 5:
                    if d.req and d.req["state"] == "waiting":
                        self.send_json(403, {"error": "Finish the pending request before unlocking"})
                        return
                    client, pin = b.get("pinClient"), b.get("pin")
                    if not isinstance(client, str) or not re.fullmatch(r"[0-9a-f]{32}", client):
                        self.send_json(403, {"error": "Reopen the wallet page before confirming a PIN attempt"})
                        return
                    if not isinstance(pin, str) or not re.fullmatch(r"[0-9]{6}", pin):
                        self.send_json(403, {"error": "Enter a six-digit PIN"})
                        return
                    if not d.pin_client:
                        d.pin_gate = True
                        d.pin_client, d.pin_candidate = client, pin
                        d.pin_allowed_until = time.time() + TIMEOUT_S
                        d.press_start = None
                        d.wait_release = d.pressed
                        d.paint()
                    elif not secrets.compare_digest(d.pin_client, client):
                        self.send_json(403, {"error": "Another PIN confirmation is pending. Wait for it to expire."})
                        return
                    elif not secrets.compare_digest(d.pin_candidate, pin):
                        self.send_json(403, {"error": "A different PIN is awaiting confirmation. Retry the original PIN or wait for it to expire."})
                        return
                    if d.pin_gate:
                        self.send_json(403, {"error": "Hold and release BOOT, then try the PIN again"})
                        return
                d.clear_pin_gate()
                time.sleep(0.9)
                if b.get("pin") != d.pin:
                    d.fails += 1
                    d.pin_delay = time.time() + d.fails
                    if d.fails == 10:
                        d.fails, d.has_wallet, d.pin = 0, False, ""
                        d.end_session()
                        self.send_json(403, {"error": "Too many wrong PINs. Restore with your backup words."})
                        return
                    self.send_json(403, {"error": "Wrong PIN, %d tries left" % (10 - d.fails)})
                    return
                d.end_session()
                d.unlocked, d.token, d.fails = True, secrets.token_hex(16), 0
                d.last_activity = time.time()
                d.paint()
                self.send_json(200, {"token": d.token, "address": ADDRESS})
            elif p == "/api/lock":
                if not self.session():
                    return
                d.end_session()
                self.send_json(200, {"ok": True})
            elif p in ("/api/send", "/api/send/preview", "/api/sign", "/api/wipe"):
                if not self.session():
                    return
                if p != "/api/send/preview" and d.req and d.req["state"] == "waiting":
                    self.send_json(409, {"error": "Another request is waiting for the button"})
                    return
                if p in ("/api/send", "/api/send/preview"):
                    to, amt, bh = b.get("to", ""), b.get("amount", ""), b.get("blockhash", "")
                    if not isinstance(to, str) or not re.fullmatch(f"[{B58}]{{32,44}}", to) or to == ADDRESS:
                        self.send_json(400, {"error": "That is not a Solana address"})
                        return
                    if not isinstance(bh, str) or not re.fullmatch(f"[{B58}]{{32,44}}", bh):
                        self.send_json(400, {"error": "Missing recent blockhash, is your phone online?"})
                        return
                    mint = b.get("mint", "")
                    if not isinstance(mint, str):
                        self.send_json(400, {"error": "Invalid token mint"})
                        return
                    decimals = b.get("decimals") if mint else 9
                    program = b.get("program", TOKEN_PROGRAM) if mint else ""
                    if mint and (not re.fullmatch(f"[{B58}]{{32,44}}", mint) or type(decimals) is not int
                                 or not 0 <= decimals <= 18 or program not in (TOKEN_PROGRAM, TOKEN_2022_PROGRAM)):
                        self.send_json(400, {"error": "Unknown token"})
                        return
                    if not isinstance(amt, str) or not re.fullmatch(r"[0-9]{0,20}([.,][0-9]{0,18})?", amt):
                        self.send_json(400, {"error": "Invalid amount"})
                        return
                    whole, _, frac = amt.replace(",", ".").partition(".")
                    units = int(whole or 0) * 10 ** decimals + int(frac.ljust(decimals, "0") or 0)
                    if len(frac) > decimals or not 0 < units <= 2**64 - 1:
                        self.send_json(400, {"error": "Invalid amount"})
                        return
                    transfer = {"kind": "send-token" if mint else "send", "to": to, "mint": mint, "program": program,
                                "decimals": decimals, "units": units, "blockhash": bh}
                    if p == "/api/send/preview":
                        quote = {"amountUnits": str(units), "message": pack_message(transfer)}
                        if mint:
                            quote["destinationAccount"] = simulated_ata(to, mint, program)
                        if program == TOKEN_2022_PROGRAM:
                            quote["sizeTransaction"] = pack_message({"kind": "GetAccountDataSize", "mint": mint, "blockhash": bh})
                        self.send_json(200, quote)
                        return
                    req = {"kind": transfer["kind"], "to": to, "amount": amt.replace(",", "."),
                           "symbol": mint[:4] if mint else "SOL", "mint": mint, "_transfer": transfer}
                elif p == "/api/sign":
                    message = b.get("message")
                    if (not isinstance(message, str) or not message or ord(message[0]) < 32
                            or re.search(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f-\x9f\ud800-\udfff]", message)
                            or len(message.encode()) > 1024):
                        self.send_json(400, {"error": "Write a text message up to 1024 bytes"})
                        return
                    req = {"kind": "sign-message", "message": b["message"]}
                else:
                    req = {"kind": "wipe"}
                req.update({"id": d.next_id, "state": "waiting", "hold": 0, "expiresIn": TIMEOUT_S, "_t": time.time(), "_page": 0})
                d.next_id += 1
                d.clear_pin_gate()
                d.wait_release, d.press_start = d.pressed, None
                d.req = req
                d.paint()
                self.send_json(200, {"request": {k: v for k, v in req.items() if not k.startswith("_")}})
            elif p == "/api/request/reject":
                if not self.session():
                    return
                if not d.req or b.get("id") != d.req["id"]:
                    self.send_json(409, {"error": "Request changed"})
                    return
                if d.req and d.req["state"] == "waiting":
                    d.req["state"] = "rejected"
                d.paint()
                self.send_json(200, {"request": {k: v for k, v in d.req.items() if not k.startswith("_")}})
            elif p == "/api/network":
                if not self.session():
                    return
                network, mins = b.get("network"), b.get("autoLockMin", d.auto_lock_min)
                if (network not in ("devnet", "mainnet", "custom") or type(mins) is not int or mins not in (0, 1, 5, 15)
                        or (network == "custom" and not str(b.get("rpc", "")).startswith("https://"))):
                    self.send_json(400, {"error": "Invalid settings"})
                    return
                d.network, d.auto_lock_min = network, mins
                self.send_json(200, {"ok": True})
            else:
                self.send_json(404, {"error": "Unknown endpoint"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--ready", action="store_true", help="password set, wallet made, PIN 123456")
    ap.add_argument("--no-oled", action="store_true", help="simulate the board-only firmware")
    a = ap.parse_args()
    Handler.dev = Device(a.ready, oled=not a.no_oled)
    print("Solder simulator")
    print("  wallet: http://localhost:%d" % a.port)
    print("  device: http://localhost:%d/device   (hold BOOT here)" % a.port)
    print("  factory WiFi password: %s, PIN after setup: what you choose%s" % (FACTORY_PASS, " (ready mode: 123456)" if a.ready else ""))
    ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
