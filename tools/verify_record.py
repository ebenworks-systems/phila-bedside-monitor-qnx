#!/usr/bin/env python3
"""Check a Phila bedside record's hash chain, independently of the C code.

    python3 tools/verify_record.py [--head SEQ HASH] RECORD.jsonl
    python3 tools/verify_record.py --self-test

Each line is a JSON object whose "hash" is the SHA-256 of the line with the
hash removed (the text up to ',"hash":' closed with '}'), and whose "prev"
is the hash of the line before it (64 zeros for the first). Exit status 0
when every line checks, 3 at the first broken line.

The chain alone finds a changed, removed or reordered line, unless whoever
changed it also rewrote every hash after it. The monitor logs the chain's
head ("record head seq=N hash=...") to the system log once a second and
prints it when it stops; --head checks that line SEQ of the file still has
that hash, which a rewritten or shortened file does not.

Prototype for a hackathon, not a medical device, not for clinical use.
SPDX-License-Identifier: MIT
"""
import hashlib
import json
import sys

ZERO = "0" * 64
KEY = ',"hash":"'


def check(lines, hashes=None):
    """Returns (number of good lines, error or None). Appends each good
    line's hash to hashes when a list is given."""
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
        if hashes is not None:
            hashes.append(digest)
    return len(lines), None


def check_head(hashes, seq, digest):
    """Error or None: does the good chain still reach line seq with that hash?"""
    if seq < 1:
        return "the head's sequence number must be 1 or more"
    if len(hashes) < seq:
        return f"the record has {len(hashes)} lines, but the logged head is line {seq}: it was cut short"
    if hashes[seq - 1] != digest.lower():
        return f"line {seq} does not have the logged head's hash: the record was rewritten"
    return None


def make_lines(texts):
    lines, prev = [], ZERO
    for i, text in enumerate(texts, 1):
        body = '{"seq":%d,"mono_ns":%d,"time":"2026-11-03T00:00:0%d.000Z","bed":"BED-01","kind":"event","text":"%s","prev":"%s"}' % (
            i, i, i, text, prev)
        h = hashlib.sha256(body.encode()).hexdigest()
        lines.append(body[:-1] + ',"hash":"%s"}\n' % h)
        prev = h
    return lines


def self_test():
    lines = make_lines(["t1", "t2", "t3"])
    hashes = []
    assert check(lines, hashes) == (3, None)
    head = hashes[2]
    assert check_head(hashes, 3, head) is None
    assert check_head(hashes, 2, hashes[1]) is None  # an older head still matches a longer file
    bad = list(lines)
    bad[1] = bad[1].replace('"t2"', '"t9"')
    assert check(bad)[1].startswith("line 2: line hash")
    assert check([lines[0], lines[2]])[1].startswith("line 2: sequence")
    # Rewriting every hash after the change passes the chain but not the head.
    forged, fh = make_lines(["t1", "t9", "t3"]), []
    assert check(forged, fh) == (3, None)
    assert "rewritten" in check_head(fh, 3, head)
    # Dropping the last line passes the chain but not the head.
    short = []
    assert check(lines[:2], short) == (2, None)
    assert "cut short" in check_head(short, 3, head)
    print("verify_record.py: self-test passed")
    return 0


def main(argv):
    args = argv[1:]
    if args == ["--self-test"]:
        return self_test()
    head = None
    if len(args) == 4 and args[0] == "--head":
        try:
            head = (int(args[1]), args[2])
        except ValueError:
            head = None
        args = args[3:] if head else []
    if len(args) != 1:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 1
    with open(args[0], encoding="ascii") as f:
        lines = f.readlines()
    hashes = []
    good, err = check(lines, hashes)
    if err:
        print(f"verify_record.py: {err} ({good} good lines before it)", file=sys.stderr)
        return 3
    if head:
        err = check_head(hashes, *head)
        if err:
            print(f"verify_record.py: {err}", file=sys.stderr)
            return 3
        print(f"verify_record.py: {good} lines, chain intact, line {head[0]} matches the logged head")
        return 0
    print(f"verify_record.py: {good} lines, chain intact")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
