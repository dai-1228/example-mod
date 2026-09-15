# Changelog

## v1.0.0
* Auto-probe: TAS-style trial measurement of true frame windows in-level
  (checkpoint restore + shifted-input binary search, self-test included)
* Live input recorder (press + release, seconds + 240Hz ticks)
* NaN-style bucket counter list (1 / 2 / 3 / 4 / 5-6 / 7-8 / 9-12) with video colors
* Ring + number popup at the player for every tight timing (toggleable)
* Full in-game NaNDL calculator popup: editable rows, FPS/respawn settings,
  solve + fixed modes, nerve/fatigue/CPS constants, JSON import/export
* Pause-menu FW panel: window histogram, NaNDL precision estimate, export
* Export of calculator-compatible JSON (honest + heuristic guess variants)
* `tools/gdr_to_nandl.py`: offline `.gdr.json` replay converter + bundled
  `Aeternus.nandl.json` / `Aeternus.nandl.honest.json` ready to upload
