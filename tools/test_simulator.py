"""Regression checks for the simulator HTTP boundary and wallet approval flows."""
import base64
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
        for endpoint in ("ap-password", "network", "lock", "request/reject", "wipe", "sign", "send", "send/preview"):
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
        self.dev.oled = False
        first = json.load(self.call("/api/setup/generate", {"words": 12}))
        self.assertEqual(len(first["words"].split()), 12)
        self.assertEqual(self.call("/api/setup/check", {"action": "start", "setupToken": first["setupToken"]}).status, 404)
        self.assertEqual(self.call("/api/setup/generate", {"words": 24}).status, 409)
        self.assertEqual(self.call("/api/setup/finish", {"pin": "123456"}).status, 409)
        new = json.load(self.call("/api/setup/generate", {"words": 24, "setupToken": first["setupToken"]}))
        self.assertNotEqual(first["setupToken"], new["setupToken"])
        self.assertEqual(self.call("/api/setup/finish", {"pin": "123456", "setupToken": first["setupToken"]}).status, 409)
        self.assertEqual(self.call("/api/setup/finish", {"pin": "123456", "setupToken": new["setupToken"]}).status, 200)
        self.assertFalse(self.dev.pending_words)

    def test_oled_paper_copy_and_button_check(self):
        for count in (12, 24):
            with self.subTest(count=count):
                self.dev.has_wallet = False
                created = json.load(self.call("/api/setup/generate", {"words": count}))
                owner = {"setupToken": created["setupToken"]}
                self.assertEqual(created, {**owner, "onScreen": True, "count": count})
                paper = []
                for page in range(count // 6):
                    self.assertEqual(self.call("/api/setup/words", {**owner, "page": page}).status, 200)
                    screen = json.load(self.call("/sim/state"))["screen"]
                    paper.extend(line.split(". ")[1] for line in screen[1:])
                self.assertEqual(len(paper), count)

                def check(action):
                    return json.load(self.call("/api/setup/check", {**owner, "action": action}))

                finish = {**owner, "pin": "123456"}
                self.assertEqual(check("status"), {"state": "idle"})
                self.assertEqual(self.call("/api/setup/finish", finish).status, 409)
                self.call("/sim/button", {"down": True})
                self.assertEqual(check("start"), {"state": "checking"})
                first_question = self.dev.screen[:]
                self.clock.return_value += 2
                self.call("/sim/button", {"down": False})
                self.assertEqual(check("start"), {"state": "checking"})
                self.assertEqual(self.dev.screen, first_question)
                self.assertEqual(self.call("/api/setup/words", {**owner, "page": 0}).status, 409)

                # Submit one wrong word, then review the original (unchanged) copy.
                _, question, *options, _ = first_question
                correct = paper[int(question.removeprefix("Word #").removesuffix("?")) - 1]
                if options[0].strip("> ") == correct:
                    self.press(0.2)
                self.press(1.6)
                self.assertEqual(check("status"), {"state": "incorrect"})
                self.assertEqual(self.call("/api/setup/finish", finish).status, 409)
                self.call("/api/setup/words", {**owner, "page": 0})
                self.assertEqual([s.split(". ")[1] for s in self.dev.screen[1:]], paper[:6])
                self.assertEqual(check("start"), {"state": "checking"})

                asked = set()
                for _ in range(3):
                    _, question, *options, _ = json.load(self.call("/sim/state"))["screen"]
                    pos = int(question.removeprefix("Word #").removesuffix("?")) - 1
                    self.assertNotIn(pos, asked)
                    asked.add(pos)
                    choices = [w.strip("> ") for w in options]
                    self.assertEqual(len(set(choices)), 3)
                    self.assertTrue(set(choices).issubset(paper))
                    for _ in range(choices.index(paper[pos])):
                        self.press(0.2)
                    self.assertEqual(self.call("/api/setup/finish", finish).status, 409)
                    self.call("/sim/button", {"down": True})
                    self.clock.return_value += 1.6
                    answer = check("status")
                    self.clock.return_value += 3
                    self.assertEqual(check("status"), answer)
                    self.call("/sim/button", {"down": False})
                self.assertEqual(check("status"), {"state": "verified"})
                self.assertEqual(check("start"), {"state": "verified"})
                self.assertEqual(self.call("/api/setup/finish", finish).status, 200)
                self.assertFalse(self.dev.pending_words)

    def test_check_choices_for_repeated_words(self):
        self.dev.pending_words = " ".join(["ability"] * 12)
        self.dev.start_check()
        for _, choices in self.dev.questions:
            self.assertCountEqual(choices, ["ability", "cactus", "yellow"])

    def test_oled_check_is_bound_to_one_setup_and_expires_without_button_activity(self):
        self.dev.has_wallet = False
        first = json.load(self.call("/api/setup/generate", {"words": 12}))["setupToken"]
        start = {"setupToken": first, "action": "start"}
        self.assertEqual(self.call("/api/setup/check", {"action": "start"}).status, 409)
        self.assertEqual(self.call("/api/setup/check", start).status, 200)
        new = json.load(self.call("/api/setup/generate", {"words": 24, "setupToken": first}))["setupToken"]
        self.assertEqual(self.call("/api/setup/check", start).status, 409)
        status = {"action": "status", "setupToken": new}
        self.assertEqual(json.load(self.call("/api/setup/check", status)), {"state": "idle"})
        self.call("/api/setup/check", {**status, "action": "start"})
        self.clock.return_value += 899
        self.assertEqual(json.load(self.call("/api/setup/check", status)), {"state": "checking"})
        self.clock.return_value += 2
        self.assertEqual(self.call("/api/setup/check", status).status, 409)
        self.assertFalse(self.dev.pending_words)
        self.assertFalse(self.dev.questions)
        new = json.load(self.call("/api/setup/generate", {"words": 12}))["setupToken"]
        self.call("/api/setup/check", {**start, "setupToken": new})
        self.assertEqual(self.call("/api/setup/cancel", {"setupToken": new}).status, 200)
        self.assertEqual(self.call("/api/setup/finish", {"setupToken": new, "pin": "123456"}).status, 409)
        self.assertFalse(self.dev.questions)

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
        transfer = {"to": simulator.USDC, "amount": "0.1", "blockhash": bh}
        quote = json.load(self.call("/api/send/preview", transfer, token=token))
        self.assertCountEqual(quote, ["message", "amountUnits"])
        self.assertEqual(quote["amountUnits"], "100000000")
        self.assertEqual(self.dev.chain.call("getFeeForMessage", [quote["message"]])["value"], 5000)
        self.assertIsNone(self.dev.req)
        self.assertIsNone(self.dev.chain.signed)
        req = json.load(self.call("/api/send", transfer, token=token))["request"]
        self.assertEqual(json.load(self.call("/api/send/preview", transfer, token=token)), quote)
        self.assertEqual(self.dev.req["id"], req["id"])
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

    def test_token_previews_match_first_and_repeat_transfer_cost(self):
        token = self.login()
        chain = self.dev.chain
        for program, mint, size in ((simulator.TOKEN_PROGRAM, simulator.USDC, 165),
                                    (simulator.TOKEN_2022_PROGRAM, simulator.TOKEN_2022_MINT, 170)):
            with self.subTest(program=program):
                transfer = {"to": "1" * 32, "amount": "2.5", "decimals": 6, "mint": mint,
                            "program": program, "blockhash": "1" * 32}
                quote = json.load(self.call("/api/send/preview", transfer, token=token))
                dest = quote["destinationAccount"]
                self.assertEqual(quote["amountUnits"], "2500000")
                self.assertIsNone(chain.call("getAccountInfo", [dest])["value"])
                if program == simulator.TOKEN_2022_PROGRAM:
                    result = chain.call("simulateTransaction", [quote["sizeTransaction"]])["value"]
                    self.assertIsNone(result["err"])
                    self.assertEqual(result["returnData"]["programId"], program)
                    raw, encoding = result["returnData"]["data"]
                    self.assertEqual(encoding, "base64")
                    self.assertEqual(base64.b64decode(raw), size.to_bytes(8, "little"))
                else:
                    self.assertNotIn("sizeTransaction", quote)

                fee = chain.call("getFeeForMessage", [quote["message"]])["value"]
                minimum = chain.call("getMinimumBalanceForRentExemption", [size])
                # Partial funding is possible even before the ATA is initialized.
                prefunded = 10000 if program == simulator.TOKEN_2022_PROGRAM else 0
                if prefunded:
                    chain.accounts[dest] = simulator.simulated_account(simulator.SYSTEM_PROGRAM, prefunded)
                for cost in (fee + minimum - prefunded, fee):
                    before = chain.lamports
                    req = json.load(self.call("/api/send", transfer, token=token))["request"]
                    while self.dev.req["_page"] + 1 < self.dev.req["_pages"]:
                        self.press(0.2)
                    self.press(1.6)
                    signed = json.load(self.call("/api/request?id=" + str(req["id"]), token=token))["request"]
                    wire = json.loads(base64.b64decode(signed["tx"]))
                    self.assertEqual(wire["message"], json.loads(base64.b64decode(quote["message"])))
                    with patch.object(chain, "lamports", cost - 1):
                        with self.assertRaisesRegex(ValueError, "Insufficient balance"):
                            chain.call("sendTransaction", [signed["tx"]])
                    for _ in range(2):
                        self.assertEqual(chain.call("sendTransaction", [signed["tx"]]), signed["signature"])
                    self.assertEqual(before - chain.lamports, cost)
                    account = chain.call("getAccountInfo", [dest])["value"]
                    self.assertEqual((account["owner"], account["lamports"], account["space"]), (program, minimum, size))


if __name__ == "__main__":
    unittest.main()
