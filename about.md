# Frame Windows (NaNDL Helper)

Live **60fps frame-perfect candidates** plus modern **NaNDL-style 240Hz frame
windows**, with in-game precision (σ/s) and one-click export to
[nandl.pages.dev](https://nandl.pages.dev) calculator JSON.

## Why two counters?

* **60FP?** — classic definition (FPLL_GD): a timing with only **one 60Hz
  frame** available for a press/release. As
  [r/geometrydash](https://www.reddit.com/r/geometrydash/) threads
  (*"Frame perfect counters are very flawed"*, *"What counts as a frame
  perfect?"*) point out, one 60fps perfect spans ~1–7 ticks at 240Hz, so
  this number is vague on high refresh rates.
* **W<=10** — [NaN GD's](https://nandl.pages.dev/#about) fix: measure the
  **frame window** (available 240Hz ticks, GD's max tick rate) per input,
  bucketed 0–10 (`-` = easy/ignored timing). Strictly more informative.

## Use

* Play — the left side shows NaN's bucket list (`9-12 / 7-8 / 5-6 / 4 / 3 /
  2 / 1`, same colors), and every tight timing spawns its bucket-colored
  ring + window number at the player, just like the Frame Window Counter
  videos.
* Pause → **FW** button — the full calculator, in-game: editable
  frame-window rows (paginated), Game/Window FPS, respawn time,
  seconds-or-frames times, Solve-for-24h / fixed-precision modes,
  nerve/fatigue/CPS constants, JSON import/export, and the required
  precision + attempt stats. Load run pulls in the current recording.
* **Measure** (in the calculator) — auto-probes TRUE windows directly in
  the level: it replays your passing run from checkpoints with each input
  shifted tick-by-tick and binary-searches both sides. Needs a passing
  run; best at stable 240fps.
* Upload — the mod writes two files to its save folder:
  `Level.nandl.honest.json` (times only, fill windows in by hand like NaN
  does) and `Level.nandl.json` (density guesses so the calculator solves
  immediately). On the site: **Calculator → Import JSON**.

Numeric guesses are starting points, **not measurements** — verify tight
timings in-game before citing them.
