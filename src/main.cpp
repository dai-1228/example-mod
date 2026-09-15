/**
 * Frame Windows (NaNDL Helper) - a Geode mod for Geometry Dash 2.2081.
 *
 * What it does:
 *  - Records every press/release live (time in seconds + 240Hz tick).
 *  - Shows an overlay with the classic 60fps frame-perfect candidate count
 *    (FPLL_GD definition: only ONE 60Hz frame available) alongside the
 *    modern NaNDL-style 240Hz window histogram (0..10, "-" = easy/ignored).
 *  - Computes NaNDL precision (sigma/s for a 24h completion target) with the
 *    exact model from https://nandl.pages.dev (#formula / #calculator tabs).
 *  - Exports upload-ready JSON files (calculator "Import JSON" compatible)
 *    into the mod's save directory. See tools/gdr_to_nandl.py for the
 *    offline equivalent that converts .gdr.json replays (e.g. the bundled
 *    Aeternus.gdr.json) into the same format.
 *
 * Honesty note (same caveat NaN GD gives: frame windows are ~90% manual):
 * true frame windows can only be MEASURED in-game by probing each timing
 * (click-pattern method). This mod records exact input times and flags
 * TIGHT candidates heuristically; treat numeric guesses as a starting
 * point and verify timings before citing them.
 */
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/utils/cocos.hpp>

#include <cmath>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "NandlCalc.hpp"

using namespace geode::prelude;

namespace frame_windows {

// ---------------------------------------------------------------- state

struct InputEvent {
    int index = 0;       // 1-based, presses and releases both count
    double time = 0.0;   // seconds since level start (accumulated in update)
    bool down = true;    // true = press, false = release
    int button = 1;      // GD button id (1 = jump)
    bool p2 = false;
};

struct Recorder {
    PlayLayer* owner = nullptr;
    double elapsed = 0.0;
    std::vector<InputEvent> events;
    CCLabelBMFont* overlay = nullptr;        // "inputs N" header
    CCLabelBMFont* bucketLabels[7]{};        // NaN-style bucket rows
    int buckets[7]{};                        // counts per bucket
    std::string levelName = "unknown";

    void reset(PlayLayer* pl, const std::string& name) {
        owner = pl;
        elapsed = 0.0;
        events.clear();
        levelName = name;
        overlay = nullptr;
        for (int i = 0; i < 7; ++i) {
            bucketLabels[i] = nullptr;
            buckets[i] = 0;
        }
    }
    void clear() {
        owner = nullptr;
        elapsed = 0.0;
        events.clear();
        overlay = nullptr;
        for (int i = 0; i < 7; ++i) {
            bucketLabels[i] = nullptr;
            buckets[i] = 0;
        }
    }
};

static Recorder g_rec;

inline int tickOf(double t) { return static_cast<int>(std::lround(t * 240.0)); }

inline int getInt(const char* key, int fallback) {
    (void)fallback;
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>(key));
}
inline double getDbl(const char* key, double fallback) {
    (void)fallback;
    return Mod::get()->getSettingValue<double>(key);
}
inline bool getBool(const char* key, bool fallback) {
    (void)fallback;
    return Mod::get()->getSettingValue<bool>(key);
}

// ------------------------------------------------- window classification

// Neighbour distance in 240Hz ticks for event i.
static int neighbourDist(const std::vector<InputEvent>& ev, size_t i) {
    int best = INT_MAX;
    if (i > 0) best = std::min(best, tickOf(ev[i].time) - tickOf(ev[i - 1].time));
    if (i + 1 < ev.size()) best = std::min(best, tickOf(ev[i + 1].time) - tickOf(ev[i].time));
    return best;
}

