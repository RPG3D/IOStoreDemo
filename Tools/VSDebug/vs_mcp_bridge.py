#!/usr/bin/env python3
"""vs_mcp_bridge.py - MCP stdio server that lets an AI agent fully drive
Visual Studio 2026 (DTE/COM automation): open solution, build, set
breakpoints, F5 into the REAL VS debugger, step, read output pane, execute
any VS command. Expression evaluation is unreliable on VS 18 (see
DEBUGGER.md) - use the debugmcp/codelldb lane for evaluate.

Requires: pywin32; VS running (vs_launch starts it). ProgID:
VisualStudio.DTE.18.0

Usage: python vs_mcp_bridge.py serve
       (registered in .zcode/config.json as MCP server "vs")

VS 2026 DTE hard facts (all verified via GetIDsOfNames / experiment):
  - Debugger has NO Continue member (Go=6, Break=7, Stop=8,
    SetNextStatement=9) and "Debug.Continue" command is invalid:
    Go(False) in Break mode IS continue (F5 semantics).
  - Breakpoint.CurrentHits stays 0: never use it for stop detection.
  - GetExpression intermittently fails with RPC_E_CALL_REJECTED and
    waiting does not help: evaluate via the codelldb lane instead.
  - Every call can bounce off a busy VS ("Call was rejected by callee"):
    ALL COM access goes through _retry() via ComProxy.
  - The .suo remembers a stale ActiveConfiguration: vs_start_debug
    activates the requested solution configuration before Go.
  - Modal dialogs block ALL COM calls: dismiss_modal_dialogs() auto-OKs
    devenv's visible #32770 dialogs (only those that have an OK button).
"""
import json
import os
import subprocess
import sys
import time

import pythoncom

VS_ROOT = r"C:\Program Files\Microsoft Visual Studio\18\Community"
DEVENV = os.path.join(VS_ROOT, "Common7", "IDE", "devenv.exe")
DEFAULT_SLN = r"D:\UEProject\IOStoreDemo\Tools\VSDebug\UGPDebugOnly.sln"
DTE_PROGID = "VisualStudio.DTE.18.0"

MODES = {1: "Design", 2: "Break", 3: "Run"}

RPC_E_CALL_REJECTED = -2147418111
RPC_E_SERVERCALL_RETRYLATER = -2147417846

# ---------------------------------------------------------------------------
# COM plumbing: STA init + retry wrapper

def _com_init():
    import pythoncom
    pythoncom.CoInitialize()  # STA

def _retry(fn, max_wait_s=30.0):
    """Retry a COM call while VS rejects it (busy: breakpoint binding,
    symbol load, stepping). pywin32 has no CoRegisterMessageFilter, so this
    explicit retry loop is the rejected-call message filter."""
    t0 = time.time()
    while True:
        try:
            return fn()
        except pythoncom.com_error as e:
            hr = e.hresult if hasattr(e, "hresult") else (e.args[0] if e.args else 0)
            if hr in (RPC_E_CALL_REJECTED, RPC_E_SERVERCALL_RETRYLATER) and time.time() - t0 < max_wait_s:
                time.sleep(0.1)
                continue
            raise

class ComProxy:
    """Transparent COM wrapper: attribute reads, method calls and collection
    iteration all go through _retry."""
    __slots__ = ("_obj",)

    def __init__(self, obj):
        object.__setattr__(self, "_obj", obj)

    def __getattr__(self, name):
        obj = object.__getattribute__(self, "_obj")
        target = _retry(lambda: getattr(obj, name))
        # COM objects first: CDispatch is itself callable (property gets that
        # return collections must proxy, not wrap as plain functions)
        if hasattr(target, "_oleobj_"):
            return ComProxy(target)
        if callable(target):
            def _call(*a, **kw):
                return _retry(lambda: target(*a, **kw))
            return _call
        return target

    def __call__(self, *a, **kw):
        obj = object.__getattribute__(self, "_obj")
        return _retry(lambda: obj(*a, **kw))

    def __iter__(self):
        obj = object.__getattribute__(self, "_obj")
        it = iter(_retry(lambda: obj))
        while True:
            try:
                item = _retry(lambda nxt=it: next(nxt))
            except StopIteration:
                return
            if hasattr(item, "_oleobj_"):
                yield ComProxy(item)
            else:
                yield item

