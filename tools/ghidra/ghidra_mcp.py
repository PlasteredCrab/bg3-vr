"""Minimal client for the GhidrAssistMCP server (streamable HTTP, SSE-framed replies).

Standard library only. Used by tools/export_symbols.py and for bulk work.

    from ghidra_mcp import Mcp
    m = Mcp()                      # default http://127.0.0.1:8080/mcp
    print(m.call("get_binary_info"))
"""
import json
import urllib.request

DEFAULT_URL = "http://127.0.0.1:8080/mcp"


class Mcp:
    def __init__(self, url=DEFAULT_URL, timeout=600):
        self.url = url
        self.timeout = timeout
        self.session = None
        self._id = 0
        r = self._rpc("initialize", {
            "protocolVersion": "2025-03-26",
            "capabilities": {},
            "clientInfo": {"name": "b3rs-tools", "version": "0.1"},
        })
        self.server_info = r.get("serverInfo")
        self._post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _post(self, payload):
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(self.url, data=data, method="POST")
        req.add_header("Content-Type", "application/json")
        req.add_header("Accept", "application/json, text/event-stream")
        if self.session:
            req.add_header("Mcp-Session-Id", self.session)
        with urllib.request.urlopen(req, timeout=self.timeout) as resp:
            sid = resp.headers.get("Mcp-Session-Id")
            if sid:
                self.session = sid
            body = resp.read().decode("utf-8", errors="replace")
        return body

    @staticmethod
    def _parse(body):
        body = body.strip()
        if not body:
            return None
        if body.startswith("{"):
            return json.loads(body)
        # SSE framing: take the last data: payload that parses as JSON.
        out = None
        buf = []
        for line in body.splitlines():
            if line.startswith("data:"):
                buf.append(line[5:].lstrip())
            elif line == "" and buf:
                try:
                    out = json.loads("\n".join(buf))
                except ValueError:
                    pass
                buf = []
        if buf:
            try:
                out = json.loads("\n".join(buf))
            except ValueError:
                pass
        return out

    def _rpc(self, method, params):
        self._id += 1
        msg = self._parse(self._post({"jsonrpc": "2.0", "id": self._id,
                                      "method": method, "params": params}))
        if msg is None:
            raise RuntimeError("empty reply to " + method)
        if "error" in msg:
            raise RuntimeError(f"{method}: {msg['error']}")
        return msg["result"]

    def call(self, tool, **args):
        """Call a tool; return the concatenated text content."""
        r = self._rpc("tools/call", {"name": tool, "arguments": args})
        parts = [c.get("text", "") for c in r.get("content", []) if c.get("type") == "text"]
        text = "\n".join(parts)
        if r.get("isError"):
            raise RuntimeError(f"{tool}: {text[:500]}")
        return text

    def call_wait(self, tool, poll=0.2, timeout=300, **args):
        """Call a tool that may run as an async task; poll until it finishes."""
        import re
        import time
        text = self.call(tool, **args)
        m = re.search(r"Task ID: ([0-9a-f-]{36})", text)
        if not m:
            return text
        tid = m.group(1)
        end = time.time() + timeout
        while time.time() < end:
            time.sleep(poll)
            st = self.call("get_task_status", task_id=tid)
            if "RUNNING" in st or "PENDING" in st:
                continue
            return st
        raise RuntimeError(f"{tool}: task {tid} timed out")
