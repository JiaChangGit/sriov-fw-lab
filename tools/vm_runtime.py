"""Serial transport and guest checkpoints shared by direct and UEFI boots."""
import json
from pathlib import Path
import select
import socket
import time

from lab_common import sha256, write_json


def connect_serial(path, timeout=20):
    stream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    deadline = time.monotonic() + timeout
    while True:
        try:
            stream.connect(str(path))
            return stream
        except (FileNotFoundError, ConnectionRefusedError):
            if time.monotonic() >= deadline:
                stream.close()
                raise TimeoutError("Serial socket did not become available")
            time.sleep(0.05)


def object_snapshot(qmp):
    deadline = time.monotonic() + 2
    while True:
        owned = {name: qmp.execute("qom-get", {"path": qmp.toy_path, "property": name})
                 for name in ("live-vfs", "pending-completions", "worker-pending", "mailbox-pending")}
        if all(value == 0 for value in owned.values()):
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("Device-owned objects or work remain at disabled checkpoint: " + json.dumps(owned))
        time.sleep(0.01)
    pending = ["/machine"]
    children = []
    while pending:
        path = pending.pop()
        for prop in qmp.execute("qom-list", {"path": path}):
            if prop["type"].startswith("child<"):
                child = path + "/" + prop["name"]
                children.append({"path": child, "type": prop["type"]})
                pending.append(child)
                if len(children) > 4096:
                    raise RuntimeError("Unexpected QOM object count")
    return {"owned": owned, "children": sorted(children, key=lambda item: item["path"]),
            "pci": qmp.execute("query-pci")}


def monitor_guest(process, qmp, serial_path, out, timeout, event_handler=None):
    """Resume after UART connection, preserve bytes, inspect both T01 checkpoints."""
    deadline = time.monotonic() + timeout
    initial = None
    final = None
    cold = None
    partial = b""
    with connect_serial(serial_path) as stream, (out / "serial.log").open("wb") as log:
        Path(serial_path).chmod(0o600)
        qmp.execute("cont")
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError("Guest exceeded configured deadline")
            readable, _, _ = select.select([stream], [], [], 0.25)
            if not readable:
                if process.poll() is not None:
                    break
                continue
            chunk = stream.recv(65536)
            if not chunk:
                break
            log.write(chunk)
            log.flush()
            partial += chunk
            if len(partial) > 1024 * 1024:
                raise RuntimeError("Guest serial line exceeded bounded parser size")
            while b"\n" in partial:
                line, partial = partial.split(b"\n", 1)
                try:
                    item = json.loads(line)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue
                if not isinstance(item, dict):
                    continue
                if event_handler is not None:
                    event_handler(item, qmp, stream)
                event = item.get("event")
                if event not in ("M1_COLD_CHECKPOINT", "M1_INITIAL_CHECKPOINT", "M1_FINAL_CHECKPOINT"):
                    continue
                snapshot = object_snapshot(qmp)
                if event == "M1_COLD_CHECKPOINT":
                    if cold is not None or initial is not None:
                        raise RuntimeError("Invalid cold checkpoint order")
                    cold = snapshot
                    write_json(out / "qom-cold.json", snapshot)
                elif event == "M1_INITIAL_CHECKPOINT":
                    if initial is not None:
                        raise RuntimeError("Duplicate initial host checkpoint")
                    if cold is None:
                        raise RuntimeError("Cold checkpoint missing")
                    initial = snapshot
                    write_json(out / "qom-initial.json", snapshot)
                else:
                    if initial is None or final is not None:
                        raise RuntimeError("Invalid final host checkpoint order")
                    final = snapshot
                    write_json(out / "qom-final.json", snapshot)
                    if initial["children"] != final["children"]:
                        raise RuntimeError("QEMU object inventory grew or changed across 100 VF cycles")
                    # PCI 配置欄位可能正常改變；只比較實際存在的 requester 身分。
                    def identities(buses):
                        found = []
                        for bus in buses:
                            for dev in bus.get("devices", []):
                                found.append((bus["bus"], dev["slot"], dev["function"],
                                              dev["id"]["vendor"], dev["id"]["device"]))
                                bridge = dev.get("pci_bridge")
                                if bridge:
                                    found.extend(identities([{"bus": bridge["bus"]["number"],
                                                               "devices": bridge.get("devices", [])}]))
                        return sorted(found)
                    if identities(initial["pci"]) != identities(final["pci"]):
                        raise RuntimeError("PCI requester inventory changed across 100 VF cycles")
                stream.sendall(b"M1_HOST_CHECKED\n")
    remaining = max(1, deadline - time.monotonic())
    exit_code = process.wait(timeout=remaining)
    return {"exit_code": exit_code,
            "host_checkpoints": {
                "status": "PASS" if initial is not None and final is not None else "NOT_RUN",
                "warmup_cycles": 1,
                "measured_cycles": 100,
                "cold_objects": len(cold["children"]) if cold else None,
                "initial_objects": len(initial["children"]) if initial else None,
                "final_objects": len(final["children"]) if final else None,
                "initial_sha256": sha256(out / "qom-initial.json") if initial else None,
                "final_sha256": sha256(out / "qom-final.json") if final else None}}