// Classic 60fps frame-perfect candidate (FPLL_GD sense): an action packed
// within one 60Hz frame (<= 4 ticks @240Hz) of its neighbour. This is a
// CANDIDATE flag, not a measurement - see the reddit threads
// "What counts as a frame perfect?" and "Frame perfect counters are very
// flawed": a 60fps FP spans ~1-7 ticks at 240Hz, so 240Hz windows carry
// strictly more information (NaN GD FAQ, nandl.pages.dev/#about).
static int countFp60(const std::vector<InputEvent>& ev, int fp60Ticks) {
    int n = 0;
    for (size_t i = 0; i < ev.size(); ++i) {
        if (neighbourDist(ev, i) <= fp60Ticks) ++n;
    }
    return n;
}

// NaN-style buckets, top-to-bottom exactly like the "NAN COUNTER" overlay:
// 9-12 (purple) / 7-8 (sky) / 5-6 (green) / 4 (white) /
// 3 (yellow) / 2 (orange) / 1 (red). A 0-tick neighbour distance collars
// into "1" (tightest); anything above 12 is easy and ignored.
static int bucketOf(int w) {
    if (w <= 1) return 6;  // "1"
    if (w == 2) return 5;  // "2"
    if (w == 3) return 4;  // "3"
    if (w == 4) return 3;  // "4"
    if (w <= 6) return 2;  // "5-6"
    if (w <= 8) return 1;  // "7-8"
    if (w <= 12) return 0; // "9-12"
    return -1;
}

static const char* bucketRange(int b) {
    switch (b) {
        case 0: return "9-12";
        case 1: return "7-8";
        case 2: return "5-6";
        case 3: return "4";
        case 4: return "3";
        case 5: return "2";
        default: return "1";
    }
}

static ccColor3B bucketColor(int b) {
    switch (b) {
        case 0: return {130, 130, 255}; // 9-12 purple-blue
        case 1: return {100, 200, 255}; // 7-8 sky
        case 2: return {90, 255, 130};  // 5-6 green
        case 3: return {255, 255, 255}; // 4 white
        case 4: return {255, 235, 50};  // 3 yellow
        case 5: return {255, 155, 40};  // 2 orange
        default: return {255, 70, 70};  // 1 red
    }
}

// Heuristic 240Hz window guess from local density: raw neighbour distance
// in ticks. Timings far from any neighbour are easy -> "-" (ignored,
// exactly like the site's "-" rows).
static std::string guessWindow(const std::vector<InputEvent>& ev, size_t i, int cutoff) {
    int d = neighbourDist(ev, i);
    if (d < 0) d = 0;
    if (d > cutoff) return "-";
    return std::to_string(d);
}

static std::map<std::string, int> histogram(const std::vector<InputEvent>& ev, int cutoff) {
    std::map<std::string, int> h;
    for (size_t i = 0; i < ev.size(); ++i) h[guessWindow(ev, i, cutoff)]++;
    return h;
}

static std::vector<nandl::Row> toRows(const std::vector<InputEvent>& ev, int cutoff,
                                      double respawn, bool heuristic) {
    std::vector<nandl::Row> rows;
    rows.reserve(ev.size());
    for (size_t i = 0; i < ev.size(); ++i) {
        nandl::Row r;
        r.input = static_cast<int>(i + 1);
        r.time = ev[i].time + respawn;
        if (heuristic) {
            int d = neighbourDist(ev, i);
            if (d < 0) d = 0;
            if (d <= cutoff) {
                r.ignored = false;
                r.window = d;
            } else {
                r.ignored = true;
            }
        } else {
            r.ignored = true;
        }
        rows.push_back(r);
    }
    return rows;
}

// ------------------------------------------------------------- exporting

static std::string sanitize(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (std::isalnum(static_cast<unsigned char>(ch))) o += ch;
        else if (ch == ' ' || ch == '-' || ch == '_') o += '_';
    }
    if (o.empty()) o = "level";
    if (o.size() > 48) o.resize(48);
    return o;
}

