# SPDX-License-Identifier: MIT
"""sv.c's image decoder in Python (test/save/svdump.py): unpack(image bytes)
-> the uncompressed state stream. The format is in engine/save/sv.c's
header: the state's size (4 bytes, big-endian), then range-coded LZ
tokens with adaptive 11-bit bit models."""

PBITS, PMOVE, TOP = 11, 5, 1 << 24
PONE = 1 << PBITS
NSTATE, SLOT_BITS, END_SLOT = 12, 5, 14
FOOT_AT = [0, 2, 4, 8, 12, 20, 28, 44, 60, 92]


def _st_lit(s):
    return 0 if s < 4 else s - 3 if s < 10 else s - 6


def unpack(data):
    """The state an image decodes to; ValueError when it is corrupt."""
    if len(data) < 8:
        raise ValueError("image too short")
    size = int.from_bytes(data[0:4], "big")
    rc = {"range": 0xFFFFFFFF, "code": int.from_bytes(data[4:8], "big"), "at": 8}

    def model(n):
        return [PONE // 2] * n

    is_match, is_rep = model(NSTATE), model(NSTATE)
    rep_g0, rep_g1, rep_g2, rep0_long = (model(NSTATE) for _ in range(4))
    lit = model(0x300)
    slot = [model(1 << SLOT_BITS) for _ in range(4)]
    foot, align = model(124), model(16)
    lens = [{"choice": model(1), "choice2": model(1), "low": model(8), "mid": model(8),
             "high": model(256)} for _ in range(2)]

    def norm():
        while rc["range"] < TOP:
            if rc["at"] >= len(data):
                raise ValueError("image ends early")
            rc["range"] = (rc["range"] << 8) & 0xFFFFFFFF
            rc["code"] = ((rc["code"] << 8) | data[rc["at"]]) & 0xFFFFFFFF
            rc["at"] += 1

    def bit(probs, i):
        p = probs[i]
        bound = (rc["range"] >> PBITS) * p
        if rc["code"] < bound:
            rc["range"] = bound
            probs[i] = p + ((PONE - p) >> PMOVE)
            b = 0
        else:
            rc["code"] -= bound
            rc["range"] -= bound
            probs[i] = p - (p >> PMOVE)
            b = 1
        norm()
        return b

    def direct(n):
        v = 0
        for _ in range(n):
            rc["range"] >>= 1
            v <<= 1
            if rc["code"] >= rc["range"]:
                rc["code"] -= rc["range"]
                v |= 1
            norm()
        return v

    def tree(probs, n, base=0):
        m = 1
        for _ in range(n):
            m = m << 1 | bit(probs, base + m)
        return m - (1 << n)

    def rtree(probs, n, base=0):
        m, v = 1, 0
        for i in range(n):
            b = bit(probs, base + m)
            m = m << 1 | b
            v |= b << i
        return v

    def length(lm):
        if not bit(lm["choice"], 0):
            return 2 + tree(lm["low"], 3)
        if not bit(lm["choice2"], 0):
            return 10 + tree(lm["mid"], 3)
        return 18 + tree(lm["high"], 8)

    def distance(ln):
        s = tree(slot[ln - 2 if ln < 5 else 3], SLOT_BITS)
        if s < 4:
            return s + 1
        n = (s >> 1) - 1
        base = (2 | (s & 1)) << n
        if s < END_SLOT:
            return base + rtree(foot, n, FOOT_AT[s - 4]) + 1
        base += direct(n - 4) << 4
        return base + rtree(align, 4) + 1

    out = bytearray()
    reps, st = [1, 1, 1, 1], 0
    while len(out) < size:
        p = len(out)
        if not bit(is_match, st):
            if st >= 7:
                mb, m, same = out[p - reps[0]], 1, True
                for i in range(7, -1, -1):
                    if same:
                        x = mb >> i & 1
                        b = bit(lit, 0x100 + (x << 8) + m)
                        same = b == x
                    else:
                        b = bit(lit, m)
                    m = m << 1 | b
                c = m & 0xFF
            else:
                c = tree(lit, 8)
            out.append(c)
            st = _st_lit(st)
            continue
        if not bit(is_rep, st):
            ln = length(lens[0])
            reps = [distance(ln)] + reps[:3]
            st = 7 if st < 7 else 10
        else:
            if not bit(rep_g0, st):
                i = 0
            elif not bit(rep_g1, st):
                i = 1
            else:
                i = 2 + bit(rep_g2, st)
            if i == 0 and not bit(rep0_long, st):
                ln = 1
                st = 9 if st < 7 else 11
            else:
                ln = length(lens[1])
                st = 8 if st < 7 else 11
            reps = [reps[i]] + reps[:i] + reps[i + 1:]
        d = reps[0]
        if d > p or ln > size - p:
            raise ValueError("bad match")
        for k in range(ln):
            out.append(out[p + k - d])
    if rc["at"] != len(data):
        raise ValueError("bytes after the stream")
    return bytes(out)
