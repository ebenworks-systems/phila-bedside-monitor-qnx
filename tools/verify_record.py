#!/usr/bin/env python3
"""Check a Phila bedside record's hash chain, independently of the C code.

    python3 tools/verify_record.py RECORD.jsonl
    python3 tools/verify_record.py --self-test

Each line is a JSON object whose "hash" is the SHA-256 of the line with the
hash removed (the text up to ',"hash":' closed with '}'), and whose "prev"
is the hash of the line before it (64 zeros for the first). Exit status 0
when every line checks, 3 at the first broken line.

Prototype for a hackathon, not a medical device, not for clinical use.
SPDX-License-Identifier: MIT
"""
import hashlib
import json
import sys

ZERO = "0" * 64
KEY = ',"hash":"'


def check(lines):
    """Returns (number of good lines, error or None)."""
    prev, seq = ZERO, 1
    for n, raw in enumerate(lines, 1):
        line = raw.rstrip("\n")
        at = line.rfind(KEY)
        if at < 0 or not line.endswith('"}'):
            return n - 1, f"line {n}: not a record line"
        body = line[:at] + "}"
        try:
            obj = json.loads(line)
        except ValueError as e:
            return n - 1, f"line {n}: not JSON ({e})"
        if obj.get("seq") != seq:
            return n - 1, f"line {n}: sequence {obj.get('seq')} where {seq} was expected"
        if obj.get("prev") != prev:
            return n - 1, f"line {n}: previous hash does not match"
        digest = hashlib.sha256(body.encode("ascii")).hexdigest()
        if obj.get("hash") != digest:
            return n - 1, f"line {n}: line hash does not match its content"
        prev, seq = digest, seq + 1
    return len(lines), None


def self_test():
    lines, prev = [], ZERO
    for i in range(1, 4):
        body = '{"seq":%d,"mono_ns":%d,"time":"2026-11-03T00:00:0%d.000Z","bed":"BED-01","kind":"event","text":"t%d","prev":"%s"}' % (
            i, i, i, i, prev)
        h = hashlib.sha256(body.encode()).hexdigest()
        lines.append(body[:-1] + ',"hash":"%s"}\n' % h)
        prev = h
    assert check(lines) == (3, None)
    bad = list(lines)
    bad[1] = bad[1].replace('"t2"', '"t9"')
    assert check(bad)[1].startswith("line 2: line hash")
    assert check([lines[0], lines[2]])[1].startswith("line 2: sequence")
    print("verify_record.py: self-test passed")
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 1
    with open(argv[1], encoding="ascii") as f:
        lines = f.readlines()
    good, err = check(lines)
    if err:
        print(f"verify_record.py: {err} ({good} good lines before it)", file=sys.stderr)
        return 3
    print(f"verify_record.py: {good} lines, chain intact")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
