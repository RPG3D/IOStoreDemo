#!/usr/bin/env python3
r"""dbg_scenario.py - scenario-driven debugger verification of UGP core paths.

Drives CodingWithCalvin/VS-MCPServer (http://localhost:5050) through a
debug session per scenario: set breakpoints, launch the CLI command, and at
each stop capture locals / callstack / evaluated expressions as evidence for
the dynamic review.

Usage:
  python Tools/VSDebug/dbg_scenario.py <scenario>          # run one
  python Tools/VSDebug/dbg_scenario.py --list
Scenarios cover the 15 test suites' core code paths (see
Docs/review-dbg-2026-10-10.md for the mapping).
"""
import json
import os
import subprocess
import sys
import time
import urllib.request
import urllib.error

BASE = "http://localhost:5050"
REPO = r"D:\UEProject\IOStoreDemo"
SRC = REPO + r"\Source\Programs\UnrealGamePatcher"
CORE = SRC + r"\UnrealGamePatcherCore\Private"
EXE = REPO + r"\Binaries\Win64\UnrealGamePatcher.exe"
CDN = REPO + r"\Saved\UGPDebug\cdn"
INSTALL = REPO + r"\Saved\UGPDebug\install"
OUTDIR = REPO + r"\Saved\UGPDebug\dbg"
SLN = REPO + r"\Tools\VSDebug\UGPDebugOnly.sln"

SCENARIOS = {
    # covers: buildtest / difftest / diffbuild / cdcshift
    "s1_ingest": {
        "prepare": [f'"{EXE}" -cli build -staged="{REPO}\\.manifesttest\\chunkinstall\\Windows\\1.0.0\\pakchunk1" -app=IOStoreDemo -version=1.0.0 -out="{CDN}"'],
        "breakpoints": [(SRC + r"\UnrealGamePatcherBuild\Private\FBuildIngestor.cpp", 50)],
        "launch_args": ["-cli", "build", f"-staged={REPO}\\.manifesttest\\chunkinstall\\Windows\\1.0.0\\pakchunk1",
                        "-app=IOStoreDemo", "-version=1.0.1", f"-out={CDN}"],
    },
    # covers: updatetest / e2etest / perftest baseline
    "s2_fresh_install": {
        "prepare": [f'rmdir /s /q "{INSTALL}"',
                    f'"{EXE}" -cli build -staged="{REPO}\\.manifesttest\\chunkinstall\\Windows\\1.0.0\\pakchunk1" -app=IOStoreDemo -version=1.0.0 -out="{CDN}"'],
        "breakpoints": [(CORE + r"\Update\FUpdateOrchestrator.cpp", 449),
                        (CORE + r"\Update\FUpdateOrchestrator.cpp", 256)],
        "launch_args": ["-cli", "update", f"-cdn=file:///{CDN.replace(os.sep, '/')}",
                        f"-install={INSTALL}"],
    },
    # covers: pakinplace / iostoreinplace (CoW + hole backfill, H1 fix point).
    # The pakinplace suite drives the real pipeline in-process (fixture v1 ->
    # v2 CoW), so breakpoints in Patch() fire on genuine calls.
    "s3_cow_patch": {
        "prepare": [f'"{EXE}" -cli build -staged="{REPO}\\.manifesttest\\chunkinstall\\Windows\\1.0.0\\pakchunk1" -app=IOStoreDemo -version=1.0.0 -out="{CDN}"'],
        "breakpoints": [(CORE + r"\Update\FInPlacePakPatcher.cpp", 345),
                        (CORE + r"\Update\FInPlacePakPatcher.cpp", 359)],
        "launch_args": ["-cli", "pakinplace"],
    },
    # covers: statetest / transaction rollback (injected write failure).
    # Fresh install + -injectfail=2: the write failure fires inside the
    # transaction, exercising the rollback path end to end.
    "s4_rollback": {
        "prepare": [f'rmdir /s /q "{INSTALL}"',
                    f'"{EXE}" -cli build -staged="{REPO}\\.manifesttest\\chunkinstall\\Windows\\1.0.0\\pakchunk1" -app=IOStoreDemo -version=1.0.0 -out="{CDN}"'],
        "breakpoints": [(CORE + r"\Update\FUpdateOrchestrator.cpp", 1547)],
        "launch_args": ["-cli", "update", f"-cdn=file:///{CDN.replace(os.sep, '/')}",
                        f"-install={INSTALL}", "-injectfail=2"],
    },
    # covers: downloadtest / benchtest (HTTP transport layer). A local
    # cdn_server.py serves the CDN root over HTTP; FDownloadManager::DownloadAll
    # is the single choke point every HTTP-driven update passes through.
    "s6_http_download": {
        "prepare": [f'rmdir /s /q "{INSTALL}"',
                    f'"{EXE}" -cli build -staged="{REPO}\\.manifesttest\\chunkinstall\\Windows\\1.0.0\\pakchunk1" -app=IOStoreDemo -version=1.0.0 -out="{CDN}"'],
        "serve": [sys.executable, REPO + r"\Source\Programs\UnrealGamePatcher\Scripts\cdn_server.py",
                  "--root", CDN, "--port", "18099"],
        "breakpoints": [(CORE + r"\Download\FDownloadManager.cpp", 65)],
        "launch_args": ["-cli", "update", "-cdn=http://127.0.0.1:18099",
                        f"-install={INSTALL}"],
    },
}

