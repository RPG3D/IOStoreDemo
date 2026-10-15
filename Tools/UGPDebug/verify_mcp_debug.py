#!/usr/bin/env python3
"""verify_mcp_debug.py - end-to-end verification of the UGP MCP debug lane.

Drives `debugmcp serve --stdio` (MCP, newline-delimited JSON-RPC) through a
real debug session of the UGP fresh-install scenario:

  1. add_breakpoint at FUpdateOrchestrator::Update and FPakWriter::Commit
  2. start_debugging (adapter: codelldb, registered as "cpp", user scope)
  3. on each stop: dump location + requested locals, continue
  4. poll until the debuggee exits
  5. assert exit code / installed files / state files (breakpoints.json)

Run:  python Tools/UGPDebug/verify_mcp_debug.py
Pre:  Tools/UGPDebug/prepare_scenario.bat  (fresh CDN + no install dir)
"""
import json
import os
import queue
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PRESET = json.load(open(os.path.join(REPO, "Tools", "UGPDebug", "breakpoints.json"), encoding="utf-8"))
DEBUGMCP_JS = r"C:\Users\imxql\AppData\Roaming\npm\node_modules\debugmcp\dist\debugmcp.js"
INSTALL = os.path.join(REPO, "Saved", "UGPDebug", "install")

PASS = True
def check(cond, label):
    global PASS
    print(("  [PASS] " if cond else "  [FAIL] ") + label)
    if not cond:
        PASS = False

class McpClient:
    def __init__(self, proc):
        self.proc = proc
        self.q = queue.Queue()
        self.next_id = 0
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for raw in self.proc.stdout:
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            try:
                self.q.put(json.loads(line))
            except json.JSONDecodeError:
                print("[non-json]", line[:200], file=sys.stderr)

    def request(self, method, params=None, timeout=60):
        self.next_id += 1
        rid = self.next_id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError(method)
            item = self.q.get(timeout=remaining)
            if item.get("id") == rid:
                if "error" in item:
                    raise RuntimeError(f"{method} error: {json.dumps(item['error'])[:300]}")
                return item.get("result")
            # notifications pass through (printed by caller if needed)

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()

def tool_text(result):
    """MCP tools/call result -> concatenated text content."""
    out = []
    for c in result.get("content", []):
        if c.get("type") == "text":            out.append(c.get("text", ""))
    return "\n".join(out)

def main():
    proc = subprocess.Popen(
        ["node", DEBUGMCP_JS, "serve", "--stdio"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    cli = McpClient(proc)
    try:
        init = cli.request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "ugp-debug-verify", "version": "0.1"},
        }, timeout=30)
        print("server:", init.get("serverInfo"))
        cli.notify("notifications/initialized")

        tools = {t["name"] for t in cli.request("tools/list", {}, timeout=30).get("tools", [])}
        for need in ["start_debugging", "add_breakpoint", "continue_execution",
                     "get_debug_status", "get_variables_values", "evaluate_expression"]:
            check(need in tools, f"tool available: {need}")

        # 1. breakpoints (absolute paths; source roots in the preset are repo-relative)
        for bp in PRESET["breakpoints"]:
            absf = os.path.join(REPO, bp["file"].replace("/", os.sep))
            r = cli.request("tools/call", {
                "name": "add_breakpoint",
                "arguments": {"fileFullPath": absf, "line": bp["line"]},
            }, timeout=30)
            print(f"  bp {bp['label']}: {tool_text(r)[:200].replace(chr(10), ' | ')}")

        # 2. start the session (fresh-install scenario; program/args live in the
        #    adapter's registered launch properties, user scope .debugmcp).
        #    stopOnEntry=true => start_debugging returns paused at entry, which
        #    also gives the PDB symbol loader time before breakpoints apply.
        src = os.path.join(REPO, PRESET["breakpoints"][0]["file"].replace("/", os.sep))
        r = cli.request("tools/call", {
            "name": "start_debugging",
            "arguments": {"fileFullPath": src, "workingDirectory": REPO,
                          "configurationName": "cpp"},
        }, timeout=300)
        print("  start_debugging:", tool_text(r)[:400].replace("\n", " | "))

        # 3. stop loop. Hit detection uses the status JSON's frame info:
        #    state.frameName / stackTrace[0] name our function at the stop.
        #    (the "breakpoints" list in the status is the SET list, not the
        #     hit location - never use it for hit detection.)
        seen_bp = set()
        markers = {b["line"]: b["marker"] for b in PRESET["breakpoints"]}
        expect_vars = {b["line"]: b.get("expectVars", []) for b in PRESET["breakpoints"]}
        first_user_stop_done = [False]
        deadline = time.time() + 480
        exited = False
        while time.time() < deadline:
            st = cli.request("tools/call", {
                "name": "get_debug_status",
                "arguments": {"waitForPauseSeconds": 30},
            }, timeout=90)
            txt = tool_text(st)
            low = txt.lower()
            if any(k in low for k in ("exited", "terminated", "no active", "not running",
                                      "completed", "no-session", "no session")):
                exited = True
                print("  session:", txt[:200].replace("\n", " | "))
                break
            if '"paused": true' not in low and '"paused":true' not in low:
                cli.request("tools/call", {"name": "continue_execution", "arguments": {}}, timeout=300)
                continue
            # paused: parse the JSON payload
            try:
                payload = json.loads(txt)
            except json.JSONDecodeError:
                payload = {}
            state = payload.get("state", {})
            frame_name = state.get("frameName") or ""
            trace = state.get("stackTrace") or []
            print(f"  [STOP] frame={frame_name[:90]} trace0={str(trace[0])[:90] if trace else '-'}")
            hit = next((l for l, m in markers.items() if m in frame_name), None)
            if hit:
                seen_bp.add(hit)
                names = expect_vars.get(hit) or []
                if names:
                    try:
                        vr = cli.request("tools/call", {
                            "name": "get_variables_values",
                            "arguments": {"variableNames": names, "scope": "local"},
                        }, timeout=30)
                        print("    vars:", tool_text(vr)[:400].replace("\n", " | "))
                    except Exception as e:
                        print("    vars: unavailable:", str(e)[:150])
                if not first_user_stop_done[0]:
                    first_user_stop_done[0] = True
                    for expr in ["?Version", "?Version.Num()"]:
                        try:
                            ev = cli.request("tools/call", {
                                "name": "evaluate_expression",
                                "arguments": {"expression": expr},
                            }, timeout=30)
                            print(f"    eval {expr} -> {tool_text(ev)[:200]!r}".replace("\n", " | "))
                        except Exception as e:
                            print(f"    eval {expr} failed:", str(e)[:120])
            cli.request("tools/call", {"name": "continue_execution", "arguments": {}}, timeout=300)

        check(exited, "debuggee ran to completion")
        for b in PRESET["breakpoints"]:
            check(b["line"] in seen_bp, f"breakpoint hit: {b['label']}")

        # 4. on-disk assertions
        check(os.path.isdir(INSTALL), f"install dir created: {INSTALL}")
        for f in PRESET["assert"]["installedFiles"]:
            check(os.path.isfile(os.path.join(INSTALL, f)), f"installed: {f}")
        for f in PRESET["assert"]["stateFiles"]:
            check(os.path.exists(os.path.join(INSTALL, f)), f"state: {f}")
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.terminate()

    print("\n== VERIFY:", "PASS" if PASS else "FAIL", "==")
    sys.exit(0 if PASS else 1)

if __name__ == "__main__":
    main()