// Builds a payload byte-identical in schema to tools/gdr_to_nandl.py so it
// can be loaded via the site: Calculator tab -> "Import JSON".
static std::string buildNandlJson(const std::vector<InputEvent>& ev, bool heuristic) {
    const double gameFps = getDbl("game-fps", 240.0);
    const double windowFps = getDbl("window-fps", 240.0);
    const double respawn = getDbl("respawn-time", 0.0);
    const int cutoff = getInt("tight-cutoff", 12);

    std::ostringstream o;
    o << "{\n  \"format\": \"nandl-calculator\",\n  \"version\": 1,\n  \"settings\": {\n";
    o << "    \"gameFps\": " << gameFps << ",\n";
    o << "    \"windowFps\": " << windowFps << ",\n";
    o << "    \"respawnSeconds\": " << respawn << ",\n";
    o << "    \"timeUnit\": \"seconds\"\n  },\n  \"frameWindows\": [";
    for (size_t i = 0; i < ev.size(); ++i) {
        o << (i ? ",\n    " : "\n    ");
        o << "{\"input\": " << (i + 1) << ", \"timePosition\": " << ev[i].time
          << ", \"frameWindow\": ";
        if (heuristic) {
            int d = neighbourDist(ev, i);
            if (d < 0) d = 0;
            if (d <= cutoff) o << d;
            else o << "\"-\"";
        } else {
            o << "\"-\"";
        }
        o << "}";
    }
    o << "\n  ]\n}\n";
    return o.str();
}

static std::string exportAll() {
    namespace fs = std::filesystem;
    std::string base = sanitize(g_rec.levelName);
    fs::path dir = Mod::get()->getSaveDir();
    fs::path honest = dir / (base + ".nandl.honest.json");
    fs::path guess = dir / (base + ".nandl.json");
    {
        std::ofstream f(honest.string());
        f << buildNandlJson(g_rec.events, false);
    }
    {
        std::ofstream f(guess.string());
        f << buildNandlJson(g_rec.events, true);
    }
    log::info("[frame-windows] exported {} input(s) for '{}':\n  {}\n  {}",
        g_rec.events.size(), g_rec.levelName, honest.string(), guess.string());
    return honest.string() + "\n" + guess.string();
}

// --------------------------------------------------------------- overlay

static void refreshOverlay() {
    const bool on = getBool("overlay-enabled", true);
    if (g_rec.overlay) {
        g_rec.overlay->setVisible(on);
        if (on) g_rec.overlay->setString(
            fmt::format("inputs {}", g_rec.events.size()).c_str());
    }
    for (int b = 0; b < 7; ++b) {
        if (!g_rec.bucketLabels[b]) continue;
        g_rec.bucketLabels[b]->setVisible(on);
        if (on) g_rec.bucketLabels[b]->setString(
            fmt::format("{}: {}", bucketRange(b), g_rec.buckets[b]).c_str());
    }
}

static void createBucketUI(PlayLayer* pl) {
    auto win = CCDirector::get()->getWinSize();
    auto head = CCLabelBMFont::create("inputs 0", "bigFont.fnt");
    head->setID("fw-overlay"_spr);
    head->setAnchorPoint({0.f, 1.f});
    head->setPosition({10.f, win.height - 10.f});
    head->setScale(0.4f);
    head->setZOrder(1000);
    pl->addChild(head);
    g_rec.overlay = head;
    float y = win.height - 34.f;
    for (int b = 0; b < 7; ++b) {
        auto row = CCLabelBMFont::create(
            fmt::format("{}: 0", bucketRange(b)).c_str(), "bigFont.fnt");
        row->setID(fmt::format("fw-bucket-{}", b));
        row->setAnchorPoint({0.f, 1.f});
        row->setPosition({10.f, y});
        row->setScale(0.62f);
        row->setColor(bucketColor(b));
        row->setZOrder(1000);
        pl->addChild(row);
        g_rec.bucketLabels[b] = row;
        y -= 24.f;
    }
    refreshOverlay();
}