class Client:
    def __init__(self, base=BASE):
        self.base = base
        self.next_id = 0
        self.session = None

    def rpc(self, method, params=None, timeout=180, notify=False):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if not notify:
            msg["id"] = self.next_id
        body = json.dumps(msg).encode()
        req = urllib.request.Request(self.base + "/", data=body, method="POST",
                                     headers={"Content-Type": "application/json",
                                              "Accept": "application/json, text/event-stream",
                                              **({"Mcp-Session-Id": self.session} if self.session else {})})
        try:
            resp = urllib.request.urlopen(req, timeout=timeout)
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:200]}")
        sid = resp.headers.get("Mcp-Session-Id")
        if sid:
            self.session = sid
        ctype = resp.headers.get("Content-Type", "")
        raw = resp.read().decode("utf-8", "replace")
        if notify or not raw.strip():
            return {}
        if "text/event-stream" in ctype:
            result = None
            for line in raw.splitlines():
                if line.startswith("data:"):
                    payload = line[5:].strip()
                    if payload and payload != "[DONE]":
                        m = json.loads(payload)
                        if not notify and m.get("id") == self.next_id:
                            result = m
            if result is None:
                raise RuntimeError(f"SSE no reply: {raw[:200]}")
            msg = result
        else:
            msg = json.loads(raw)
        if "error" in msg:
            raise RuntimeError(f"rpc error: {json.dumps(msg['error'])[:200]}")
        return msg["result"]

def tool_text(result):
    return "\n".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")

def call(cli, name, args=None, timeout=180):
    r = cli.rpc("tools/call", {"name": name, "arguments": args or {}}, timeout)
    return tool_text(r), r.get("isError")

def wait_break(cli, timeout_s=120):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        time.sleep(1.5)
        txt, _ = call(cli, "debugger_status", {}, timeout=60)
        if '"Mode": "Break"' in txt or '"Mode":"Break"' in txt:
            return True, txt
        if '"Mode": "Design"' in txt:
            return False, txt
    return False, "timeout"

def run_scenario(name):
    cfg = SCENARIOS[name]
    os.makedirs(OUTDIR, exist_ok=True)
    out = {"scenario": name, "stops": [], "notes": []}
    print(f"=== {name} ===", flush=True)

    for cmd in cfg["prepare"]:
        p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=300)
        rc = p.returncode
        print(f"  prepare rc={rc}: {cmd[:80]}...", flush=True)
        if rc != 0:
            out["notes"].append(f"prepare failed rc={rc}: {p.stdout[-200:]} {p.stderr[-200:]}")

    server = None
    if "serve" in cfg:
        server = subprocess.Popen(cfg["serve"], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, encoding="utf-8")
        t0 = time.time()
        ready = False
        while time.time() - t0 < 20:
            line = server.stdout.readline() if server.poll() is None else ""
            if "READY" in (line or ""):
                ready = True
                break
            time.sleep(0.2)
        print(f"  cdn_server ready={ready} pid={server.pid}", flush=True)
        out["notes"].append(f"cdn_server pid={server.pid} ready={ready}")

    cli = Client()
    cli.rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                           "clientInfo": {"name": "dbg-scenario", "version": "0.1"}})
    cli.rpc("notifications/initialized", {}, notify=True)

    # (solution_open on an already-open same-path sln errors - documented)

    for f, line in cfg["breakpoints"]:
        if line is None:
            continue
        txt, err = call(cli, "debugger_add_breakpoint", {"path": f, "line": line})
        print(f"  bp {os.path.basename(f)}:{line} -> {txt[:80].replace(chr(10),' | ')}", flush=True)
        out["notes"].append(f"bp {f}:{line}: {'ERR ' + txt[:100] if err else txt[:100]}")

    args = cfg["launch_args"]
    txt, err = call(cli, "debugger_launch", {}, timeout=300)
    print(f"  launch: {txt[:120].replace(chr(10),' | ')}", flush=True)

    stop_n = 0
    deadline = time.time() + 600
    while time.time() < deadline:
        broke, status = wait_break(cli, timeout_s=30)
        if not broke:
            out["notes"].append(f"session end/status: {status[:150]}")
            print(f"  session: {status[:120]}", flush=True)
            break
        stop_n += 1
        lh = None
        st, _ = call(cli, "debugger_status", {}, timeout=60)
        out["stops"].append({"n": stop_n, "status": st[:400]})
        print(f"  [STOP {stop_n}] {st[:220].replace(chr(10),' | ')}", flush=True)
        # capture locals at each stop (evidence)
        loc, _ = call(cli, "debugger_get_locals", {}, timeout=60)
        out["stops"][-1]["locals"] = loc[:2000]
        try:
            json_loc = json.loads(loc)
            interesting = [v for v in json_loc if isinstance(v, dict) and v.get("IsValidValue")]
            print(f"    locals: {', '.join((v.get('Name','?') + '=' + str(v.get('Value',''))[:40]) for v in interesting[:10])}", flush=True)
        except Exception:
            print(f"    locals(raw): {loc[:300]}", flush=True)
        # continue
        call(cli, "debugger_continue", {}, timeout=300)
        time.sleep(2)

    try:
        call(cli, "debugger_stop", {}, timeout=60)
    except Exception:
        pass
    if server is not None:
        server.terminate()
    outfn = os.path.join(OUTDIR, f"{name}.json")
    json.dump(out, open(outfn, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"  saved -> {outfn}", flush=True)

def main():
    if "--list" in sys.argv:
        print("\n".join(SCENARIOS))
        return
    for name in sys.argv[1:]:
        if name not in SCENARIOS:
            print(f"unknown scenario {name}")
            continue
        run_scenario(name)

if __name__ == "__main__":
    main()
