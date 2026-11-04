"""Acknowledged, bounded guest barriers for a single owned QEMU process."""
import json
import re
import time
from lab_common import write_json
from vm_runtime import object_snapshot

POINTS = {"provision.before_create", "provision.after_create", "provision.before_commit",
          "provision.abort_release", "destroy.release"}


class M3Controller:
    def __init__(self, directory, scenario):
        self.directory = directory
        self.scenario = scenario
        self.barriers = set()
        self.armed = {}
        self.inventory = {}
        self.records = []

    def need(self, condition, reason):
        if not condition:
            raise RuntimeError("M3 controller: " + reason)

    def get(self, qmp, property_name):
        return qmp.execute("qom-get", {"path": qmp.toy_path, "property": property_name})

    def __call__(self, item, qmp, stream):
        if item.get("event") != "M3_BARRIER":
            return
        token = item.get("barrier")
        self.need(item.get("scenario") == self.scenario and item.get("phase") == "request", "scenario or phase")
        self.need(isinstance(token, str) and re.fullmatch(r"[a-zA-Z0-9_-]{1,64}", token), "invalid barrier token")
        self.need(token not in self.barriers and len(self.barriers) < 4096, "duplicate or excessive barrier")
        self.barriers.add(token)
        action = item.get("action")
        record = {"request": item, "status": "FAIL"}
        self.records.append(record)
        try:
            if action in ("failpoint-arm", "failpoint-check"):
                slot = item.get("slot")
                self.need(slot in ("forward", "unwind"), "unknown failpoint slot")
                prefix = "failpoint-" if slot == "forward" else "unwind-failpoint-"
                if action == "failpoint-arm":
                    name, hit, vf = item.get("name"), item.get("hit"), item.get("vf")
                    self.need(name in POINTS, "unknown failpoint name")
                    self.need(type(hit) is int and 1 <= hit <= 100000 and type(vf) is int and
                              (0 <= vf < 8 or vf == 0xffffffff), "invalid hit or target")
                    self.need(slot not in self.armed, "previous failpoint has not been checked")
                    record["acks"] = [qmp.fault(prefix + key, value) for key, value in
                                      (("vf", vf), ("hit", hit), ("name", name))]
                    self.need(self.get(qmp, prefix + "name") == name and self.get(qmp, prefix + "seen") == 0 and
                              self.get(qmp, prefix + "fired") == 0, "armed state mismatch")
                    self.armed[slot] = {"name": name, "hit": hit, "vf": vf}
                else:
                    self.need(slot in self.armed, "unarmed failpoint check")
                    armed = self.armed[slot]
                    seen, fired = self.get(qmp, prefix + "seen"), self.get(qmp, prefix + "fired")
                    self.need(self.get(qmp, prefix + "name") == armed["name"] and fired == 1 and seen >= armed["hit"],
                              "required failpoint did not fire")
                    record["observation"] = {**armed, "seen": seen, "fired": fired}
                    record["disarm"] = qmp.fault(prefix + "name", "")
                    del self.armed[slot]
            elif action == "inventory":
                name = item.get("name")
                self.need(name in ("cold", "initial", "final") and name not in self.inventory, "inventory name/order")
                if name == "initial":
                    self.need("cold" in self.inventory, "cold inventory missing")
                if name == "final":
                    self.need("initial" in self.inventory, "initial inventory missing")
                snapshot = object_snapshot(qmp)
                self.inventory[name] = snapshot
                write_json(self.directory / ("m3-qom-" + name + ".json"), snapshot)
                if name == "final":
                    self.need(snapshot["children"] == self.inventory["initial"]["children"], "QOM inventory changed")
                record["observation"] = {"objects": len(snapshot["children"]), "owned": snapshot["owned"]}
            elif action == "qmp-set":
                name, value = item.get("property"), item.get("value")
                allowed_bool = {"fw-hang-vf", "fw-hang-all", "ignore-quiesce", "ignore-reset", "drop-mbx-reply-once", "try-stale-cpl"}
                allowed_int = {"fault-vf": 7, "fault-after": 0xffffffff, "delay-cpl-ticks": 60000}
                self.need((name in allowed_bool and type(value) is bool) or
                          (name in allowed_int and type(value) is int and 0 <= value <= allowed_int[name]),
                          "unsupported fault property or value")
                record["ack"] = qmp.fault(name, value)
            elif action == "pending-check":
                expected = item.get("expected")
                self.need(type(expected) is int and expected in (0,1), "pending count range")
                actual = self.get(qmp, "pending-completions")
                self.need(type(actual) is int and actual == expected, "actual retained completion count")
                record["observation"] = {"pending":actual, "tick":self.get(qmp,"virtual-tick")}
            elif action == "virtual-delay":
                ticks = item.get("ticks")
                self.need(type(ticks) is int and 1 <= ticks <= 500, "bounded virtual delay")
                first = self.get(qmp,"virtual-tick")
                self.need(type(first) is int and first >= 0, "invalid virtual clock sample")
                current = first
                deadline = time.monotonic()+10
                while current-first < ticks:
                    self.need(time.monotonic() < deadline, "virtual clock made no bounded progress")
                    # Wall time only bounds polling; device virtual ticks determine completion.
                    time.sleep(0.005)
                    previous = current
                    current = self.get(qmp,"virtual-tick")
                    self.need(type(current) is int and current >= previous, "virtual clock regressed")
                record["observation"] = {"first_tick":first,"last_tick":current,"requested_ticks":ticks}
            else:
                raise RuntimeError("M3 controller: unsupported barrier action")
            record["status"] = "PASS"
            self.save()
            stream.sendall(("M3_HOST_ACK " + token + "\n").encode("ascii"))
        except Exception as error:
            record["status"] = "FAIL"
            record["error"] = str(error)
            self.save()
            raise

    def save(self):
        write_json(self.directory / "m3-barriers.json", {"scenario": self.scenario, "records": self.records,
                                                      "pending_failpoints": self.armed})

    def finish(self):
        self.save()
        self.need(not self.armed, "unchecked failpoint remains armed")
        return {"status": "PASS", "barriers": len(self.records)}
