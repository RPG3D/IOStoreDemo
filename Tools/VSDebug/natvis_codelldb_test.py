#!/usr/bin/env python3
r"""natvis_codelldb_test.py - verify UE.natvis makes FString readable through
the codelldb evaluate lane (debugmcp adapter must have natvis registered, or
pass natvis in launch args as this script does).

Usage: python Tools/VSDebug/natvis_codelldb_test.py
Pre:   CDN prepared (UGPDebug/cdn), install dir absent.
"""
import json
import subprocess
import sys
import time

REPO = r"D:\UEProject\IOStoreDemo"
ADAPTER = REPO + r"\Saved\UGPDebug\codelldb\extension\adapter\codelldb.exe"
LIBLLDB = REPO + r"\Saved\UGPDebug\codelldb\extension\lldb\bin\liblldb.dll"
EXE = REPO + r"\Binaries\Win64\UnrealGamePatcher.exe"
SRC = REPO + r"\Source\Programs\UnrealGamePatcher\UnrealGamePatcherCore\Private\Update\FUpdateOrchestrator.cpp"
NATVIS = r"D:\Code\UE5\Engine\Extras\VisualStudioDebugging\Unreal.natvis"
INSTALL = REPO + r"\Saved\UGPDebug\install"
CDN = REPO + r"\Saved\UGPDebug\cdn"

class DapClient:
    def __init__(self, proc):
        self.proc = proc
        self.seq = 0
        self.responses = {}
        self.events = []
        self.lock = __import__("threading").Lock()
        import threading
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
                headers = {}
                continue
            m = re.match(r"content-length:\s*(\d+)$", s, re.I)
            if m:
                headers["content-length"] = int(m.group(1))
            # anything else = debuggee stdout leaking: skip

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
                    raise RuntimeError(f"{command} failed: {json.dumps(r.get('message', ''))[:200]}")
                return r
            time.sleep(0.05)
        raise TimeoutError(command)

def main():
    # fresh scenario
    subprocess.run(["cmd", "/c", "rmdir /s /q " + INSTALL], capture_output=True)
    subprocess.run([EXE, "-cli", "build",
                    r"-staged=D:\UEProject\IOStoreDemo\.manifesttest\chunkinstall\Windows\1.0.0\pakchunk1",
                    "-app=IOStoreDemo", "-version=1.0.0", "-out=" + CDN], capture_output=True, timeout=120)

    proc = subprocess.Popen([ADAPTER, "--liblldb", LIBLLDB],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    cli = DapClient(proc)
    cli.request("initialize", {"adapterID": "codelldb", "capabilities": {}}, timeout=60)
    with cli.lock:
        cli.seq += 1
        lseq = cli.seq
    launch = {"command": "launch", "type": "request", "seq": lseq,
              "arguments": {"program": EXE, "cwd": REPO, "stopOnEntry": True,
                            "natvis": NATVIS.replace("\\", "/"),
                            "args": ["-cli", "update", "-cdn=file:///" + CDN.replace("\\", "/"),
                                     "-install=" + INSTALL.replace("\\", "/")]}}
    body = json.dumps(launch).encode()
    proc.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    proc.stdin.flush()
    t0 = time.time()
    while time.time() - t0 < 90:
        with cli.lock:
            if any(e.get("event") == "stopped" for e in cli.events):
                break
        time.sleep(0.3)
    cli.request("setBreakpoints", {"source": {"name": "FUpdateOrchestrator.cpp", "path": SRC},
                                   "breakpoints": [{"line": 449}], "lines": [449]})
    cli.request("configurationDone")
    cli.request("continue", {"threadId": 0})
    t0 = time.time()
    tid = None
    while time.time() - t0 < 60:
        with cli.lock:
            stops = [e for e in cli.events if e.get("event") == "stopped"]
        if len(stops) >= 2:
            tid = stops[-1].get("body", {}).get("threadId")
            break
        time.sleep(0.3)
    if tid is None:
        print("FAIL: no breakpoint stop")
        return 1
    st = cli.request("stackTrace", {"threadId": tid})
    frame_id = st["body"]["stackFrames"][0]["id"]
    print("top frame:", st["body"]["stackFrames"][0].get("name", "")[:90])
    ok = False
    for expr in ("Version", "InstallDir", "CdnDir"):
        try:
            ev = cli.request("evaluate", {"expression": expr, "frameId": frame_id, "context": "watch"})
            val = ev.get("body", {}).get("result", "")
            print(f"eval {expr}:", val[:200])
            if "cdn" in val.lower() or "1.0.0" in val or "ugpdebug" in val.lower():
                ok = True
        except Exception as e:
            print(f"eval {expr} err:", str(e)[:100])
    try:
        cli.request("terminate")
    except Exception:
        pass
    time.sleep(1)
    try:
        proc.kill()
    except Exception:
        pass
    print("\n== NATVIS:", "PASS (FString content readable)" if ok else "INCONCLUSIVE/FAIL", "==")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
