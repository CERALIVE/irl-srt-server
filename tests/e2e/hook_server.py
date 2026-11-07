"""Webhook stand-in for the e2e scripts: on_event_url and player_key_auth_url.

Every request is appended to stdout as one JSON object per line, with the
parsed query, so a script can wait for an exact hook and read its session_id.

Player on_connect verdicts are chosen by the caller's remote_port, which the
test client controls with --bind-port:
  --slow PORTS      answer 200 after --slow-ms (the reader stays pending)
  --hold PORTS      answer 200 once POST /release arrives, or after
                    --hold-max-ms; every record carries "held", the number
                    of on_connect requests held when it arrived
  --reserved PORTS  answer 200 with {"reserved": true}
  --deny PORTS      answer 403
PORTS is "a-b" or "a". --deny-substring S answers 403 to any on_connect
whose srt_url contains S. Everything else, and every publisher, gets 200.

Player-key lookups (GET <any path>?player_key=K) answer {"stream_id": ...}
for each --player-key K=STREAM_ID. For each --reserved-player-key K=STREAM_ID@IP
(a hosted or preview key bound to IP) the answer adds "internal_class" ("p" for
a pvw_ key, else "h") and "bound_peer": IP, but only when the lookup's
remote_ip is IP, as the platform verifies the binding. 403 otherwise.
"""

import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


def port_range(text: str) -> range:
    low, _, high = text.partition("-")
    return range(int(low), int(high or low) + 1)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("port", type=int)
    parser.add_argument("--slow", type=port_range, action="append", default=[])
    parser.add_argument("--reserved", type=port_range, action="append", default=[])
    parser.add_argument("--deny", type=port_range, action="append", default=[])
    parser.add_argument("--slow-ms", type=int, default=2000)
    parser.add_argument("--hold", type=port_range, action="append", default=[])
    parser.add_argument("--hold-max-ms", type=int, default=3500)
    parser.add_argument("--deny-substring", action="append", default=[])
    parser.add_argument("--player-key", action="append", default=[])
    parser.add_argument("--reserved-player-key", action="append", default=[])
    args = parser.parse_args()
    player_keys = dict(item.split("=", 1) for item in args.player_key)
    reserved_keys = dict(item.split("=", 1) for item in args.reserved_player_key)

    def within(port: int, ranges: list) -> bool:
        return any(port in r for r in ranges)

    hold_state = {"held": 0, "generation": 0}
    hold_cond = threading.Condition()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args: object) -> None:
            pass

        def reply(self, status: int, body: dict) -> None:
            data = json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def record(self, method: str, status: int, held: int = 0) -> None:
            url = urlparse(self.path)
            query = {k: v[0] for k, v in parse_qs(url.query).items()}
            entry = {"t": time.monotonic(), "method": method, "path": url.path, "status": status, "held": held,
                     **query}
            sys.stdout.write(json.dumps(entry) + "\n")
            sys.stdout.flush()

        def do_GET(self) -> None:
            query = parse_qs(urlparse(self.path).query)
            key = query.get("player_key", [""])[0]
            if key in player_keys:
                self.record("GET", 200)
                self.reply(200, {"stream_id": player_keys[key]})
            elif key in reserved_keys:
                sid, _, bound = reserved_keys[key].partition("@")
                body = {"stream_id": sid}
                peer = query.get("remote_ip", [""])[0]
                peer = peer[len("::ffff:"):] if peer.startswith("::ffff:") else peer
                if bound and peer == bound:
                    body["internal_class"] = "p" if key.startswith("pvw_") else "h"
                    body["bound_peer"] = bound
                self.record("GET", 200)
                self.reply(200, body)
            elif key:
                self.record("GET", 403)
                self.reply(403, {"error": "unknown key"})
            else:
                self.reply(200, {"status": "ok"})

        def do_POST(self) -> None:
            if urlparse(self.path).path == "/release":
                with hold_cond:
                    hold_state["generation"] += 1
                    hold_cond.notify_all()
                self.reply(200, {"status": "released"})
                return
            query = parse_qs(urlparse(self.path).query)
            event = query.get("on_event", [""])[0]
            role = query.get("role_name", [""])[0]
            port = int(query.get("remote_port", ["0"])[0] or 0)
            body: dict = {"status": "ok"}
            status = 200
            slow = False
            hold = False
            srt_url = query.get("srt_url", [""])[0]
            if event == "on_connect" and any(s in srt_url for s in args.deny_substring):
                status = 403
            elif event == "on_connect" and role == "player":
                if within(port, args.deny):
                    status = 403
                elif within(port, args.slow):
                    slow = True
                elif within(port, args.hold):
                    hold = True
                elif within(port, args.reserved):
                    body["reserved"] = True
            # Recorded on arrival, so a script sees a slow or held hook while it
            # waits.
            with hold_cond:
                held_now = hold_state["held"]
                generation = hold_state["generation"]
                if hold:
                    hold_state["held"] += 1
            self.record("POST", status, held_now)
            if slow:
                time.sleep(args.slow_ms / 1000.0)
            if hold:
                with hold_cond:
                    hold_cond.wait_for(lambda: hold_state["generation"] != generation, args.hold_max_ms / 1000.0)
                    hold_state["held"] -= 1
            self.reply(status, body)

    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
