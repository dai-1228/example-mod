# Frame Windows (NaNDL Helper)

A Geode mod for Geometry Dash 2.2081 that counts **frame perfects the old
way and the new way**: classic 60fps frame-perfect candidates (FPLL_GD
definition) plus [NaN GD](https://nandl.pages.dev/#about)-style **240Hz
frame windows** with a live precision (σ/s) estimate.

<img src="logo.png" width="150" alt="the mod's logo" />

*Update logo.png to change your mod's icon (please)*

## Background

* Traditional counters mark a timing "frame perfect" when only **one 60Hz
  frame** is available (see *"HOW I COUNT FRAME PERFECTS"*). Per
  r/geometrydash (*"Frame perfect counters are very flawed"*, *"What counts
  as a frame perfect?"*), that maps to a window of ~1–7 ticks at 240Hz —
  vague for high-refresh players.
* [NaNDL](https://nandl.pages.dev/) instead measures **frame windows 0–10
  at 240Hz** and solves the precision `L` with `E[T_C(L)] = 24h` under a
  normal timing-error model (see the site's Formula tab). This mod ports
  that exact model to C++ (`src/NandlCalc.hpp`, mirroring
  `calculator.js`) so in-game numbers match the website.

## Build instructions

```sh
# Assuming you have the Geode CLI set up already
geode build
```

## Offline replay converter (no game needed)

`tools/gdr_to_nandl.py` converts xdBot-style `.gdr.json` replays into
calculator-ready JSON:

```sh
# Honest output: times only, windows '-' (fill in by hand like NaN does)
python3 tools/gdr_to_nandl.py Aeternus.gdr.json -o Aeternus.nandl.honest.json --mode honest
# Demo output: density-based window guesses so the calculator solves at once
python3 tools/gdr_to_nandl.py Aeternus.gdr.json -o Aeternus.nandl.json --mode heuristic
```

Then open https://nandl.pages.dev/#calculator → **Import JSON** → pick the
file. Pre-generated outputs (`Aeternus.nandl.json`,
`Aeternus.nandl.honest.json`) are included in the repo root.

## In-game use

1. Play any level — the overlay shows `Inputs | 60FP? | W<=10`.
2. Pause and press **FW** for the histogram, precision estimate, and export.
3. Find the exported files in the mod's save directory (path is shown in
   the popup and the Geode log) and import them into the site calculator.

Numeric window guesses are starting points, **not measurements** — verify
tight timings in-game before citing them.

## Resources
* [NaNDL — frame-window list, formula, calculator](https://nandl.pages.dev/)
* [Geode SDK Documentation](https://docs.geode-sdk.org/)
* [Geode SDK Source Code](https://github.com/geode-sdk/geode/)
* [Geode CLI](https://github.com/geode-sdk/cli)
* [Bindings](https://github.com/geode-sdk/bindings/)
* [Dev Tools](https://github.com/geode-sdk/DevTools)
