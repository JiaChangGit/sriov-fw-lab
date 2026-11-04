"""Bounded QMP client with schema/property discovery and raw JSON evidence."""
import json
from pathlib import Path
import socket
import time


class QMP:
    def __init__(self, path, log, timeout=20):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(timeout)
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.socket.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() >= deadline:
                    self.socket.close()
                    raise TimeoutError("QMP socket did not become available")
                time.sleep(0.05)
        self.reader = self.socket.makefile("rb")
        self.log = Path(log).open("a", encoding="utf-8")
        self.sequence = 0
        self.events = []
        hello = self.receive()
        if "QMP" not in hello:
            raise RuntimeError("Invalid QMP greeting")
        self.execute("qmp_capabilities")
        self.schema = self.execute("query-qmp-schema")
        self.toy_path = self.discover_toy()
        self.properties = {entry["name"]: entry["type"] for entry in self.execute("qom-list", {"path": self.toy_path})}

    def receive(self):
        line = self.reader.readline(8 * 1024 * 1024)
        if not line:
            raise EOFError("QMP connection closed")
        value = json.loads(line)
        self.log.write(json.dumps({"direction": "receive", "value": value}) + "\n")
        self.log.flush()
        return value

    def execute(self, command, arguments=None):
        self.sequence += 1
        request = {"execute": command, "id": self.sequence}
        if arguments is not None:
            request["arguments"] = arguments
        self.log.write(json.dumps({"direction": "send", "value": request}) + "\n")
        self.log.flush()
        self.socket.sendall(json.dumps(request).encode() + b"\r\n")
        while True:
            response = self.receive()
            if "event" in response:
                self.events.append(response)
                continue
            if response.get("id") != self.sequence:
                raise RuntimeError("Unexpected QMP response id")
            if "error" in response:
                raise RuntimeError("QMP error: " + json.dumps(response["error"]))
            return response["return"]

    def discover_toy(self):
        path = "/"
        for component in ("machine", "peripheral", "toy"):
            entries = self.execute("qom-list", {"path": path})
            matches = [item for item in entries if item["name"] == component and item["type"].startswith("child<")]
            if len(matches) != 1:
                raise RuntimeError("QOM discovery failed at " + component)
            path = path.rstrip("/") + "/" + matches[0]["name"]
        return path

    def fault(self, name, value):
        if name not in self.properties:
            raise ValueError("Fault property absent from actual QOM schema: " + name)
        before = self.execute("qom-get", {"path": self.toy_path, "property": "fault-ack"})
        self.execute("qom-set", {"path": self.toy_path, "property": name, "value": value})
        after = self.execute("qom-get", {"path": self.toy_path, "property": "fault-ack"})
        if after <= before:
            raise RuntimeError("Fault injection was not acknowledged")
        return {"property": name, "value": value, "ack": after}

    def close(self):
        self.reader.close()
        self.socket.close()
        self.log.close()
