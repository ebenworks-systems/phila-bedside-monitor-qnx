#!/usr/bin/env python3
"""Summarise the latency cases fetched by scripts/latency-runs.sh.

    python3 tools/latency_report.py results/latency [--chart docs/latency.png]

Prints a Markdown table per span (total, timer wake, alarm pipeline) for the
clinical alarms of every case found, plus the technical alarms, missed sensor
deadlines, the host's load, how long the host kept the VM's virtual CPUs
waiting, whether the monitor's own summary counted the same alarms as
latency.csv, the timer wake of every ten seconds for a case whose host.txt
names stalls (scripts/stall-test.sh), and the guest's clock tick, and
optionally draws a chart on a white background. Only numbers read from the
files are printed.

Prototype for a hackathon, not a medical device, not for clinical use.
SPDX-License-Identifier: MIT
"""
import csv
import os
import re
import statistics
import sys

ORDER = ["designed-idle", "designed-tick-idle", "designed-hog35", "designed-hog10", "naive-hog10", "naive-hog35"]
LABELS = {
    "designed-idle": "Designed, no load",
    "designed-tick-idle": "Designed, tick timers, no load",
    "designed-hog35": "Designed, hog FIFO 35",
    "designed-hog10": "Designed, hog RR 10",
    "naive-hog10": "Naive, hog RR 10",
    "naive-hog35": "Naive, hog FIFO 35",
    "before-fix": "Before the fix: periodic timer, two host stalls",
    "after-fix": "After the fix: one-shot timer, two host stalls",
}
CLINICAL = {"alarm", "escalate"}


