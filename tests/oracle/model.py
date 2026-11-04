"""Independent executable interpretation of HTML sections 8--12.

This module never loads C source, generated tables, headers, or compiled constants.
Integer arithmetic uses Python's unbounded integers and Fraction for fair shares.
"""
from fractions import Fraction

MAX64 = (1 << 64) - 1
EDGES = {
    (0, 0): 1, (1, 1): 2, (1, 2): 0, (1, 3): 6,
    (2, 4): 3, (3, 5): 4, (3, 6): 6, (4, 7): 5,
    (4, 6): 6, (5, 8): 2, (5, 9): 6,
    (2, 10): 5, (3, 10): 5, (4, 10): 5, (5, 10): 5,
    (2, 11): 7, (6, 11): 7, (5, 11): 7, (7, 12): 0, (7, 13): 6,
}


def fair_service(reserves, backlog, rotation):
    result = [min(r, b) for r, b in zip(reserves, backlog)]
    remaining = 64 - sum(result)
    while remaining:
        candidates = [i for i in range(8) if backlog[i] > result[i]]
        if not candidates:
            break
        total = sum(reserves[i] for i in candidates)
        shares = {i: Fraction(remaining * reserves[i], total) for i in candidates}
        saturated = [i for i in candidates if shares[i] >= backlog[i] - result[i]]
        if saturated:
            for i in saturated:
                remaining -= backlog[i] - result[i]
                result[i] = backlog[i]
            continue
        whole = {i: int(shares[i]) for i in candidates}
        for i in candidates:
            result[i] += whole[i]
        remaining -= sum(whole.values())
        ordered = sorted(candidates, key=lambda i: (-(shares[i] - whole[i]), (i - rotation) % 8))
        for i in ordered[:remaining]:
            result[i] += 1
        break
    assert sum(result) <= 64
    assert all(0 <= n <= b for n, b in zip(result, backlog))
    return result


