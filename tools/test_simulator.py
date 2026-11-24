"""Regression checks for the simulator HTTP boundary and wallet approval flows."""
import io
import json
import unittest
from http.client import HTTPResponse
from types import SimpleNamespace
from unittest.mock import patch

import simulator


class MemorySocket:
    def __init__(self, data=b""):
        self.data = io.BytesIO(data)
        self.output = bytearray()

    def makefile(self, *args):
        return self.data

    def sendall(self, data):
        self.output.extend(data)

    def settimeout(self, timeout):
        pass


class SimulatorTests(unittest.TestCase):
    def setUp(self):
        self.dev = simulator.Device(ready=True)
        self.server = SimpleNamespace(server_port=8080)
        self.handler = type("TestHandler", (simulator.Handler,), {"dev": self.dev})
        self.clock = patch("simulator.time.time", return_value=1000000).start()
        self.addCleanup(patch.stopall)
        patch("simulator.time.sleep").start()

    def call(self, path, body=None, *, token="", host="localhost:8080", origin=None, ctype="application/json"):
        raw = b"" if body is None else json.dumps(body).encode()
        headers = [f"{'GET' if body is None else 'POST'} {path} HTTP/1.1",
                   f"Host: {host}", f"X-Solder-Session: {token}", "Connection: close"]
        if origin is not None:
            headers.append("Origin: " + origin)
        if body is not None:
            headers.extend([f"Content-Length: {len(raw)}", f"Content-Type: {ctype}"])
        sock = MemorySocket(("\r\n".join(headers) + "\r\n\r\n").encode() + raw)
        self.handler(sock, ("127.0.0.1", 1234), self.server)
        response = HTTPResponse(MemorySocket(sock.output))
        response.begin()
        return response

    def login(self):
        return json.load(self.call("/api/unlock", {"pin": "123456"}))["token"]

    def press(self, seconds):
        self.call("/sim/button", {"down": True})
        self.clock.return_value += seconds
        self.call("/sim/button", {"down": False})

    def test_http_boundaries_and_public_assets(self):
        self.assertEqual(self.call("/api/state", host="unrecognized.test").status, 403)
        self.assertEqual(self.call("/sim/button", {"down": True}, origin="https://unrecognized.test").status, 403)
        self.assertEqual(self.call("/sim/button", {"down": True}, ctype="text/plain").status, 415)
        self.assertEqual(self.call("/api/unlock", []).status, 400)
        self.assertEqual(self.call("/api/unlock", {"pad": "0" * 17000}).status, 413)
        self.assertEqual(self.call("/vendor/unlisted.txt").status, 404)
        self.assertNotIn("/vendor/other.js", simulator.ASSETS)
        for path in simulator.ASSETS:
            res = self.call(path)
            self.assertEqual(res.status, 200)
            self.assertIsNone(res.headers["Access-Control-Allow-Origin"])

    def test_session_privacy_and_mutating_routes(self):
        token = self.login()
        public = json.load(self.call("/api/state"))
        self.assertFalse(public["unlocked"])
        self.assertEqual(public["address"], "")
        self.assertEqual(public["rpc"], "")
        self.assertEqual(json.load(self.call("/api/state", token=token))["address"], simulator.ADDRESS)
        for endpoint in ("ap-password", "network", "lock", "request/reject", "wipe", "sign", "send"):
            self.assertEqual(self.call("/api/" + endpoint, {}).status, 401, endpoint)
        self.assertEqual(self.call("/api/setup/finish", {}).status, 409)
        self.clock.return_value += 299
        self.call("/api/state", token=token)
        self.clock.return_value += 2
        self.assertFalse(json.load(self.call("/api/state", token=token))["unlocked"])

    def test_request_identity_and_fresh_button_press(self):
        token = self.login()
        self.call("/sim/button", {"down": True})
        r = json.load(self.call("/api/sign", {"message": "Login"}, token=token))["request"]
        self.clock.return_value += 2
        self.assertEqual(self.call("/api/request?id=999", token=token).status, 404)
        self.assertEqual(self.call("/api/request/reject", {"id": r["id"] + 1}, token=token).status, 409)
        self.assertEqual(self.dev.req["state"], "waiting")
        self.call("/sim/button", {"down": False})
        self.press(1.6)
        self.assertEqual(self.dev.req["state"], "done")
        self.assertTrue(self.dev.req["signature"])
        self.call("/api/lock", {}, token=token)
        self.assertIsNone(self.dev.req)

    def test_generated_backup_is_bound_to_setup_owner(self):
        self.dev.has_wallet = False
        first = json.load(self.call("/api/setup/generate", {"words": 12}))
        self.assertEqual(len(first["words"].split()), 12)
        self.assertEqual(self.call("/api/setup/generate", {"words": 24}).status, 409)
        self.assertEqual(self.call("/api/setup/finish", {"pin": "123456"}).status, 409)
        new = json.load(self.call("/api/setup/generate", {"words": 24, "setupToken": first["setupToken"]}))
        self.assertNotEqual(first["setupToken"], new["setupToken"])
        self.assertEqual(self.call("/api/setup/finish", {"pin": "123456", "setupToken": first["setupToken"]}).status, 409)
        self.assertEqual(self.call("/api/setup/finish", {"pin": "123456", "setupToken": new["setupToken"]}).status, 200)
        self.assertFalse(self.dev.pending_words)

    def test_staged_restore_needs_all_pages_and_owner(self):
        self.dev.has_wallet = False
        res = self.call("/api/setup/finish", {"mnemonic": " ".join(simulator.WORDS[:12]), "pin": "314159"})
        self.assertEqual(res.status, 202)
        claim = json.load(res)["setupToken"]
        self.assertEqual(self.call("/api/setup/cancel", {}).status, 409)
        self.assertEqual(self.call("/api/setup/finish", {"setupToken": claim}).status, 202)
        self.assertFalse(self.dev.has_wallet)
        self.assertEqual(self.call("/api/request").status, 401)
        while self.dev.req["_page"] + 1 < self.dev.req["_pages"]:
            self.press(0.2)
        self.press(1.6)
        self.assertFalse(self.dev.has_wallet)
        self.assertEqual(self.call("/api/setup/finish", {"setupToken": claim}).status, 200)
        self.assertEqual(self.dev.pin, "314159")
        self.assertIsNone(self.dev.req)

    def test_pin_retry_physical_gate(self):
        self.dev.fails = 5
        owner, other = "0" * 32, "1" * 32
        pending = {"pin": "123456", "pinClient": owner}
        self.assertEqual(self.call("/api/unlock", {"pin": "123456"}).status, 403)
        self.assertFalse(self.dev.pin_gate)
        self.assertEqual(self.call("/api/unlock", pending).status, 403)
        deadline = self.dev.pin_allowed_until
        wrong = {"pin": "654321", "pinClient": owner}
        self.assertEqual(self.call("/api/unlock", wrong).status, 403)
        self.assertEqual(self.dev.pin_allowed_until, deadline)
        self.press(1.6)
        deadline = self.dev.pin_allowed_until
        responses = []
        for candidate in (pending["pin"], wrong["pin"]):
            response = self.call("/api/unlock", {"pin": candidate, "pinClient": other})
            self.assertEqual(response.status, 403)
            responses.append(json.load(response)["error"])
        self.assertEqual(responses[0], responses[1])
        self.assertEqual(self.call("/api/unlock", wrong).status, 403)
        self.assertEqual(self.dev.fails, 5)
        self.assertEqual(self.dev.pin_allowed_until, deadline)
        self.assertEqual(self.call("/api/unlock", pending).status, 200)
        self.assertEqual(self.dev.fails, 0)

    def test_bound_pin_attempt_expires(self):
        self.dev.fails = 5
        pending = {"pin": "123456", "pinClient": "0" * 32}
        self.call("/api/unlock", pending)
        self.clock.return_value += 46
        self.dev.tick()
        self.assertFalse(self.dev.pin_client)
        self.call("/api/unlock", pending)
        self.press(1.6)
        self.clock.return_value += 31
        self.assertEqual(self.call("/api/unlock", pending).status, 403)
        self.assertTrue(self.dev.pin_gate)
        self.assertEqual(self.dev.fails, 5)

    def test_transfer_review_and_broadcast(self):
        token = self.login()
        bh = "1" * 32
        req = json.load(self.call("/api/send", {"to": simulator.USDC, "amount": "0.1", "blockhash": bh}, token=token))["request"]
        self.press(1.6)
        self.assertEqual(self.dev.req["state"], "waiting")
        while self.dev.req["_page"] + 1 < self.dev.req["_pages"]:
            self.press(0.2)
        self.press(1.6)
        signed = json.load(self.call("/api/request?id=" + str(req["id"]), token=token))["request"]
        submission = {"jsonrpc": "2.0", "id": 1, "method": "sendTransaction", "params": [signed["tx"]]}
        lookup = {"jsonrpc": "2.0", "id": 1, "method": "getSignatureStatuses",
                  "params": [[signed["signature"]], {"searchTransactionHistory": True}]}
        with patch.object(self.dev.chain, "lamports", 0):
            self.assertEqual(json.load(self.call("/rpc", submission))["error"]["code"], -32002)
        self.assertEqual(json.load(self.call("/rpc", lookup))["result"]["value"], [None])
        for _ in range(2):
            self.assertEqual(json.load(self.call("/rpc", submission))["result"], signed["signature"])
        self.assertEqual(json.load(self.call("/rpc", lookup))["result"]["value"][0]["confirmationStatus"], "processed")
        self.clock.return_value += 2
        self.assertEqual(json.load(self.call("/rpc", lookup))["result"]["value"][0]["confirmationStatus"], "confirmed")
        self.assertEqual(self.dev.chain.lamports, 1_899_995_000)


if __name__ == "__main__":
    unittest.main()
