#!/usr/bin/env python3
"""
gdr_to_nandl.py - Convert an xdBot-style .gdr.json replay into a
NaNDL calculator JSON file uploadable to https://nandl.pages.dev/#calculator

Usage:
    python3 tools/gdr_to_nandl.py Aeternus.gdr.json -o Aeternus.nandl.json
    python3 tools/gdr_to_nandl.py Aeternus.gdr.json -o honest.json --mode honest
    python3 tools/gdr_to_nandl.py Aeternus.gdr.json -o heuristic.json --mode heuristic

Background (from online research):
  - Traditional "60fps frame perfect" (FPLL_GD method, see "HOW I COUNT FRAME
    PERFECTS" on YouTube): a timing where ONLY ONE 60Hz frame is available
    for a press/release.
  - r/geometrydash critiques ("Frame perfect counters are very flawed",
    "What counts as a frame perfect?"): one 60fps frame-perfect maps to a
    window of ~1-7 ticks at 240Hz (avg ~5 when it is not also a 120fps
    perfect), so the old metric is vague.
  - NaN GD (nandl.pages.dev) fixes this by measuring FRAME WINDOWS at 240Hz
    (GD's max tick rate): N_i = number of 240Hz ticks available to pass
    input i, bucketed 0..10+ ("-" = ignored/easy timing). Precision sigma/s
    (L) is then solved from E[T_C(L)] = 24h under a normal timing-error
    model. See the site's Formula tab.

What this script does:
  - Reads replay frames (240fps xdBot format: inputs[] with {frame, down,
    btn, 2p}) and emits time positions (seconds) for every press/release.
  - --mode honest (default): every row gets frameWindow "-" (ignored).
    This is the HONEST output: true windows can only be measured in-game
    (NaN does it manually with click-pattern probing, self-reported ~90%
    accurate). Import it into the NaNDL calculator, then fill in measured
    windows manually.
  - --mode heuristic: pre-fills GUESSES from local input density
    (min distance to neighbouring actions). These are CANDIDATES ONLY,
    not measurements - verify in-game before treating them as real data.
    Useful as a starting point / demo so the calculator solves immediately.

Output format matches calculator.js normalizeCalculatorImport():
    {
      "format": "nandl-calculator", "version": 1,
      "settings": {"gameFps": 240, "windowFps": 240,
                   "respawnSeconds": 0.0, "timeUnit": "seconds"},
      "frameWindows": [{"input": 1, "timePosition": 0.3333,
                        "frameWindow": 4 | "-"}, ...]
    }
In the calculator UI: Calculator tab -> "Import JSON" -> pick this file.
"""
import argparse
import json
import math
import sys
from collections import Counter

FORMAT = "nandl-calculator"
VERSION = 1
TIGHT_CUTOFF_TICKS = 12  # neighbour distance at/below this gets a numeric guess
FP60_TICKS = 4           # 240/60: one 60fps frame == 4 ticks at 240Hz


def load_replay(path):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    inputs = data.get("inputs", [])
    framerate = float(data.get("framerate", 240.0))
    level = data.get("level", {})
    name = level.get("name", "?") if isinstance(level, dict) else str(level)
    return data, inputs, framerate, name


def heuristic_window(dist_ticks):
    """Map min neighbour distance (ticks @240Hz) to a window guess.

    Rapid re-actions imply a tight timing; wide spacing implies an easy
    (ignored) timing. Returns the raw tick distance so NaN buckets map
    1:1 (1 / 2 / 3 / 4 / 5-6 / 7-8 / 9-12). This is only a density proxy
    - NOT a measurement.
    """
    if dist_ticks is None or dist_ticks > TIGHT_CUTOFF_TICKS:
        return "-"
    return max(0, int(dist_ticks))


def convert(inputs, framerate, mode, game_fps=240.0, window_fps=240.0,
            respawn=0.0):
    frames = [int(ev["frame"]) for ev in inputs]
    rows = []
    fp60_candidates = 0
    hist = Counter()
    n = len(frames)
    for idx, frame in enumerate(frames):
        t_sec = frame / framerate
        if mode == "heuristic":
            dists = []
            if idx > 0:
                dists.append(frame - frames[idx - 1])
            if idx + 1 < n:
                dists.append(frames[idx + 1] - frame)
            d = min(dists) if dists else None
            window = heuristic_window(d)
            if d is not None and d <= FP60_TICKS:
                fp60_candidates += 1
        else:
            window = "-"
            d = None
            if idx > 0 and (frame - frames[idx - 1]) <= FP60_TICKS:
                fp60_candidates += 1
            elif idx + 1 < n and (frames[idx + 1] - frame) <= FP60_TICKS:
                fp60_candidates += 1
        hist[window] += 1
        rows.append({
            "input": idx + 1,
            "timePosition": t_sec,
            "frameWindow": window,
        })
    payload = {
        "format": FORMAT,
        "version": VERSION,
        "settings": {
            "gameFps": game_fps,
            "windowFps": window_fps,
            "respawnSeconds": respawn,
            "timeUnit": "seconds",
        },
        "frameWindows": rows,
    }
    return payload, fp60_candidates, hist