// Ring popup like NaN's videos: bucket-colored ring + window number to its
// left, spawned at the player, pops in, drifts up and vanishes.
static void spawnPopup(int window, int bucket) {
    if (!g_rec.owner || !getBool("nan-popups", true)) return;
    auto pl = g_rec.owner;
    CCPoint at = ccp(0.f, 0.f);
    if (pl->m_player1) at = pl->m_player1->getPosition();

    const ccColor3B col3 = bucketColor(bucket);
    const ccColor4F col4 = ccc4FFromccc3B(col3);

    auto box = CCNode::create();
    box->setID(fmt::format("fw-popup-{}", g_rec.events.size()));
    box->setPosition(at + ccp(-40.f, -50.f));
    box->setZOrder(1000);

    auto ring = CCDrawNode::create();
    constexpr int SEG = 40;
    constexpr float R = 26.f;
    constexpr float TAU = 6.28318530718f;
    for (int i = 0; i < SEG; ++i) {
        float a0 = TAU * i / SEG;
        float a1 = TAU * (i + 1) / SEG;
        ring->drawSegment(
            ccp(R * cosf(a0), R * sinf(a0)),
            ccp(R * cosf(a1), R * sinf(a1)),
            4.f, col4);
    }
    box->addChild(ring);

    auto num = CCLabelBMFont::create(fmt::format("{}", window).c_str(), "bigFont.fnt");
    num->setColor(col3);
    num->setScale(0.9f);
    num->setPosition(ccp(-R - 18.f, 0.f));
    box->addChild(num);

    pl->addChild(box);
    const float dur = static_cast<float>(getDbl("popup-time", 0.9));
    box->setScale(0.5f);
    box->runAction(CCScaleTo::create(0.12f, 1.f));
    box->runAction(CCMoveBy::create(dur, ccp(0.f, 26.f)));
    // NOTE: only the label fades (CCLabelBMFont is RGBA-safe); fading a
    // plain CCNode container with CCFadeOut would be undefined behaviour.
    num->runAction(CCSequence::create(
        CCDelayTime::create(dur * 0.55f), CCFadeOut::create(dur * 0.45f), nullptr));
    box->runAction(CCSequence::create(
        CCDelayTime::create(dur), CCRemoveSelf::create(), nullptr));
}

static void removePopups() {
    if (!g_rec.owner) return;
    auto kids = g_rec.owner->getChildren();
    if (!kids) return;
    std::vector<CCNode*> dead;
    for (auto n : CCArrayExt<CCNode*>(kids)) {
        if (!n) continue;
        std::string id = n->getID();
        if (id.rfind("fw-popup-", 0) == 0) dead.push_back(n);
    }
    for (auto n : dead) n->removeFromParent();
}

// A timing's window is only known once the NEXT action lands (the gap is
// the measurement), so each new input finalizes the previous one.
static void classifyPrev() {
    auto& ev = g_rec.events;
    if (ev.size() >= 2) {
        const int cutoff = getInt("tight-cutoff", 12);
        int d = tickOf(ev.back().time) - tickOf(ev[ev.size() - 2].time);
        if (d < 0) d = 0;
        if (d <= cutoff) {
            int b = bucketOf(d);
            if (b >= 0) {
                g_rec.buckets[b]++;
                spawnPopup(d, b);
            }
        }
    }
    refreshOverlay();
}

static void recordEvent(int button, bool player1, bool down) {
    if (!g_rec.owner) return;
    // player1 == true -> P1; the GJBase hook passes (state, player1)-style
    // args where the bool selects P2 when false on some forks, so we only
    // use it as a label, never to drop events.
    InputEvent e;
    e.index = static_cast<int>(g_rec.events.size() + 1);
    e.time = g_rec.elapsed;
    e.down = down;
    e.button = button;
    e.p2 = !player1;
    g_rec.events.push_back(e);
    classifyPrev();
}

// ----------------------------------------------------------------- hooks