class Model:
    def __init__(self):
        self.session = 1
        self.round = self.queues = self.vectors = self.credit = self.last = 0
        self.negotiated = self.cache_valid = 0
        self.cached = None
        self.cached_result = None
        self.tick = 0
        self.pending = 0
        self.pending_epoch = self.pending_deadline = 0
        self.hold = self.failpoints = 0
        self.seen = [0] * 5
        self.v = [dict(state=0, reserve=0, epoch=1, deadline=0, serviced=0) for _ in range(8)]
        self.h = [dict(session=1, epoch=1, token=0, ready=0, outstanding=0,
                       fence=1, kill=0, bme=0, config=0, pending=0, present=0, closed=1) for _ in range(8)]

    def create(self, i, reserve):
        if i >= 8 or not 1 <= reserve <= 64:
            return 1
        v, h = self.v[i], self.h[i]
        if v['state'] != 0 or not h['fence'] or h['kill'] or h['config'] or h['outstanding'] or h['present']:
            return 3
        if self.credit + reserve > 64 or self.queues == 8:
            return 2
        v.update(state=1, reserve=reserve, epoch=h['epoch'])
        self.queues += 1
        self.vectors += 2
        self.credit += reserve
        return 0

    def reserve(self, i, reserve):
        if i >= 8 or not 1 <= reserve <= 64:
            return 1
        v, h = self.v[i], self.h[i]
        if v['state'] not in (2, 5) or not h['fence'] or h['outstanding'] or h['kill']:
            return 3
        if self.credit - v['reserve'] + reserve > 64:
            return 2
        self.credit += reserve - v['reserve']
        v['reserve'] = reserve
        return 0

    def rearm(self, i, epoch, ready):
        if i >= 8 or not ready:
            return 1
        v, h = self.v[i], self.h[i]
        if (v['state'] not in (2, 5) or h['epoch'] != epoch or v['epoch'] != epoch or
                not h['config'] or not h['pending'] or h['ready'] != ready or h['kill'] or
                not h['fence'] or not h['bme'] or h['outstanding']):
            return 3
        h.update(fence=0, pending=0)
        if v['state'] == 5 and self.tick >= v['deadline']:
            h['fence'] = 1
            self.event(i, 9)
            return 6
        v.update(state=2, deadline=0)
        return 0

    def event(self, i, event):
        if i >= 8:
            return 1
        v, h = self.v[i], self.h[i]
        dest = EDGES.get((v['state'], event))
        if dest is None:
            return 3
        if event == 0:
            return self.create(i, 8)
        if not h['fence']:
            return 3
        if event == 8:
            return self.rearm(i, h['epoch'], h['ready'])
        deadline = self.tick + (1000 if event in (7, 10) else 100) if event in (4, 5, 7, 10) else 0
        if deadline > MAX64:
            return 12
        if event == 1:
            if not v['reserve'] or h['kill'] or h['config'] or h['outstanding']:
                return 3
        elif event in (2, 12):
            if h['present'] or h['config'] or h['outstanding'] or not h['closed']:
                return 3
            self.credit -= v['reserve']
            self.queues -= 1
            self.vectors -= 2
            v['reserve'] = 0
        elif event in (3, 6, 9, 13):
            if event == 6 and self.tick < v['deadline']:
                return 3
            h.update(fence=1, kill=1, outstanding=0, config=0, pending=0)
        elif event == 4:
            if h['epoch'] <= v['epoch']:
                return 3
            h['outstanding'] = 0
        elif event in (5, 7):
            if h['outstanding']:
                return 3
            if event == 7:
                h.update(config=0, pending=0, outstanding=0)
        elif event == 10:
            if h['epoch'] <= v['epoch'] or h['outstanding'] or h['config']:
                return 3
        elif event == 11:
            if not h['closed'] or h['outstanding'] or (v['state'] != 6 and h['epoch'] <= v['epoch']):
                return 3
            h.update(config=0, pending=0, outstanding=0)
        v.update(state=dest, epoch=h['epoch'], deadline=deadline)
        return 0

    def recover(self):
        if any(not h['closed'] or h['present'] for h in self.h):
            return 3
        if self.session == MAX64 or any(h['epoch'] == MAX64 or h['token'] == MAX64 for h in self.h):
            return 12
        self.session += 1
        for v, h in zip(self.v, self.h):
            h.update(session=self.session, epoch=h['epoch'] + 1, token=h['token'] + 1,
                     fence=1, config=0, pending=0, outstanding=0, kill=0)
            v.update(state=0, reserve=0, epoch=h['epoch'], deadline=0, serviced=0)
        self.round = self.queues = self.vectors = self.credit = self.last = 0
        self.negotiated = self.cache_valid = 0
        self.cached = self.cached_result = None
        self.pending = self.pending_epoch = self.pending_deadline = 0
        return 0

    def boundary(self, stage):
        self.seen[stage] += 1
        armed = self.failpoints & (1 << stage)
        self.failpoints &= ~(1 << stage)
        return 15 if armed else 0

    def finish(self, target, opcode, epoch, deadline):
        v = self.v[target]
        expected = {4: 3, 5: 4}[opcode]
        if v['epoch'] != epoch:
            return 3
        if v['state'] == 6 and self.tick >= deadline:
            return 6
        if v['state'] != expected:
            return 3
        if self.tick >= deadline:
            self.event(target, 6)
            return 6
        if self.hold == opcode:
            return 11
        return self.event(target, 5 if opcode == 4 else 7)

    def mailbox(self, r):
        session, seq, op, target, *args = r
        values = [0] * 4
        if not seq:
            return 1, values
        if session != self.session:
            return 10, values
        if self.cache_valid and seq == self.last:
            return self.cached_result if tuple(r) == self.cached else (9, values)
        if seq <= self.last:
            return 10, values
        if not 1 <= op <= 9:
            return 7, values
        if op == 1:
            if target != 65535 or args[3] or args[1] > args[2]:
                return 1, values
            if args[0] != 1 or args[1] > 0:
                return 7, values
        else:
            if not self.negotiated:
                return 8, values
            if (op == 9 and target != 65535) or (op != 9 and target >= 8):
                return 1, values
            used = 1 if op in (3, 8) else 3 if op == 6 else 0
            if (op in (3, 8) and not 1 <= args[0] <= 64) or (op == 6 and not args[2]) or any(args[used:]):
                return 1, values
        if self.pending:
            return 11, values
        if op == 1:
            self.negotiated = 1
            values[0] = 65536
            rc = 0
        elif op == 2:
            h = self.h[target]
            values = [self.v[target]['state'], h['epoch'] & 0xffffffff, h['epoch'] >> 32,
                      h['fence'] | h['kill'] << 1 | h['present'] << 2 | h['bme'] << 3 | h['config'] << 4 | h['pending'] << 5]
            rc = 0
        elif op == 3:
            rc = self.boundary(0)
            if not rc:
                rc = self.create(target, args[0])
                if not rc:
                    rc = self.boundary(1) or self.boundary(2) or self.event(target, 1)
                    if rc:
                        if self.boundary(3) or self.event(target, 2):
                            self.event(target, 3)
        elif op == 4:
            rc = self.event(target, 4)
            if not rc:
                rc = self.finish(target, op, self.v[target]['epoch'], self.v[target]['deadline'])
        elif op == 5:
            rc = self.finish(target, op, self.v[target]['epoch'], self.v[target]['deadline'])
        elif op == 6:
            rc = self.rearm(target, args[0] | args[1] << 32, args[2])
        elif op == 7:
            rc = self.event(target, 11)
            if not rc:
                rc = self.boundary(4) or self.event(target, 12)
                if rc:
                    self.event(target, 13)
        elif op == 8:
            rc = self.reserve(target, args[0])
        else:
            rc = self.recover()
            if rc == 0:
                values[0], values[1] = self.session & 0xffffffff, self.session >> 32
                return rc, values
        self.last = seq
        self.cache_valid = 1
        self.cached = tuple(r)
        self.cached_result = (rc, values[:])
        if rc == 11 and op in (4, 5):
            self.pending = 1
            self.pending_epoch = self.v[target]['epoch']
            self.pending_deadline = self.v[target]['deadline']
        return rc, values

    def step(self, op, a):
        reply, service = [0] * 4, [0] * 8
        i = a[0] if a else 0
        if op == 'C':
            rc = self.create(*a)
        elif op == 'E':
            rc = self.event(*a)
        elif op == 'F':
            h = self.h[i]
            h.update(fence=1, epoch=h['epoch'] + 1, token=h['token'] + 1, outstanding=0)
            if a[1]:
                h.update(config=0, pending=0)
            rc = 0
        elif op == 'Q':
            h = self.h[i]
            h.update(config=1, pending=1, bme=1, ready=h['ready'] + 1)
            rc = 0
        elif op == 'A':
            rc = self.rearm(*a)
        elif op == 'R':
            rc = self.reserve(*a)
        elif op == 'G':
            rc = self.recover()
        elif op == 'T':
            self.tick, rc = i, 0
        elif op == 'P':
            rc = 0
            for j, v in enumerate(self.v):
                if v['state'] in (3, 4, 5) and self.tick >= v['deadline']:
                    if v['state'] == 5 and not self.h[j]['present']:
                        continue
                    one = self.event(j, 9 if v['state'] == 5 else 6)
                    rc = one or rc
        elif op == 'O':
            self.h[i]['closed'], rc = a[1], 0
        elif op == 'X':
            self.failpoints, rc = i, 0
        elif op == 'H':
            self.hold, rc = i, 0
        elif op == 'B':
            rc = 3
            if self.pending:
                rc = self.finish(self.cached[3], self.cached[2], self.pending_epoch, self.pending_deadline)
                reply = self.cached_result[1][:]
                self.cached_result = (rc, reply[:])
                if rc != 11:
                    self.pending = self.pending_epoch = self.pending_deadline = 0
        elif op == 'S':
            demand = [b if v['state'] == 2 and not h['fence'] and not h['kill'] and h['bme'] and h['config'] and not h['outstanding'] else 0
                      for b, v, h in zip(a, self.v, self.h)]
            service = fair_service([v['reserve'] for v in self.v], demand, self.round % 8)
            for v, s in zip(self.v, service):
                v['serviced'] += s
            self.round += 1
            rc = 0
        elif op == 'M':
            rc, reply = self.mailbox(a)
        else:
            raise ValueError(op)
        assert self.credit == sum(v['reserve'] for v in self.v) <= 64
        assert self.queues == sum(bool(v['reserve']) for v in self.v)
        assert self.vectors == 2 * self.queues <= 16
        for v, h in zip(self.v, self.h):
            if v['state'] == 0:
                assert not v['reserve'] and not h['config'] and not h['outstanding']
        result = [rc, *reply, *service, self.session, self.round, self.queues, self.vectors,
                  self.credit, self.last, self.negotiated, self.cache_valid,
                  self.pending, self.pending_epoch, self.pending_deadline, self.hold, self.failpoints, *self.seen]
        for v, h in zip(self.v, self.h):
            result.extend(v[key] for key in ('state', 'reserve', 'epoch', 'deadline', 'serviced'))
            result.extend(h[key] for key in ('session', 'epoch', 'token', 'ready', 'outstanding', 'fence', 'kill', 'bme', 'config', 'pending', 'present', 'closed'))
        return result
