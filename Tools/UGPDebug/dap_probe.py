#!/usr/bin/env python3
"""dap_probe.py - raw DAP client for codelldb: does an absolute-path file:line
breakpoint bind and hit? Prints the DAP event flow for diagnosis.

Usage: python dap_probe.py [line]
Pre:   Tools/UGPDebug/prepare_scenario.bat  (fresh CDN, no install dir)
"""
import json
import os
import subprocess
import sys
import threading
import time

REPO = r"D:\UEProject\IOStoreDemo"
ADAPTER = REPO + r"\Saved\UGPDebug\codelldb\extension\adapter\codelldb.exe"
LIBLLDB = REPO + r"\Saved\UGPDebug\codelldb\extension\lldb\bin\liblldb.dll"
EXE = REPO + r"\Binaries\Win64\UnrealGamePatcher.exe"
SRC = REPO + r"\Source\Programs\UnrealGamePatcher\UnrealGamePatcherCore\Private\Update\FUpdateOrchestrator.cpp"
LINE = int(sys.argv[1]) if len(sys.argv) > 1 else 435

class DapClient:
    def __init__(self, proc):
        self.proc = proc
        self.seq = 0
        self.responses = {}
        self.events = []
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def _read_msg(self):
        import re
        headers = {}
        while True:
            line = b""
            while not line.endswith(b"\r\n"):
                c = self.proc.stdout.read(1)
                if not c:
                    return None
                line += c
            s = line.strip().decode("utf-8", "replace")
            if not s:
                if headers.get("content-length") is not None:
                    body = self.proc.stdout.read(headers["content-length"])
                    return json.loads(body)
                headers = {}  # stray blank line from debuggee stdout
                continue
            m = re.match(r"content-length:\s*(\d+)$", s, re.I)
            if m:
                headers["content-length"] = int(m.group(1))
            # anything else = debuggee stdout leaking into the stream: skip

    def _reader(self):
        while True:
            msg = self._read_msg()
            if msg is None:
                return
            with self.lock:
                if msg.get("type") == "response":
                    self.responses[msg["request_seq"]] = msg
                else:
                    self.events.append(msg)
            if msg.get("type") == "event":
                print("EVENT:", msg.get("event"), json.dumps(msg.get("body", {}))[:200])

    def request(self, command, args=None, timeout=60):
        with self.lock:
            self.seq += 1
            rid = self.seq
        m = {"command": command, "arguments": args or {}, "type": "request", "seq": rid}
        body = json.dumps(m).encode()
        self.proc.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                r = self.responses.get(rid)
            if r is not None:
                if not r.get("success"):
                    raise RuntimeError(f"{command} failed: {json.dumps(r.get('message', ''))[:250]}")
                return r
            time.sleep(0.05)
        raise TimeoutError(command)

def main():
    proc = subprocess.Popen([ADAPTER, "--liblldb", LIBLLDB],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    cli = DapClient(proc)
    cli.request("initialize", {"adapterID": "codelldb", "capabilities": {}}, timeout=60)
    # Fire launch without waiting: codelldb answers it only after
    # configurationDone; meanwhile it emits the `initialized` event.
    with cli.lock:
        cli.seq += 1
        launch_seq = cli.seq
    launch_msg = {"command": "launch", "type": "request", "seq": launch_seq,
                  "arguments": {
                      "program": EXE,
                      "cwd": REPO,
                      "stopOnEntry": True,
                      "args": ["-cli", "update",
                               "-cdn=file:///D:/UEProject/IOStoreDemo/Saved/UGPDebug/cdn",
                               "-install=D:/UEProject/IOStoreDemo/Saved/UGPDebug/install3"],
                  }}
    lbody = json.dumps(launch_msg).encode()
    proc.stdin.write(f"Content-Length: {len(lbody)}\r\n\r\n".encode() + lbody)
    proc.stdin.flush()
    t0 = time.time()
    while time.time() - t0 < 120:
        with cli.lock:
            has_stop = any(e.get("event") == "stopped" for e in cli.events)
        if has_stop:
            print("ENTRY STOP reached (symbols settled)")
            break
        time.sleep(0.2)
    r = cli.request("setBreakpoints", {
        "source": {"name": os.path.basename(SRC), "path": SRC},
        "breakpoints": [{"line": LINE}],
        "lines": [LINE],
    })
    print("setBreakpoints ->", json.dumps(r.get("body", {}))[:300])
    cli.request("configurationDone")
    with cli.lock:
        stops_before = sum(1 for e in cli.events if e.get("event") == "stopped")
    cli.request("continue", {"threadId": 0})

    t0 = time.time()
    new_stop = None
    while time.time() - t0 < 180:
        with cli.lock:
            stops = [e for e in cli.events if e.get("event") == "stopped"]
            done = [e for e in cli.events if e.get("event") in ("exited", "terminated")]
        if len(stops) > stops_before:
            new_stop = stops[-1]
            print("== DAP PROBE: breakpoint HIT ==")
            break
        if done:
            print("== DAP PROBE: process finished, breakpoint never hit ==")
            break
        time.sleep(0.2)
    else:
        print("== DAP PROBE: timeout ==")

    if new_stop:

        tid = new_stop.get("body", {}).get("threadId")
        try:
            st = cli.request("stackTrace", {"threadId": tid})
            frames = st.get("body", {}).get("stackFrames", [])
            print("TOP FRAME:", json.dumps(frames[0], ensure_ascii=False)[:300] if frames else None)
        except Exception as e:
            print("stackTrace failed:", str(e)[:150])
    try:
        cli.request("terminate")
    except Exception:
        pass
    time.sleep(1)
    try:
        proc.kill()
    except Exception:
        pass

if __name__ == "__main__":
    main()