class $modify(FWBaseHook, GJBaseGameLayer) {
    // Unified input entry point in 2.2081 bindings: one hook catches every
    // press AND release, with the GD button id and the P1/P2 flag.
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (g_rec.owner) {
            if (down || getBool("count-releases", true)) recordEvent(button, isPlayer1, down);
        }
    }
};

class $modify(FWPlayHook, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        std::string name = "unknown";
        if (level) name = level->m_levelName.c_str();
        g_rec.reset(this, name);
        createBucketUI(this);
        log::info("[frame-windows] recording '{}'", name);
        return true;
    }
    void update(float dt) {
        PlayLayer::update(dt);
        // update() is not called while paused, so this accumulates level time.
        if (g_rec.owner == this) g_rec.elapsed += dt;
    }
    void resetLevel() {
        PlayLayer::resetLevel();
        // Restart the clock every attempt so timestamps always belong to
        // the current run. By default the event list restarts too (a clean
        // per-attempt recording, like other in-game counters); disable
        // "clear-on-reset" to keep accumulating across attempts.
        if (g_rec.owner == this) {
            g_rec.elapsed = 0.0;
            if (getBool("clear-on-reset", true)) {
                g_rec.events.clear();
                for (int i = 0; i < 7; ++i) g_rec.buckets[i] = 0;
                removePopups();
                refreshOverlay();
            }
        }
    }
    void onQuit() {
        if (g_rec.owner == this && !g_rec.events.empty() && getBool("auto-export", false)) {
            exportAll();
        }
        if (g_rec.owner == this) g_rec.clear();
        PlayLayer::onQuit();
    }
};

class $modify(FWPauseHook, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = typeinfo_cast<CCMenu*>(this->getChildByID("left-button-menu"));
        if (!menu) return;
        auto spr = ButtonSprite::create("FW");
        if (!spr) return;
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FWPauseHook::onFWButton));
        btn->setID("fw-button"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }
    void onFWButton(CCObject*) {
        if (g_rec.events.empty()) {
            FLAlertLayer::create("Frame Windows",
                "No inputs recorded yet.\nPlay the level first, then come back.", "OK")->show();
            return;
        }
        const int fp60 = getInt("fp60-ticks", 4);
        const int cutoff = getInt("tight-cutoff", 12);
        const double windowFps = getDbl("window-fps", 240.0);
        const double respawn = getDbl("respawn-time", 0.0);
        auto hist = histogram(g_rec.events, cutoff);
        int tight = 0;
        for (auto& [k, v] : hist) {
            if (k != "-") tight += v;
        }
        std::string precision = "n/a (all windows '-')";
        try {
            auto rows = toRows(g_rec.events, cutoff, respawn, true);
            nandl::Constants c; // base mode, like the site's default list
            auto sol = nandl::solveSkill(rows, 86400.0, 1.0, 240, windowFps, c);
            precision = fmt::format("{:.1f} s/s ({} attempts est.)", sol.skill, (long long)sol.stats.attempts);
        } catch (const std::exception& e) {
            precision = std::string("n/a (") + e.what() + ")";
        }
        std::string paths;
        try {
            paths = exportAll();
        } catch (const std::exception& e) {
            paths = std::string("export failed: ") + e.what();
        }
        std::string msg = fmt::format(
            "Level: {}\nInputs: {} (press+release)\n"
            "60fps FP candidates: {}\nTight (240Hz guess <= {}): {}\n"
            "Precision est. (24h, base): {}\n\n"
            "Exported upload-ready JSON:\n{}\n\n"
            "Upload: nandl.pages.dev -> Calculator -> Import JSON.\n"
            "Numeric windows are density GUESSES - verify in-game!",
            g_rec.levelName, g_rec.events.size(),
            countFp60(g_rec.events, fp60), cutoff, tight, precision, paths);
        FLAlertLayer::create("Frame Windows", msg.c_str(), "OK")->show();
    }
};

} // namespace frame_windows
