#!/usr/bin/env python3
r"""cw_verify.py - end-to-end verification of CodingWithCalvin/VS-MCPServer
over streamable HTTP (localhost:5050) against the UGP debug scenario:

  tools/list -> solution_open -> breakpoint_add -> debug_start
  -> stop at breakpoint -> callstack -> locals -> evaluate -> continue
  -> session ends -> installed files on disk

Run:  cmd /c Tools\UGPDebug\prepare_scenario.bat && python Tools\VSDebug\cw_verify.py
"""
import json
import os
import sys
import time
import urllib.request
import urllib.error

BASE = "http://localhost:5050"
REPO = r"D:\UEProject\IOStoreDemo"
SRC = REPO + r"\Source\Programs\UnrealGamePatcher\UnrealGamePatcherCore\Private\Update\FUpdateOrchestrator.cpp"
INSTALL = REPO + r"\Saved\UGPDebug\install"
SLN = REPO + r"\Tools\VSDebug\UGPDebugOnly.sln"

PASS = True
def check(cond, label):
    global PASS
    print(("  [PASS] " if cond else "  [FAIL] ") + label, flush=True)
    if not cond:
        PASS = False

class Client:
    def __init__(self, base):
        self.base = base
        self.next_id = 0
        self.session = None

    def rpc(self, method, params=None, timeout=120, notify=False):
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
            body = e.read().decode("utf-8", "replace")
            raise RuntimeError(f"HTTP {e.code}: {body[:200]}")
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
                raise RuntimeError(f"SSE stream had no matching reply: {raw[:200]}")
            msg = result
        else:
            msg = json.loads(raw)
        if "error" in msg:
            raise RuntimeError(f"rpc error: {json.dumps(msg['error'])[:250]}")
        return msg["result"]

def tool_text(result):
    out = []
    for c in result.get("content", []):
        if c.get("type") == "text":
            out.append(c.get("text", ""))
    return "\n".join(out)

def call(cli, name, args=None, timeout=120):
    r = cli.rpc("tools/call", {"name": name, "arguments": args or {}}, timeout)
    txt = tool_text(r)
    err = r.get("isError")
    if err:
        print(f"    [tool-error {name}] {txt[:200]}", flush=True)
    return txt, err

def main():
    cli = Client(BASE)
    init = cli.rpc("initialize", {
        "protocolVersion": "2024-11-05", "capabilities": {},
        "clientInfo": {"name": "ugp-cw-verify", "version": "0.1"}}, timeout=30)
    print("server:", init.get("serverInfo"))
    cli.rpc("notifications/initialized", {}, notify=True)

    tools = cli.rpc("tools/list", {}, timeout=30)
    tool_names = [t["name"] for t in tools.get("tools", [])]
    print(f"tools: {len(tool_names)}")
    for need in ["debugger_add_breakpoint", "debugger_launch", "debugger_continue",
                 "debugger_step_into", "debugger_step_over", "debugger_step_out",
                 "debugger_get_callstack", "debugger_get_locals", "debugger_evaluate",
                 "debugger_status", "debugger_stop", "solution_open"]:
        check(need in tool_names, f"tool available: {need}")

    # 1. open the UGP debug-only solution
    txt, err = call(cli, "solution_open", {"solutionPath": SLN}, timeout=240)
    print("  solution_open:", txt[:150].replace("\n", " | "))

    # 2. breakpoint at UpdateWithOptions body entry
    txt, err = call(cli, "debugger_add_breakpoint",
                    {"filePath": SRC, "line": 449}, timeout=60)
    print("  bp:", txt[:150].replace("\n", " | "))

    # 3. launch (F5)
    txt, err = call(cli, "debugger_launch", {}, timeout=300)
    print("  launch:", txt[:200].replace("\n", " | "))
    time.sleep(3)

    # 4. status + callstack + locals + evaluate at the stop
    txt, err = call(cli, "debugger_status", {}, timeout=60)
    print("  status:", txt[:200].replace("\n", " | "))
    check("Break" in txt or "break" in txt, "debugger in Break mode at breakpoint")

    txt, err = call(cli, "debugger_get_callstack", {}, timeout=60)
    print("  callstack:", txt[:300].replace("\n", " | "))
    check("FUpdateOrchestrator" in txt or "UpdateWithOptions" in txt,
          "callstack shows UpdateWithOptions")

    txt, err = call(cli, "debugger_get_locals", {}, timeout=60)
    print("  locals:", txt[:300].replace("\n", " | "))
    check("CdnDir" in txt or "Version" in txt or "InstallDir" in txt,
          "locals show CdnDir/Version/InstallDir")

    for expr in ("Version", "InstallDir"):
        txt, err = call(cli, "debugger_evaluate", {"expression": expr}, timeout=60)
        print(f"  eval {expr}:", txt[:200].replace("\n", " | "))

    # 5. step over once, then continue through ALL stops (the CLI calls
    # UpdateWithOptions twice: check-only + apply) until Break mode ends
    txt, err = call(cli, "debugger_step_over", {}, timeout=120)
    print("  step_over:", txt[:150].replace("\n", " | "))
    stops = 0
    mode_break = True
    for i in range(10):
        txt, err = call(cli, "debugger_continue", {}, timeout=300)
        stops += 1
        time.sleep(2)
        txt, err = call(cli, "debugger_status", {}, timeout=60)
        mode_break = '"Mode": "Break"' in txt or '"Mode":"Break"' in txt
        print(f"  continue #{i+1}: stillBreak={mode_break} status={txt[:70]}", flush=True)
        if not mode_break:
            break
    check(stops >= 1, "continue advanced through stops")
    check(not mode_break, "debugger left Break mode (session ran on)")

    # 6. stop + on-disk assertions
    txt, err = call(cli, "debugger_stop", {}, timeout=60)
    check(os.path.isdir(INSTALL), f"install dir created: {INSTALL}")
    for f in ("pakchunk1-Windows.pak", "pakchunk1-Windows.ucas", "pakchunk1-Windows.utoc"):
        check(os.path.isfile(os.path.join(INSTALL, f)), f"installed: {f}")

    print("\n== CW VERIFY:", "PASS" if PASS else "FAIL", "==")
    sys.exit(0 if PASS else 1)

if __name__ == "__main__":
    main()