def pct(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    return sorted_vals[int((len(sorted_vals) - 1) * p)]


def load_case(path):
    rows = []
    with open(os.path.join(path, "latency.csv"), newline="") as f:
        for r in csv.DictReader(f):
            sched, sample, raised = int(r["sched_ns"]), int(r["sample_ns"]), int(r["raised_ns"])
            rows.append((r["kind"], (raised - sched) / 1000, (sample - sched) / 1000, (raised - sample) / 1000, sched))
    info = {"missed": None, "hog": "", "qtime": None, "summary": (None, None)}
    try:
        log = open(os.path.join(path, "monitor.log")).read()
        m = re.search(r"(\d+) missed sensor deadlines", log)
        info["missed"] = int(m.group(1)) if m else None
        clin = re.search(r"clinical alarm latency over (\d+) alarms", log)
        tech = re.search(r"technical alarms (\d+)", log)
        info["summary"] = (int(clin.group(1)) if clin else None, int(tech.group(1)) if tech else None)
    except OSError:
        pass
    try:
        info["hog"] = open(os.path.join(path, "hog.log")).read().strip().splitlines()[0]
    except (OSError, IndexError):
        pass
    host, stalls = {}, []
    try:
        for line in open(os.path.join(path, "host.txt")):
            k, _, v = line.partition(":")
            if k.strip() == "stall":
                stalls.append(v.strip())
            else:
                host[k.strip()] = v.strip()
    except OSError:
        pass
    info["host"], info["stalls"] = host, stalls
    try:
        q = open(os.path.join(path, "qtime.txt")).read()
        inc, cps = re.search(r"nsec_inc:([0-9a-fA-F]+)", q), re.search(r"CPS:([0-9a-fA-F]+)", q)
        if inc and cps and int(inc.group(1), 16) and int(cps.group(1), 16):
            info["qtime"] = (int(inc.group(1), 16), int(cps.group(1), 16))
    except OSError:
        pass
    return rows, info


def vcpu_wait(host):
    """The host's run-queue wait of the VM's virtual CPUs over the case: the total,
    how many times they were put on a processor, and the mean wait each time.
    No share is given: their run time includes KVM's halt polling when the guest
    idles and none when a hog keeps it busy, so no share compares the cases."""
    try:
        wait = float(host["vcpu wait ms"])
        secs, k, times = float(host["case seconds"]), int(host["vcpu threads"]), int(host["vcpu times run"])
    except (KeyError, ValueError):
        return "-", "-", "-"
    if k <= 0 or secs <= 0 or times <= 0 or wait < 0:
        return "-", "-", "-"
    return f"{wait:,.1f} ms over {secs:.0f} s, {k} virtual CPUs", f"{times:,}", f"{wait * 1000 / times:,.1f} µs"


def fmt(v):
    if v != v:  # nan
        return "-"
    if v >= 10000:
        return f"{v / 1000:,.1f} ms"
    return f"{v:,.1f} µs"


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 1
    root = argv[1]
    chart = argv[argv.index("--chart") + 1] if "--chart" in argv else None
    found = sorted(d for d in (os.listdir(root) if os.path.isdir(root) else [])
                   if os.path.isfile(os.path.join(root, d, "latency.csv")))
    cases = [c for c in ORDER if c in found] + [c for c in found if c not in ORDER]
    for c in cases:
        LABELS.setdefault(c, c)
    if not cases:
        print(f"no cases with latency.csv under {root}", file=sys.stderr)
        return 1
    data = {c: load_case(os.path.join(root, c)) for c in cases}

    spans = [("total", 1, "Deadline of the reading to the alarm on the board"),
             ("timer wake", 2, "Deadline to the sensor thread running"),
             ("alarm pipeline", 3, "Sensor thread running to the alarm on the board")]
    for title, idx, what in spans:
        print(f"\n**Clinical alarms, {title}** ({what})\n")
        print("| Case | Alarms | Min | Median | 99th percentile | 99.9th percentile | Max |")
        print("|:---|---:|---:|---:|---:|---:|---:|")
        for c in cases:
            v = sorted(r[idx] for r in data[c][0] if r[0] in CLINICAL)
            print(f"| {LABELS[c]} | {len(v)} | {fmt(pct(v, 0))} | {fmt(pct(v, 0.5))} | {fmt(pct(v, 0.99))} | "
                  f"{fmt(pct(v, 0.999))} | {fmt(pct(v, 1))} |")

    print("\n**Each run**\n")
    print("| Case | Technical alarms | Missed sensor deadlines | Hog | Started | Host load before | Host load after |")
    print("|:---|---:|---:|:---|:---|:---|:---|")
    for c in cases:
        rows, info = data[c]
        tech = sum(1 for r in rows if r[0] == "technical")
        hog = info["hog"].replace("phila-hog: ", "") or "none"
        missed = info["missed"] if info["missed"] is not None else "-"
        h = info["host"]
        print(f"| {LABELS[c]} | {tech} | {missed} | {hog} | {h.get('started', '-')} | "
              f"{h.get('host load before', '-')} | {h.get('host load after', '-')} |")

    print("\n**Timer phase** (the median timer wake of each run of 100 clinical alarms, about ten seconds; "
          "a lasting shift means the sensor timer kept a late phase)\n")
    print("| Case | Lowest median | Highest median | Medians over 1 ms | Readings taken before their deadline | Earliest |")
    print("|:---|---:|---:|---:|---:|---:|")
    for c in cases:
        clin = sorted((r for r in data[c][0] if r[0] in CLINICAL), key=lambda r: r[4])
        wake = [r[2] for r in clin]
        meds = [statistics.median(wake[i:i + 100]) for i in range(0, len(wake) - 99, 100)]
        early = [w for w in wake if w < 0]
        lo, hi = (fmt(min(meds)), fmt(max(meds))) if meds else ("-", "-")
        over = f"{sum(m > 1000 for m in meds)} of {len(meds)}" if meds else "-"
        print(f"| {LABELS[c]} | {lo} | {hi} | {over} | {len(early)} | {fmt(min(early)) if early else '-'} |")

    print("\n**Alarms counted** (the monitor's own summary in monitor.log against the rows of latency.csv)\n")
    print("| Case | Clinical, summary | Clinical, latency.csv | Technical, summary | Technical, latency.csv | Same |")
    print("|:---|---:|---:|---:|---:|:---|")
    for c in cases:
        rows, info = data[c]
        clin_csv = sum(1 for r in rows if r[0] in CLINICAL)
        tech_csv = sum(1 for r in rows if r[0] == "technical")
        clin_log, tech_log = info["summary"]
        same = "yes" if (clin_log, tech_log) == (clin_csv, tech_csv) else "no"
        show = lambda v: "-" if v is None else f"{v:,}"
        print(f"| {LABELS[c]} | {show(clin_log)} | {clin_csv:,} | {show(tech_log)} | {tech_csv:,} | {same} |")

    for c in cases:
        if not data[c][1]["stalls"]:
            continue
        clin = sorted((r for r in data[c][0] if r[0] in CLINICAL), key=lambda r: r[4])
        print(f"\n**Timer wake every ten seconds, {LABELS[c]}** (the clinical alarms whose reading was due in "
              f"each ten seconds, counted from the first alarm's deadline, which comes about 1.5 s after the start "
              f"the stall times count from)\n")
        for st in data[c][1]["stalls"]:
            print(f"- Host stall: {st}")
        print("\n| Seconds | Alarms | Median wake | Max wake | Readings taken before their deadline |")
        print("|:---|---:|---:|---:|---:|")
        t0 = clin[0][4] if clin else 0
        windows = {}
        for r in clin:
            windows.setdefault(int((r[4] - t0) // 10_000_000_000), []).append(r[2])
        for i in sorted(windows):
            wake = windows[i]
            print(f"| {i * 10} to {i * 10 + 10} | {len(wake)} | {fmt(statistics.median(wake))} | {fmt(max(wake))} | "
                  f"{sum(1 for x in wake if x < 0)} |")

    print("\n**The host's hold on the virtual machine** (run-queue wait of the VM's virtual CPU threads during each case)\n")
    print("| Case | Host kept the virtual CPUs waiting | Times put on a processor | Mean wait each time |")
    print("|:---|---:|---:|---:|")
    for c in cases:
        total, times, mean = vcpu_wait(data[c][1]["host"])
        print(f"| {LABELS[c]} | {total} | {times} | {mean} |")

    ticks = {}
    for c in cases:
        if data[c][1]["qtime"]:
            ticks.setdefault(data[c][1]["qtime"], []).append(LABELS[c])
    for (inc, cps), names in sorted(ticks.items()):
        which = "every case" if len(names) == len(cases) else ", ".join(names)
        print(f"\nGuest clock (pidin syspage=qtime, {which}): tick {inc / 1e6:g} ms (nsec_inc {inc} ns); "
              f"ClockCycles() at {cps:,} per second, {1e9 / cps:.2f} ns a cycle.")

    if chart:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(10, 4.8), dpi=150)
        fig.patch.set_facecolor("white")
        ax.set_facecolor("white")
        series = [[max(r[1], 1.0) for r in data[c][0] if r[0] in CLINICAL] for c in cases]
        ax.boxplot(series, vert=False, whis=(0, 100), widths=0.55,
                   medianprops={"color": "#c62828", "linewidth": 2},
                   boxprops={"color": "#37474f"}, whiskerprops={"color": "#37474f"}, capprops={"color": "#37474f"})
        def label(c):
            total = vcpu_wait(data[c][1]["host"])[0]
            return LABELS[c] if total == "-" else f"{LABELS[c]}\nhost held the VM {total.split(' over ')[0]}"
        ax.set_yticks(range(1, len(cases) + 1), [label(c) for c in cases], fontsize=8)
        ax.set_xscale("log")
        ax.set_xlabel("Deadline of the sensor reading to the alarm on the board (microseconds, log scale; whiskers are min "
                      "and max;\nanything under 1 µs, including a reading taken before its deadline, is drawn at 1 µs)")
        ax.invert_yaxis()
        ax.grid(axis="x", color="#e0e0e0")
        ax.set_title("Alarm latency on QNX 8.0 in QEMU, per case", loc="left")
        fig.tight_layout()
        fig.savefig(chart, facecolor="white")
        print(f"\n(chart written to {chart})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
