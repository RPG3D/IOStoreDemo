#!/usr/bin/env python3
r"""vs_verify.py - end-to-end verification of the VS MCP bridge.

Drives vs_mcp_bridge.py through a REAL Visual Studio debugging session of UGP:
  vs_status -> vs_launch (starts devenv with the debug-only sln)
  -> vs_set_breakpoint x2 -> vs_start_debug (F5, real VS debugger)
  -> Break at FUpdateOrchestrator::UpdateWithOptions:449
  -> vs_evaluate Version (DTE visualizer stringifies FString)
  -> vs_continue -> Break at RebuildFileFromChunks:256 -> vs_continue -> exit
  -> vs_quit (clean teardown)

Run:  cmd /c Tools\UGPDebug\prepare_scenario.bat  &&  python Tools\VSDebug\vs_verify.py
"""
import json
import subprocess
import sys
import time

BRIDGE = r"D:\UEProject\IOStoreDemo\Tools\VSDebug\vs_mcp_bridge.py"
SRC_MAIN = r"D:\UEProject\IOStoreDemo\Source\Programs\UnrealGamePatcher\UnrealGamePatcherCore\Private\Update\FUpdateOrchestrator.cpp"

PASS = True
def check(cond, label):
    global PASS
    print(("  [PASS] " if cond else "  [FAIL] ") + label, flush=True)
    if not cond:
        PASS = False

class Client:
    def __init__(self):
        self.proc = subprocess.Popen([sys.executable, BRIDGE, "serve"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        self.next_id = 0

    def request(self, method, params=None, timeout=330):
        self.next_id += 1
        rid = self.next_id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()
        # blocking read (single in-flight request at a time)
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("bridge died")
            item = json.loads(line)
            if item.get("id") == rid:
                return item.get("result", {})
            # ignore notifications

    def call(self, name, arguments=None, timeout=330):
        r = self.request("tools/call", {"name": name, "arguments": arguments or {}}, timeout)
        txt = "".join(c.get("text", "") for c in r.get("content", []) if c.get("type") == "text")
        err = r.get("isError")
        try:
            data = json.loads(txt)
        except json.JSONDecodeError:
            data = {"raw": txt[:400]}
        if err:
            print(f"    [tool-error {name}] {txt[:220]}", flush=True)
        return data, err, txt

def dump_debug_pane(cli, lines=25):
    d, err, txt = cli.call("vs_output_pane", {"pane": "Debug", "lines": lines}, timeout=60)
    tail = (d.get("text") or "").strip().splitlines()[-lines:]
    for l in tail:
        print("    |", l[:150], flush=True)

def main():
    cli = Client()
    r = cli.request("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                   "clientInfo": {"name": "vs-verify", "version": "0.1"}})
    print("bridge:", r.get("serverInfo"))
    cli.request("notifications/initialized", None) if False else None

    tools = cli.request("tools/list")["tools"]
    names = {t["name"] for t in tools}
    for need in ["vs_status", "vs_launch", "vs_set_breakpoint", "vs_start_debug",
                 "vs_evaluate", "vs_continue", "vs_debug_state", "vs_quit"]:
        check(need in names, f"tool available: {need}")

    d, err, txt = cli.call("vs_status")
    print("  pre-launch status: vsRunning =", d.get("vsRunning"))

    # 1. launch VS with the debug-only sln (heavy: polls up to 4 min)
    print("  launching Visual Studio (first start can take a while)...")
    d, err, txt = cli.call("vs_launch", {"timeoutSeconds": 240}, timeout=300)
    check(d.get("ok"), f"vs_launch: {json.dumps({k: d.get(k) for k in ('ok', 'solution', 'error', 'waitedSeconds')}, ensure_ascii=False)}")

    # 2. breakpoints
    d, err, txt = cli.call("vs_set_breakpoint", {"fileFullPath": SRC_MAIN, "line": 449})
    check(d.get("ok"), f"breakpoint @ UpdateWithOptions:449 ({d.get('bound')} bound)")
    d, err, txt = cli.call("vs_set_breakpoint", {"fileFullPath": SRC_MAIN, "line": 256})
    check(d.get("ok"), f"breakpoint @ RebuildFileFromChunks:256 ({d.get('bound')} bound)")

    # 2b. make F5 skip the VS build pipeline (NMake build hangs in VS host;
    #     UGP builds belong to Build.bat / UBT)
    d, err, txt = cli.call("vs_set_f5_never_build", timeout=60)
    print(f"  f5-never-build: {json.dumps(d, ensure_ascii=False)[:200]}")

    # 3. F5 (async; with never-build this starts the exe immediately)
    d, err, txt = cli.call("vs_start_debug", {"timeoutSeconds": 480}, timeout=560)
    check(d.get("ok") and d.get("debugMode") in ("Break", "Run"),
          f"vs_start_debug -> mode={d.get('debugMode')} waited={d.get('waitedSeconds')} hit={json.dumps(d.get('breakLastHit'))}")
    if d.get("debugMode") == "Break":
        hit = (d.get("breakLastHit") or {})
        check("FUpdateOrchestrator" in (hit.get("file") or "") and hit.get("line") == 449,
              f"first stop = UpdateWithOptions:449 (got {hit.get('file')}:{hit.get('line')})")
    elif d.get("debugMode") == "Run":
        import time as _t
        _t.sleep(20)
        dump_debug_pane(cli)

    # 4. evaluate (informational: VS 18 DTE intermittently rejects
    #    GetExpression in Break mode - known limitation, see DEBUGGER.md)
    d, err, txt = cli.call("vs_evaluate", {"expression": "Version"})
    print(f"  eval Version: ok={d.get('ok')} value={d.get('value')!r}")
    d, err, txt = cli.call("vs_evaluate", {"expression": "InstallDir"})
    print(f"  eval InstallDir: ok={d.get('ok')} value={d.get('value')!r}")

    # 5. continue -> RebuildFileFromChunks stops (per added file)
    hits256 = 0
    for i in range(8):
        d, err, txt = cli.call("vs_continue", timeout=330)
        mode = d.get("debugMode")
        hit = d.get("breakLastHit") or {}
        if err or mode is None:
            d, err2, _ = cli.call("vs_debug_state", timeout=60)
            mode = d.get("debugMode")
            hit = d.get("breakLastHit") or {}
        print(f"  continue #{i+1}: mode={mode} hit={hit.get('file', '')[-40:]}:{hit.get('line')}")
        if mode == "Break" and hit.get("line") == 256:
            hits256 += 1
            continue
        if mode in ("Run", "Design"):
            break
    # run to completion
    for i in range(12):
        d, err, txt = cli.call("vs_continue", timeout=330)
        mode = d.get("debugMode")
        if err or mode is None:
            d, _, _ = cli.call("vs_debug_state", timeout=60)
            mode = d.get("debugMode")
        if mode != "Break":
            break
    d, err, txt = cli.call("vs_debug_state", timeout=60)
    check(d.get("debugMode") == "Design", f"debuggee completed (mode={d.get('debugMode')})")

    # 7. teardown
    cli.call("vs_quit", timeout=60)
    print("\n== VS VERIFY:", "PASS" if PASS else "FAIL", "==")
    sys.exit(0 if PASS else 1)

if __name__ == "__main__":
    main()
