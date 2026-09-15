// NandlCalc.hpp - header-only port of the NaNDL precision model.
// Mirrors https://nandl.pages.dev/calculator.js so the numbers shown
// in-game match the website's Calculator tab exactly (base mode:
// kt = ku = kc = 0, nerve/fatigue/CPS multipliers supported too).
//
// Model recap:
//   w_i = N_i / f                      time window of input i (seconds)
//   s_i = w_i * L / 2                  sigma value for input i
//   p_i = P(|X| <= s_i * mult), X~N(0,1)  pass probability per input
//   P(C) = prod p_i                    run success probability
//   E[T_A] = t_n*P(C) + sum t_i*r_i*q_i  expected time per attempt
//   E[T_C] = E[T_A] / P(C)              expected time to completion
//   L* solves E[T_C(L*)] = target (default 24h = 86400s)
#pragma once

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace nandl {

struct Row {
    int input = 0;        // 1-based input number
    double time = 0.0;    // time position in seconds (respawn already added)
    double window = 0.0;  // frame window in ticks (at windowFps)
    bool ignored = true;  // "-" row: ignored unless CPS constant is on
};

struct Constants {
    double kt = 0.0; // nerve
    double ku = 0.0; // fatigue
    double kc = 0.0; // CPS (WIP on the site too)
};

struct Stats {
    double perAttempt = 0.0;
    double success = 0.0;
    double attempts = 0.0;
    double total = 0.0;
};

struct Solution {
    double skill = 0.0;
    Stats stats;
    double error = 0.0;
};

inline double twoSided(double k) {
    if (k <= 0.0) return 0.0;
    static constexpr double SQRT2 = 1.4142135623730951;
    return std::erf(k / SQRT2);
}

inline Stats computeStats(const std::vector<Row>& rows, double skill,
                          double tps, const Constants& c) {
    if (!(skill >= 0.0)) throw std::invalid_argument("Precision must be >= 0.");
    if (rows.empty()) throw std::invalid_argument("No input rows.");
    if (!(tps > 0.0)) throw std::invalid_argument("Window FPS must be > 0.");

    double maxFrames = 0.0;
    for (auto& r : rows) {
        if (!r.ignored) maxFrames = std::max(maxFrames, r.window == 0.0 ? 1.0 : r.window);
    }

    std::vector<double> probs;
    probs.reserve(rows.size());
    double prevTime = 0.0;
    int prevInput = 0;
    for (auto& r : rows) {
        const double nerve = std::exp(-c.kt * r.time);
        const double fatigue = std::exp(-c.ku * r.input);
        double dt = 1.0;
        if (r.input - prevInput != 0) {
            dt = (r.time - prevTime) / (r.input - prevInput);
            if (dt == 0.0) dt = 1.0;
        }
        // CPS multiplier, exactly as on the site:
        //   1 / (0.25^kc * max(1, (2/dt)^kc))
        const double cps = (c.kc == 0.0)
            ? 1.0
            : 1.0 / (std::pow(0.25, c.kc) * std::max(1.0, std::pow(2.0 / dt, c.kc)));
        const double mult = nerve * fatigue * cps;

        double kRaw;
        if (r.ignored) {
            if (c.kc <= 0.0) {
                probs.push_back(1.0);
                prevInput = r.input;
                prevTime = r.time;
                continue;
            }
            kRaw = std::max(((maxFrames + 1.0) / 2.0) * (skill / tps), 0.0);
        } else {
            const double v = r.window == 0.0 ? 1.0 : r.window;
            kRaw = std::max((v / 2.0) * (skill / tps), 0.0);
        }
        double p = twoSided(kRaw * mult);
        p = std::min(1.0, std::max(0.0, p));
        probs.push_back(p);
        prevInput = r.input;
        prevTime = r.time;
    }

    double perAttempt = 0.0;
    double success = 1.0;
    for (size_t i = 0; i < rows.size(); ++i) {
        perAttempt += rows[i].time * success * (1.0 - probs[i]);
        success *= probs[i];
    }
    perAttempt += rows.back().time * success;

    Stats s;
    s.perAttempt = perAttempt;
    s.success = success;
    s.attempts = success > 0.0 ? 1.0 / success : INFINITY;
    s.total = success > 0.0 ? perAttempt / success : INFINITY;
    return s;
}

inline Solution solveSkill(const std::vector<Row>& rows, double target,
                           double tolerance, int maxIter,
                           double tps, const Constants& c) {
    if (!(target > rows.back().time)) {
        throw std::invalid_argument("Target time must exceed the final input time.");
    }
    bool allIgnored = true;
    for (auto& r : rows) allIgnored = allIgnored && r.ignored;
    if (allIgnored) {
        throw std::invalid_argument("Every frame window is ignored; nothing to solve.");
    }
    auto dist = [&](double v) {
        return std::isfinite(v) ? std::abs(v - target) : INFINITY;
    };
    double lo = 0.0, hi = 100.0;
    Stats hiStats = computeStats(rows, hi, tps, c);
    int grown = 0;
    while ((!std::isfinite(hiStats.total) || hiStats.total > target) && grown < 128) {
        hi *= 2.0;
        hiStats = computeStats(rows, hi, tps, c);
        ++grown;
    }
    if (!std::isfinite(hiStats.total) || hiStats.total > target) {
        throw std::runtime_error("Solver could not find a precision range.");
    }
    double best = hi, bestDist = dist(hiStats.total);
    Stats bestStats = hiStats;
    for (int i = 0; i < maxIter && bestDist > tolerance; ++i) {
        const double mid = (lo + hi) / 2.0;
        Stats s = computeStats(rows, mid, tps, c);
        const double d = dist(s.total);
        if (d < bestDist) {
            best = mid;
            bestStats = s;
            bestDist = d;
        }
        if (!std::isfinite(s.total) || s.total > target) lo = mid;
        else hi = mid;
    }
    return {best, bestStats, bestDist};
}

} // namespace nandl