# --- Minimal port of nandl.pages.dev/calculator.js for preview stats ---
def _phi_two_sided(k):
    return math.erf(k / math.sqrt(2.0)) if k > 0 else 0.0


def compute_stats(times, values, skill, tps=240.0, kt=0.0, ku=0.0, kc=0.0):
    max_frames = 0
    for v in values:
        if v is not None:
            max_frames = max(max_frames, 1 if v == 0 else v)
    probs = []
    prev_t, prev_i = 0.0, 0
    for n_i, (t, v) in enumerate(zip(times, values), start=1):
        nerve = math.exp(-kt * t)
        fatigue = math.exp(-ku * n_i)
        dt = (t - prev_t) / (n_i - prev_i) if (n_i - prev_i) else 1.0
        if not dt:
            dt = 1.0
        cps_m = 1.0 / (0.25 ** kc * max(1.0, (2.0 / dt) ** kc)) if kc else 1.0
        mult = nerve * fatigue * cps_m
        if v is None:
            k_raw = None if kc <= 0 else max(((max_frames + 1) / 2) * (skill / tps), 0.0)
        else:
            k_raw = max(((1 if v == 0 else v) / 2) * (skill / tps), 0.0)
        p = 1.0 if k_raw is None else min(1.0, max(0.0, _phi_two_sided(k_raw * mult)))
        probs.append(p)
        prev_t, prev_i = t, n_i
    e_att, s_prob = 0.0, 1.0
    for t, p in zip(times, probs):
        e_att += t * s_prob * (1.0 - p)
        s_prob *= p
    e_att += times[-1] * s_prob
    return {
        "per_attempt": e_att,
        "success": s_prob,
        "attempts": (1.0 / s_prob) if s_prob > 0 else float("inf"),
        "total": (e_att / s_prob) if s_prob > 0 else float("inf"),
    }


def solve_skill(times, values, target=86400.0, tol=1.0, iters=240, tps=240.0):
    if target <= times[-1]:
        raise ValueError("target must exceed final input time")
    if all(v is None for v in values):
        raise ValueError("all windows ignored - nothing to solve")
    lo, hi = 0.0, 100.0
    hi_s = compute_stats(times, values, hi, tps)
    grow = 0
    while (not math.isfinite(hi_s["total"]) or hi_s["total"] > target) and grow < 128:
        hi *= 2
        hi_s = compute_stats(times, values, hi, tps)
        grow += 1
    best, best_s = hi, hi_s
    best_d = abs(hi_s["total"] - target)
    for _ in range(iters):
        if best_d <= tol:
            break
        mid = (lo + hi) / 2
        s = compute_stats(times, values, mid, tps)
        d = abs(s["total"] - target)
        if d < best_d:
            best, best_s, best_d = mid, s, d
        if not math.isfinite(s["total"]) or s["total"] > target:
            lo = mid
        else:
            hi = mid
    return best, best_s


def main(argv=None):
    ap = argparse.ArgumentParser(description="Convert .gdr.json replay to NaNDL calculator JSON")
    ap.add_argument("input", help=".gdr.json replay file (e.g. Aeternus.gdr.json)")
    ap.add_argument("-o", "--output", required=True, help="output .json path")
    ap.add_argument("--mode", choices=["honest", "heuristic"], default="honest",
                    help="'honest' = all windows '-' (default); 'heuristic' = density guesses")
    ap.add_argument("--game-fps", type=float, default=240.0)
    ap.add_argument("--window-fps", type=float, default=240.0)
    ap.add_argument("--respawn", type=float, default=0.0)
    ap.add_argument("--no-stats", action="store_true", help="skip precision preview")
    args = ap.parse_args(argv)

    _, inputs, framerate, name = load_replay(args.input)
    if not inputs:
        print("error: replay has no inputs", file=sys.stderr)
        return 1
    payload, fp60, hist = convert(inputs, framerate, args.mode,
                                  args.game_fps, args.window_fps, args.respawn)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)

    print(f"level: {name}  (replay {framerate:g}fps, {len(inputs)} input events)")
    print(f"wrote: {args.output}  [mode={args.mode}]")
    print(f"60fps frame-perfect CANDIDATES (neighbour gap <= {FP60_TICKS} ticks): {fp60}")
    print("window histogram:", dict(sorted(hist.items(), key=lambda kv: str(kv[0]))))
    print("upload: open https://nandl.pages.dev/#calculator -> Import JSON -> pick this file")
    if args.mode == "heuristic":
        print("NOTE: numeric windows are density GUESSES, not measurements. Verify in-game.")
    else:
        print("NOTE: all windows are '-' - fill in measured 240Hz windows in the calculator.")

    if not args.no_stats and args.mode == "heuristic":
        times = [r["timePosition"] for r in payload["frameWindows"]]
        values = [None if r["frameWindow"] == "-" else r["frameWindow"]
                  for r in payload["frameWindows"]]
        try:
            skill, stats = solve_skill(times, values)
            print(f"preview precision (base, 24h target): {skill:.2f} s/s")
            print(f"  P(success)={stats['success']:.4e}  E[attempts]={stats['attempts']:.0f}")
        except ValueError as e:
            print(f"preview precision: n/a ({e})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
