#!/usr/bin/env python3
"""mcp_probe.py - minimal MCP stdio client to introspect the debugmcp server.

Speaks newline-delimited JSON-RPC 2.0 over stdio (MCP stdio transport).
Prints tool names + input schemas so the real verify driver can be written
against the actual API shape.
"""
import json
import subprocess
import sys
import threading
import queue

DEBUGMCP_JS = r"C:\Users\imxql\AppData\Roaming\npm\node_modules\debugmcp\dist\debugmcp.js"

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

    def request(self, method, params=None, timeout=30):
        self.next_id += 1
        rid = self.next_id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()
        while True:
            item = self.q.get(timeout=timeout)
            if item.get("id") == rid:
                if "error" in item:
                    raise RuntimeError(f"{method} error: {item['error']}")
                return item.get("result")
            # ignore notifications / other ids

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()

def main():
    proc = subprocess.Popen(
        [sys.executable, "-u", DEBUGMCP_JS, "serve", "--stdio"] if False else
        ["node", DEBUGMCP_JS, "serve", "--stdio"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    cli = McpClient(proc)
    result = cli.request("initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "ugp-debug-probe", "version": "0.1"},
    }, timeout=20)
    print("== serverInfo:", json.dumps(result.get("serverInfo", {})))
    print("== capabilities keys:", list(result.get("capabilities", {}).keys()))
    cli.notify("notifications/initialized")

    tools = cli.request("tools/list", {}, timeout=20)
    for t in tools.get("tools", []):
        print(f"\n### {t['name']}")
        print("   desc:", (t.get("description") or "")[:160].replace("\n", " "))
        print("   schema:", json.dumps(t.get("inputSchema", {}), ensure_ascii=False)[:800])

    proc.stdin.close()
    proc.terminate()

if __name__ == "__main__":
    main()