class Dte:
    def __init__(self):
        import win32com.client
        _com_init()
        self.dte = win32com.client.GetActiveObject(DTE_PROGID)

    def __getattr__(self, name):
        return ComProxy(_retry(lambda: getattr(self.dte, name)))

def dte_or_err():
    try:
        return Dte(), None
    except Exception as e:
        return None, (f"Visual Studio not reachable via COM ({DTE_PROGID}): {e}. "
                      f"Run vs_launch first (or start VS).")

def dismiss_modal_dialogs():
    """Auto-click OK on devenv modal dialogs (browse/IntelliSense database
    prompts etc.). Never touches a dialog without an OK button (those need a
    real decision). Returns count dismissed."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    BM_CLICK = 0x00F5
    import subprocess as _sp
    try:
        out = _sp.run(["powershell", "-NoProfile", "-Command",
                       "(Get-Process devenv -ErrorAction SilentlyContinue).Id"],
                      capture_output=True, text=True, timeout=15).stdout
        pids = {int(x) for x in out.split() if x.strip().isdigit()}
    except Exception:
        return 0
    if not pids:
        return 0
    hits = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value not in pids:
            return True
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(h, cls, 64)
        title = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, title, 256)
        if cls.value == "#32770" and user32.IsWindowVisible(h):
            hits.append(h)
        return True
    user32.EnumWindows(cb, 0)
    count = 0
    for h in hits:
        btns = []
        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def child(ch, l):
            c2 = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(ch, c2, 64)
            t2 = ctypes.create_unicode_buffer(128)
            user32.GetWindowTextW(ch, t2, 128)
            if c2.value == "Button":
                btns.append((ch, t2.value))
            return True
        user32.EnumChildWindows(h, child, 0)
        ok = next((b for b, t in btns if t.lower().replace("&", "") in ("ok", "yes", "确定", "是")), None)
        if ok is None:
            continue
        check = next((b for b, t in btns if "do not prompt" in t.lower()), None)
        if check:
            user32.SendMessageW(check, BM_CLICK, 0, 0)
        user32.SendMessageW(ok, BM_CLICK, 0, 0)
        count += 1
    return count

# ---------------------------------------------------------------------------
# DTE helpers

def _mode(d):
    try:
        return MODES.get(d.Debugger.CurrentMode, str(d.Debugger.CurrentMode))
    except Exception:
        return "Unknown"

def _solution(d):
    try:
        if d.Solution and d.Solution.IsOpen:
            return d.Solution.FullName
    except Exception:
        pass
    return None

def _break_last_hit(d):
    try:
        bp = d.Debugger.BreakpointLastHit
        if bp:
            return {"file": bp.File, "line": bp.FileLine, "function": bp.FunctionName}
    except Exception:
        pass
    return None

def _current_frame(d):
    try:
        sf = d.Debugger.CurrentStackFrame
        if sf:
            return {"function": sf.FunctionName, "file": sf.FileName, "line": sf.Line}
    except Exception:
        pass
    return None

# ---------------------------------------------------------------------------
# Tool implementations (args dict -> result dict)

def t_status(args):
    d, err = dte_or_err()
    if err:
        return {"vsRunning": False, "error": err}
    procs = []
    try:
        for p in d.Debugger.DebuggedProcesses:
            procs.append({"pid": p.ProcessID, "name": p.Name})
    except Exception:
        pass
    return {"vsRunning": True, "edition": d.Edition, "version": d.Version,
            "solution": _solution(d), "debugMode": _mode(d),
            "breakLastHit": _break_last_hit(d), "debuggedProcesses": procs}

def t_launch(args):
    sln = args.get("solutionPath") or DEFAULT_SLN
    timeout = int(args.get("timeoutSeconds", 240))
    t0 = time.time()
    # Phase 1: devenv already running? wait for it to expose the right
    # solution (do NOT spawn a second instance while the first starts up).
    import subprocess as _sp
    try:
        out = _sp.run(["powershell", "-NoProfile", "-Command",
                       "(Get-Process devenv -ErrorAction SilentlyContinue).Id"],
                      capture_output=True, text=True, timeout=15).stdout
        existing = {int(x) for x in out.split() if x.strip().isdigit()}
    except Exception:
        existing = set()
    while existing and time.time() - t0 < timeout:
        dismiss_modal_dialogs()
        d, err = dte_or_err()
        if not err:
            cur = _solution(d)
            if cur and os.path.normcase(cur) == os.path.normcase(sln):
                return {"ok": True, "alreadyRunning": True, "solution": cur,
                        "waitedSeconds": round(time.time() - t0, 1),
                        "debugMode": _mode(d)}
        time.sleep(3)
    if not os.path.isfile(DEVENV):
        return {"ok": False, "error": f"devenv.exe not found: {DEVENV}"}
    subprocess.Popen([DEVENV, sln])
    while time.time() - t0 < timeout:
        dismiss_modal_dialogs()
        d, err = dte_or_err()
        if not err:
            cur = _solution(d)
            if cur and os.path.normcase(cur) == os.path.normcase(sln):
                return {"ok": True, "started": True, "solution": cur,
                        "waitedSeconds": round(time.time() - t0, 1),
                        "debugMode": _mode(d)}
        time.sleep(3)
    return {"ok": False, "error": f"VS did not become ready within {timeout}s"}

def t_open_solution(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    d.Solution.Open(args["path"])
    return {"ok": True, "solution": _solution(d)}

def t_set_breakpoint(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    f = args["fileFullPath"]; line = int(args["line"])
    cond = args.get("condition", "")
    if cond:
        bp = d.Debugger.Breakpoints.Add("", f, line, 1, cond)
    else:
        bp = d.Debugger.Breakpoints.Add("", f, line)
    n = bp.Count if hasattr(bp, "Count") else 1
    return {"ok": True, "bound": n, "file": f, "line": line, "condition": cond}

def t_remove_breakpoint(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    f = args["fileFullPath"]; line = int(args["line"])
    removed = 0
    for bp in list(d.Debugger.Breakpoints):
        try:
            if os.path.normcase(bp.File or "") == os.path.normcase(f) and bp.FileLine == line:
                bp.Delete()
                removed += 1
        except Exception:
            pass
    return {"ok": True, "removed": removed}

def t_list_breakpoints(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    out = []
    for bp in d.Debugger.Breakpoints:
        try:
            out.append({"file": bp.File, "line": bp.FileLine,
                        "function": bp.FunctionName, "enabled": bp.Enabled,
                        "condition": getattr(bp, "Condition", "")})
        except Exception:
            pass
    return {"ok": True, "breakpoints": out}

def t_start_debug(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    if _solution(d) is None:
        return {"ok": False, "error": "no solution open (vs_launch / vs_open_solution first)"}
    # activate the requested config (the .suo may remember a different one -
    # e.g. Debug - which points at a stale NMakeOutput exe)
    config = args.get("configuration", "Development")
    try:
        d.Solution.SolutionBuild.SolutionConfigurations.Item(config).Activate()
    except Exception as e:
        return {"ok": False, "error": f"cannot activate config '{config}': {e}"}
    d.ExecuteCommand("Debug.Start")  # F5 via command system (async)
    timeout = int(args.get("timeoutSeconds", 300))
    t0 = time.time()
    seen_run = False
    while time.time() - t0 < timeout:
        dismiss_modal_dialogs()
        m = _mode(d)
        elapsed = time.time() - t0
        if m in ("Run", "Break"):
            return {"ok": True, "debugMode": m, "waitedSeconds": round(elapsed, 1),
                    "breakLastHit": _break_last_hit(d) if m == "Break" else None,
                    "frame": _current_frame(d) if m == "Break" else None}
        if m == "Design" and seen_run:
            return {"ok": True, "debugMode": "Design",
                    "note": "debug session ran to completion without stopping"}
        if m == "Run":
            seen_run = True
        time.sleep(0.2)
    return {"ok": False, "error": f"debug session did not start within {timeout}s"}

def t_debug_state(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    st = {"ok": True, "debugMode": _mode(d), "breakLastHit": _break_last_hit(d),
          "frame": _current_frame(d)}
    if _mode(d) == "Break":
        frames = []
        try:
            for i, sf in enumerate(d.Debugger.CurrentThread.StackFrames):
                if i >= 8: break
                frames.append({"function": sf.FunctionName, "file": sf.FileName,
                               "line": sf.Line})
        except Exception:
            pass
        st["stackTop"] = frames
    return st

def t_continue(args):
    """Continue from a Break. VS 2026 removed Debugger.Continue from the
    Dispatch interface (Go=6, Break=7, Stop=8, SetNextStatement=9 - no
    Continue) and "Debug.Continue" is not a valid command: Go(False) in
    Break mode IS continue (F5 semantics). Returns on the next stop
    (Break after a Run was observed, or last-hit line changed) or Design.
    The CLI calls UpdateWithOptions twice (check-only + apply), so the same
    line can legitimately stop twice - mode sequence is the stop signal
    (CurrentHits stays 0 on VS 18)."""
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    if _mode(d) != "Break":
        return {"ok": False, "error": f"not in Break mode (current: {_mode(d)})"}
    prev_line = (_break_last_hit(d) or {}).get("line")
    d.Debugger.Go(False)
    timeout = int(args.get("timeoutSeconds", 300))
    t0 = time.time()
    saw_run = False
    last_rego = 0.0
    while time.time() - t0 < timeout:
        dismiss_modal_dialogs()
        m = _mode(d)
        elapsed = time.time() - t0
        if m == "Run":
            saw_run = True
        if m == "Design":
            return {"ok": True, "debugMode": "Design",
                    "waitedSeconds": round(elapsed, 1),
                    "note": "debug session completed"}
        if m == "Break":
            line = (_break_last_hit(d) or {}).get("line")
            if saw_run or (line is not None and prev_line is not None and line != prev_line):
                return {"ok": True, "debugMode": "Break",
                        "waitedSeconds": round(elapsed, 1),
                        "breakLastHit": _break_last_hit(d),
                        "frame": _current_frame(d)}
            if elapsed - last_rego > 4.0:
                # parked on the same Break with no Run observed: re-issue Go
                last_rego = elapsed
                saw_run = False
                prev_line = line
                d.Debugger.Go(False)
        time.sleep(0.05)
    return {"ok": False, "error": f"no new stop within {timeout}s (mode={_mode(d)})"}

def t_step(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    if _mode(d) != "Break":
        return {"ok": False, "error": f"not in Break mode (current: {_mode(d)})"}
    kind = args.get("kind", "over")
    cmd = {"over": "Debug.StepOver", "into": "Debug.StepInto",
           "out": "Debug.StepOut"}.get(kind)
    if not cmd:
        return {"ok": False, "error": f"unknown step kind {kind}"}
    d.ExecuteCommand(cmd)
    deadline = time.time() + 30
    while time.time() < deadline:
        dismiss_modal_dialogs()
        m = _mode(d)
        if m == "Break":
            return {"ok": True, "debugMode": "Break", "frame": _current_frame(d)}
        if m == "Design":
            return {"ok": True, "debugMode": "Design",
                    "note": "session ended during step"}
        time.sleep(0.2)
    return {"ok": False, "error": "step did not settle within 30s"}

def t_stop_debug(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    d.ExecuteCommand("Debug.StopDebugging")
    return {"ok": True, "debugMode": _mode(d)}

def t_evaluate(args):
    """Known VS 18 limitation: GetExpression is intermittently rejected even
    with retries (see DEBUGGER.md). Kept for parity; prefer the debugmcp
    (codelldb) lane for expression evaluation."""
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    if _mode(d) != "Break":
        return {"ok": False, "error": f"not in Break mode (current: {_mode(d)})"}
    expr = d.Debugger.GetExpression(args["expression"])
    return {"ok": bool(expr.IsValidValue), "name": expr.Name, "type": expr.Type,
            "value": expr.Value}

def t_build(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    if _solution(d) is None:
        return {"ok": False, "error": "no solution open"}
    sb = d.Solution.SolutionBuild
    t0 = time.time()
    sb.Build(True)
    return {"ok": True, "buildSeconds": round(time.time() - t0, 1),
            "lastBuildInfo": sb.LastBuildInfo,
            "note": "LastBuildInfo > 0 = failed project count; vs_output_pane for details"}

def t_output_pane(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    pane_name = args.get("pane", "Build")
    n = int(args.get("lines", 40))
    ow = d.ToolWindows.OutputWindow
    pane = None
    for p in ow.OutputWindowPanes:
        if p.Name == pane_name:
            pane = p
            break
    if pane is None:
        names = [p.Name for p in ow.OutputWindowPanes]
        return {"ok": False, "error": f"pane '{pane_name}' not found; panes: {names}"}
    td = pane.TextDocument
    ep = td.EndPoint
    sp = ep.CreateEditPoint()
    if ep.Line > n:
        sp.MoveToLineAndOffset(max(1, ep.Line - n), 1)
    return {"ok": True, "pane": pane_name, "text": sp.GetText(ep)}

def t_execute_command(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    d.ExecuteCommand(args["command"], args.get("args", ""))
    return {"ok": True, "executed": args["command"]}

def t_set_f5_never_build(args):
    d, err = dte_or_err()
    if err: return {"ok": False, "error": err}
    found = []
    for page_name in ("ProjectsAndSolution", "BuildAndRun"):
        try:
            props = d.Properties("Environment", page_name)
        except Exception:
            continue
        for p in props:
            try:
                nm = p.Name
            except Exception:
                continue
            if "buildrun" in nm.lower() or "outofdate" in nm.lower() or "onrun" in nm.lower():
                found.append((page_name, nm, p.Value))
    for page_name, nm, _v in found:
        try:
            d.Properties("Environment", page_name).Item(nm).Value = 2  # Never build
        except Exception as e:
            return {"ok": False, "error": f"found {nm} but set failed: {e}", "found": found}
    if not found:
        return {"ok": False, "error": "setting not found in Environment/ProjectsAndSolution"}
    return {"ok": True, "set": found}

def _quit():
    d, err = dte_or_err()
    if not err:
        d.Quit()

# ---------------------------------------------------------------------------
# MCP plumbing (stdio, newline-delimited JSON-RPC)

TOOLS = {
    "vs_status": ("VS running? solution open? debugger mode, last breakpoint hit, debugged processes",
                  t_status, {"type": "object", "properties": {}}),
    "vs_launch": ("Start Visual Studio 2026 with a solution (default: the UGP debug-only .sln) and wait until DTE is reachable",
                  t_launch, {"type": "object", "properties": {
                      "solutionPath": {"type": "string"},
                      "timeoutSeconds": {"type": "integer", "minimum": 30, "maximum": 600}}}),
    "vs_open_solution": ("Open a .sln in the running VS",
                         t_open_solution, {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}),
    "vs_set_breakpoint": ("Set a breakpoint (fileFullPath + 1-based line, optional condition) using the VS debugger",
                          t_set_breakpoint, {"type": "object", "properties": {
                              "fileFullPath": {"type": "string"}, "line": {"type": "integer"},
                              "condition": {"type": "string"}}, "required": ["fileFullPath", "line"]}),
    "vs_remove_breakpoint": ("Remove a breakpoint at file/line",
                             t_remove_breakpoint, {"type": "object", "properties": {
                                 "fileFullPath": {"type": "string"}, "line": {"type": "integer"}},
                                 "required": ["fileFullPath", "line"]}),
    "vs_list_breakpoints": ("List all breakpoints", t_list_breakpoints,
                            {"type": "object", "properties": {}}),
    "vs_start_debug": ("F5 under the real VS debugger (activates the Development config first; async with mode polling)",
                       t_start_debug, {"type": "object", "properties": {
                           "configuration": {"type": "string"},
                           "timeoutSeconds": {"type": "integer", "minimum": 30, "maximum": 900}}}),
    "vs_debug_state": ("Debugger mode + last breakpoint hit + current frame + stack top (read-only)",
                       t_debug_state, {"type": "object", "properties": {}}),
    "vs_continue": ("Continue until the next breakpoint hit or the session ends",
                    t_continue, {"type": "object", "properties": {
                        "timeoutSeconds": {"type": "integer", "minimum": 10, "maximum": 900}}}),
    "vs_step": ("Step over/into/out while in Break mode", t_step,
                {"type": "object", "properties": {"kind": {"type": "string", "enum": ["over", "into", "out"]}}}),
    "vs_stop_debug": ("Stop debugging / terminate the debuggee", t_stop_debug,
                      {"type": "object", "properties": {}}),
    "vs_evaluate": ("Evaluate an expression in the current Break frame (UNRELIABLE on VS 18 - prefer the debugmcp lane)",
                    t_evaluate, {"type": "object", "properties": {"expression": {"type": "string"}}, "required": ["expression"]}),
    "vs_build": ("Build the open solution synchronously (NMake/UBT)", t_build,
                 {"type": "object", "properties": {}}),
    "vs_output_pane": ("Read the tail of an Output Window pane (Build/Debug/...)", t_output_pane,
                       {"type": "object", "properties": {"pane": {"type": "string"}, "lines": {"type": "integer"}}}),
    "vs_execute_command": ("Execute any VS command (DTE ExecuteCommand) - full-IDE fallback", t_execute_command,
                           {"type": "object", "properties": {"command": {"type": "string"}, "args": {"type": "string"}},
                            "required": ["command"]}),
    "vs_set_f5_never_build": ("Set 'On Run, when projects are out of date' = Never build (NMake F5 builds hang in the VS host)",
                              t_set_f5_never_build, {"type": "object", "properties": {}}),
    "vs_quit": ("Quit Visual Studio (discards unsaved changes - for clean teardown)",
                lambda a: (_quit(), {"ok": True})[1], {"type": "object", "properties": {}}),
}

def _emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()

def main():
    _com_init()

    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        rid = msg.get("id")
        if rid is None:
            continue  # notification (initialized etc.)
        method = msg.get("method", "")
        params = msg.get("params") or {}
        try:
            if method == "initialize":
                _emit({"jsonrpc": "2.0", "id": rid, "result": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "vs-mcp-bridge", "version": "1.1.0"}}})
            elif method == "tools/list":
                _emit({"jsonrpc": "2.0", "id": rid, "result": {
                    "tools": [{"name": n, "description": desc, "inputSchema": s}
                              for n, (desc, _f, s) in TOOLS.items()]}})
            elif method == "tools/call":
                name = params.get("name", "")
                entry = TOOLS.get(name)
                if entry is None:
                    _emit({"jsonrpc": "2.0", "id": rid, "result": {
                        "content": [{"type": "text", "text": f"unknown tool {name}"}],
                        "isError": True}})
                    continue
                try:
                    result = entry[1](params.get("arguments") or {})
                    _emit({"jsonrpc": "2.0", "id": rid, "result": {
                        "content": [{"type": "text",
                                     "text": json.dumps(result, ensure_ascii=False, indent=1)}]}})
                except Exception as e:
                    hint = "is a modal dialog open in VS?" if "rejected" in str(e).lower() else ""
                    _emit({"jsonrpc": "2.0", "id": rid, "result": {
                        "content": [{"type": "text",
                                     "text": json.dumps({"ok": False,
                                                         "error": f"{type(e).__name__}: {e}",
                                                         "hint": hint})}],
                        "isError": True}})
            else:
                _emit({"jsonrpc": "2.0", "id": rid,
                       "error": {"code": -32601, "message": method}})
        except Exception as e:  # never crash the loop
            _emit({"jsonrpc": "2.0", "id": rid,
                   "error": {"code": -32603, "message": str(e)}})

if __name__ == "__main__":
    main()
