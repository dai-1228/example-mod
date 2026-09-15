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
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/binding/CCMenuItemToggler.hpp>
#include <Geode/utils/cocos.hpp>
#include <matjson.hpp>

#include <cmath>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>
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

// ------------------------------------------------- in-game calculator
// Full port of https://nandl.pages.dev/#calculator : editable frame-window
// rows, Game/Window FPS, respawn time, frame-vs-seconds times, solve-for-
// target / fixed-precision modes, nerve/fatigue/CPS constants, JSON import
// (from the mod save dir) and export (uploadable to the site).

struct CalcRow {
    int input = 1;
    double time = 0.0; // always seconds canonically
    std::string window = "-";
    bool down = true;  // press (true) or release (false); needed by auto-probe
    int button = 1;
    bool p2 = false;
};

static std::string fmtNum(double v) {
    if (!std::isfinite(v)) return "";
    return fmt::format("{:.10g}", v);
}

// Calculator state shared across popup instances (and the auto-prober),
// so closing the popup never loses rows or settings.
struct CalcDoc {
    std::vector<CalcRow> rows;
    double gameFps = 240.0;
    double windowFps = 240.0;
    double respawn = 0.0;
    bool useFrames = false;
    bool solve = true;
    double target = 86400.0;
    double tol = 1.0;
    int iters = 240;
    double skill = 100.0;
    double kt = 0.0016520833717346;
    double ku = 0.0002727763242154;
    double kc = 0.2784421686721826;
    bool ktOn = false, kuOn = false, kcOn = false;
};
static CalcDoc g_doc;

class CalcPopup : public Popup {
public:
    static constexpr int ROWS_PER_PAGE = 6;
    static constexpr double DEF_KT = 0.0016520833717346;
    static constexpr double DEF_KU = 0.0002727763242154;
    static constexpr double DEF_KC = 0.2784421686721826;

    // state (mirrors the site's calculator fields)
    double m_gameFps = 240.0;
    double m_windowFps = 240.0;
    double m_respawn = 0.0;
    bool m_useFrames = false;
    bool m_solve = true;
    double m_target = 86400.0;
    double m_tol = 1.0;
    int m_iters = 240;
    double m_skill = 100.0;
    double m_kt = DEF_KT, m_ku = DEF_KU, m_kc = DEF_KC;
    bool m_ktOn = false, m_kuOn = false, m_kcOn = false;
    std::vector<CalcRow> m_rows;
    int m_page = 0;

    // widgets
    CCMenu* m_rowsBox = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCLabelBMFont* m_resultMain = nullptr;
    CCLabelBMFont* m_resultSub = nullptr;
    CCLabelBMFont* m_msg = nullptr;
    CCMenuItemSpriteExtra* m_modeBtn = nullptr;
    TextInput* m_gameFpsIn = nullptr;
    TextInput* m_windowFpsIn = nullptr;
    TextInput* m_respawnIn = nullptr;
    TextInput* m_targetIn = nullptr;
    TextInput* m_skillIn = nullptr;
    TextInput* m_tolIn = nullptr;
    TextInput* m_iterIn = nullptr;
    TextInput* m_ktIn = nullptr;
    TextInput* m_kuIn = nullptr;
    TextInput* m_kcIn = nullptr;

    TextInput* mkInput(float x, float y, float w, const std::string& val,
                       CommonFilter filter, geode::Function<void(std::string const&)> &&cb,
                       float scale = 0.8f) {
        auto in = TextInput::create(w, "");
        in->setPosition({x, y});
        in->setScale(scale);
        in->setCommonFilter(filter);
        in->setString(val, false);
        in->setCallback(std::move(cb));
        m_mainLayer->addChild(in);
        return in;
    }

    void mkLabel(const char* text, float x, float y, float scale = 0.42f) {
        auto l = CCLabelBMFont::create(text, "bigFont.fnt");
        l->setAnchorPoint({0.f, 0.5f});
        l->setPosition({x, y});
        l->setScale(scale);
        m_mainLayer->addChild(l);
    }

    CCMenuItemSpriteExtra* mkButton(const char* text, float x, float y,
                                    cocos2d::SEL_MenuHandler cb, float scale = 0.55f) {
        auto spr = ButtonSprite::create(text);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        btn->setPosition({x, y});
        btn->setScale(scale);
        m_menu->addChild(btn);
        return btn;
    }

    CCMenuItemToggler* mkToggler(float x, float y, bool on, cocos2d::SEL_MenuHandler cb) {
        auto off = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        auto onSpr = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto tog = CCMenuItemToggler::create(off, onSpr, this, cb);
        tog->setPosition({x, y});
        tog->setScale(0.65f);
        tog->toggle(on);
        m_menu->addChild(tog);
        return tog;
    }

    CCMenu* m_menu = nullptr;
    double m_dispTime(const CalcRow& r) const {
        return m_useFrames ? r.time * m_gameFps : r.time;
    }

    void setMsg(const std::string& s) {
        if (m_msg) m_msg->setString(s.c_str());
    }

    void syncDoc() {
        g_doc.rows = m_rows;
        g_doc.gameFps = m_gameFps;
        g_doc.windowFps = m_windowFps;
        g_doc.respawn = m_respawn;
        g_doc.useFrames = m_useFrames;
        g_doc.solve = m_solve;
        g_doc.target = m_target;
        g_doc.tol = m_tol;
        g_doc.iters = m_iters;
        g_doc.skill = m_skill;
        g_doc.kt = m_kt; g_doc.ku = m_ku; g_doc.kc = m_kc;
        g_doc.ktOn = m_ktOn; g_doc.kuOn = m_kuOn; g_doc.kcOn = m_kcOn;
    }

    void loadDoc() {
        m_rows = g_doc.rows;
        m_gameFps = g_doc.gameFps;
        m_windowFps = g_doc.windowFps;
        m_respawn = g_doc.respawn;
        m_useFrames = g_doc.useFrames;
        m_solve = g_doc.solve;
        m_target = g_doc.target;
        m_tol = g_doc.tol;
        m_iters = g_doc.iters;
        m_skill = g_doc.skill;
        m_kt = g_doc.kt; m_ku = g_doc.ku; m_kc = g_doc.kc;
        m_ktOn = g_doc.ktOn; m_kuOn = g_doc.kuOn; m_kcOn = g_doc.kcOn;
        m_page = 0;
    }

    // Called by the auto-prober when a window is measured.
    void refreshFromDoc(const std::string& note = "") {
        m_rows = g_doc.rows;
        rebuildRows();
        if (!note.empty()) setMsg(note);
    }

    void onClose(CCObject* sender) override;

    // ---- rows UI ----
    void rebuildRows() {
        if (!m_rowsBox) return;
        syncDoc();
        m_rowsBox->removeAllChildren();
        int pages = std::max(1, (int)((m_rows.size() + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE));
        m_page = std::min(std::max(0, m_page), pages - 1);
        int start = m_page * ROWS_PER_PAGE;
        for (int r = 0; r < ROWS_PER_PAGE; ++r) {
            int di = start + r;
            if (di >= (int)m_rows.size()) break;
            float y = 272.f - r * 30.f;
            auto idx = CCLabelBMFont::create(fmt::format("#{}", di + 1).c_str(), "bigFont.fnt");
            idx->setAnchorPoint({0.f, 0.5f});
            idx->setPosition({214.f, y});
            idx->setScale(0.5f);
            m_rowsBox->addChild(idx);

            auto tIn = TextInput::create(110.f, "time");
            tIn->setPosition({305.f, y});
            tIn->setScale(0.8f);
            tIn->setCommonFilter(CommonFilter::Float);
            tIn->setString(fmtNum(m_dispTime(m_rows[di])), false);
            tIn->setCallback([this, di](std::string const& s) {
                if (di < 0 || di >= (int)m_rows.size()) return;
                try {
                    double v = s.empty() ? 0.0 : std::stod(s);
                    if (!std::isfinite(v) || v < 0) return;
                    m_rows[di].time = m_useFrames ? v / m_gameFps : v;
                } catch (...) {}
            });
            m_rowsBox->addChild(tIn);

            auto wIn = TextInput::create(80.f, "-");
            wIn->setPosition({415.f, y});
            wIn->setScale(0.8f);
            wIn->setString(m_rows[di].window, false);
            wIn->setCallback([this, di](std::string const& s) {
                if (di < 0 || di >= (int)m_rows.size()) return;
                m_rows[di].window = s.empty() ? "-" : s;
            });
            m_rowsBox->addChild(wIn);

            auto del = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("x"), this, menu_selector(CalcPopup::onDelRow));
            del->setPosition({478.f, y});
            del->setScale(0.5f);
            del->setTag(di);
            m_rowsBox->addChild(del);
        }
        if (m_pageLabel) {
            m_pageLabel->setString(
                fmt::format("{}/{} - {} rows", m_page + 1, pages, (int)m_rows.size()).c_str());
        }
    }

    void onDelRow(CCObject* sender) {
        int di = static_cast<CCNode*>(sender)->getTag();
        if (di >= 0 && di < (int)m_rows.size()) m_rows.erase(m_rows.begin() + di);
        rebuildRows();
    }

    void onPrevPage(CCObject*) { m_page--; rebuildRows(); }
    void onNextPage(CCObject*) { m_page++; rebuildRows(); }
    void onAddRow(CCObject*) {
        CalcRow r;
        if (!m_rows.empty()) {
            r.input = m_rows.back().input + 1;
            r.time = m_rows.back().time;
        }
        m_rows.push_back(r);
        m_page = ((int)m_rows.size() - 1) / ROWS_PER_PAGE;
        rebuildRows();
    }

    // ---- settings callbacks ----
    static double numOr(const std::string& s, double fallback) {
        try {
            double v = std::stod(s);
            return std::isfinite(v) ? v : fallback;
        } catch (...) { return fallback; }
    }
    void onFramesTog(CCObject* sender) {
        m_useFrames = static_cast<CCMenuItemToggler*>(sender)->isToggled();
        rebuildRows();
    }
    void onKtTog(CCObject* s) { m_ktOn = static_cast<CCMenuItemToggler*>(s)->isToggled(); }
    void onKuTog(CCObject* s) { m_kuOn = static_cast<CCMenuItemToggler*>(s)->isToggled(); }
    void onKcTog(CCObject* s) { m_kcOn = static_cast<CCMenuItemToggler*>(s)->isToggled(); }
    void onMode(CCObject*) {
        m_solve = !m_solve;
        auto spr = ButtonSprite::create(m_solve ? "Mode: Solve 24h" : "Mode: Fixed skill");
        m_modeBtn->setNormalImage(spr);
        m_modeBtn->setSelectedImage(spr);
        m_modeBtn->updateSprite();
    }

    // ---- model ----
    bool collectRows(std::vector<nandl::Row>& out, std::string& err) {
        out.clear();
        if (m_rows.empty()) { err = "No input rows (Load run or Add)."; return false; }
        for (auto& r : m_rows) {
            if (r.input < 1) { err = "Input numbers must be >= 1."; return false; }
            nandl::Row nr;
            nr.input = r.input;
            nr.time = r.time + m_respawn;
            if (!(nr.time >= 0)) { err = "Times must be >= 0."; return false; }
            if (r.window == "-") {
                nr.ignored = true;
            } else {
                try {
                    double w = std::stod(r.window);
                    if (!(w >= 0)) throw std::runtime_error("x");
                    nr.window = w;
                    nr.ignored = false;
                } catch (...) { err = "Bad window (use number or -)."; return false; }
            }
            out.push_back(nr);
        }
        return true;
    }

    void onCalculate(CCObject*) {
        std::vector<nandl::Row> rows;
        std::string err;
        if (!collectRows(rows, err)) {
            setMsg(err);
            return;
        }
        nandl::Constants c{m_ktOn ? m_kt : 0.0, m_kuOn ? m_ku : 0.0, m_kcOn ? m_kc : 0.0};
        try {
            if (m_solve) {
                auto sol = nandl::solveSkill(rows, m_target, m_tol, m_iters, m_windowFps, c);
                m_resultMain->setString(fmt::format("{:.2f} s/s", sol.skill).c_str());
                m_resultSub->setString(fmt::format("E[T]={:.4g}s P={:.3e} att={:.4g}",
                    sol.stats.total, sol.stats.success, sol.stats.attempts).c_str());
                setMsg(fmt::format("Target {:.4g}s, err {:.4g}s.", m_target, sol.error));
            } else {
                auto st = nandl::computeStats(rows, m_skill, m_windowFps, c);
                m_resultMain->setString(fmt::format("{:.2f} s/s stats", m_skill).c_str());
                m_resultSub->setString(fmt::format("E[T]={:.4g}s P={:.3e} att={:.4g}",
                    st.total, st.success, st.attempts).c_str());
                setMsg("Fixed-precision stats.");
            }
        } catch (const std::exception& e) {
            setMsg(e.what());
        }
    }

    // ---- run / files ----
    std::string runPayload() {
        std::ostringstream o;
        o << "{\n  \"format\": \"nandl-calculator\",\n  \"version\": 1,\n  \"settings\": {\n";
        o << "    \"gameFps\": " << fmtNum(m_gameFps) << ",\n";
        o << "    \"windowFps\": " << fmtNum(m_windowFps) << ",\n";
        o << "    \"respawnSeconds\": " << fmtNum(m_respawn) << ",\n";
        o << "    \"timeUnit\": \"" << (m_useFrames ? "frames" : "seconds") << "\"\n";
        o << "  },\n  \"frameWindows\": [";
        for (size_t i = 0; i < m_rows.size(); ++i) {
            auto& r = m_rows[i];
            o << (i ? ",\n    " : "\n    ");
            double t = m_useFrames ? r.time * m_gameFps : r.time;
            o << "{\"input\": " << r.input << ", \"timePosition\": " << fmtNum(t)
              << ", \"frameWindow\": ";
            if (r.window != "-") o << r.window;
            else o << "\"-\"";
            // Extra fidelity for the auto-prober (site ignores unknown fields).
            o << ", \"down\": " << (r.down ? "true" : "false")
              << ", \"button\": " << r.button << ", \"p2\": " << (r.p2 ? "true" : "false");
            o << "}";
        }
        o << "\n  ]\n}\n";
        return o.str();
    }

public:
    void loadRun() {
        if (g_rec.events.empty()) return;
        m_rows.clear();
        for (size_t i = 0; i < g_rec.events.size(); ++i) {
            CalcRow r;
            r.input = (int)i + 1;
            r.time = g_rec.events[i].time;
            r.window = "-";
            r.down = g_rec.events[i].down;
            r.button = g_rec.events[i].button;
            r.p2 = g_rec.events[i].p2;
            m_rows.push_back(r);
        }
        m_page = 0;
        rebuildRows();
        syncDoc();
        setMsg(fmt::format("Loaded {} inputs; fill windows, Calculate.", (int)m_rows.size()));
    }

protected:
    void onLoadRun(CCObject*) { loadRun(); }
    void onMeasure(CCObject*);

    void onExport(CCObject*) {
        if (m_rows.empty()) { setMsg("Nothing to export."); return; }
        namespace fs = std::filesystem;
        fs::path dir = Mod::get()->getSaveDir();
        fs::path out = dir / "calculator-export.json";
        std::ofstream f(out.string());
        if (!f) { setMsg("Export failed."); return; }
        f << runPayload();
        log::info("[frame-windows] calculator export: {}", out.string());
        setMsg("Exported calculator-export.json (Import it on the site).");
    }

    bool applyPayload(const matjson::Value& root, const std::string& name) {
        auto numOrJson = [](const matjson::Value& v, double fb) {
            if (!v.isNumber()) return fb;
            auto r = v.asDouble();
            return r.isErr() ? fb : r.unwrap();
        };
        auto strOrJson = [](const matjson::Value& v, const std::string& fb) {
            if (!v.isString()) return fb;
            auto r = v.asString();
            if (r.isErr()) return fb;
            return std::string(r.unwrap());
        };
        if (!root.contains("frameWindows") || !root["frameWindows"].isArray()) return false;
        auto arrRes = root["frameWindows"].asArray();
        if (arrRes.isErr()) return false;
        std::vector<CalcRow> rows;
        bool lastDown = false; // assume alternating press/release if unknown
        for (auto const& rv : arrRes.unwrap()) {
            if (!rv.contains("input") || !rv.contains("timePosition") || !rv.contains("frameWindow"))
                return false;
            CalcRow r;
            r.input = (int)numOrJson(rv["input"], 1.0);
            r.time = numOrJson(rv["timePosition"], 0.0);
            // Direction fidelity for the auto-prober; assume alternating
            // press/release (standard play) when the file lacks it.
            if (rv.contains("down") && rv["down"].isBool()) {
                auto b = rv["down"].asBool();
                r.down = b.isErr() ? !lastDown : b.unwrap();
            } else {
                r.down = !lastDown;
            }
            lastDown = r.down;
            r.button = rv.contains("button") ? (int)numOrJson(rv["button"], 1.0) : 1;
            if (rv.contains("p2") && rv["p2"].isBool()) {
                auto b = rv["p2"].asBool();
                r.p2 = b.isErr() ? false : b.unwrap();
            } else {
                r.p2 = false;
            }
            if (rv["frameWindow"].isString()) {
                if (strOrJson(rv["frameWindow"], "-") != "-") return false;
                r.window = "-";
            } else if (rv["frameWindow"].isNumber()) {
                r.window = fmtNum(numOrJson(rv["frameWindow"], -1.0));
            } else {
                return false;
            }
            rows.push_back(r);
        }
        if (rows.empty()) return false;
        if (root.contains("settings")) {
            auto const& s = root["settings"];
            if (s.contains("gameFps")) m_gameFps = numOrJson(s["gameFps"], m_gameFps);
            if (s.contains("windowFps")) m_windowFps = numOrJson(s["windowFps"], m_windowFps);
            if (s.contains("respawnSeconds")) m_respawn = numOrJson(s["respawnSeconds"], m_respawn);
            if (s.contains("timeUnit"))
                m_useFrames = (strOrJson(s["timeUnit"], "seconds") == "frames");
            if (m_gameFpsIn) m_gameFpsIn->setString(fmtNum(m_gameFps), false);
            if (m_windowFpsIn) m_windowFpsIn->setString(fmtNum(m_windowFps), false);
            if (m_respawnIn) m_respawnIn->setString(fmtNum(m_respawn), false);
        }
        m_rows = std::move(rows);
        m_page = 0;
        rebuildRows();
        setMsg(fmt::format("Imported {} rows from {}.", (int)m_rows.size(), name));
        return true;
    }

    void onImport(CCObject*) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir = Mod::get()->getSaveDir();
        fs::path best;
        fs::file_time_type bt{};
        bool found = false;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (ec || !e.is_regular_file()) continue;
            if (e.path().extension() != ".json") continue;
            std::ifstream f(e.path());
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto pr = matjson::parse(s);
            if (pr.isErr()) continue;
            auto root = pr.unwrap();
            if (!root.contains("frameWindows") || !root["frameWindows"].isArray()) continue;
            auto t = fs::last_write_time(e.path(), ec);
            if (ec) continue;
            if (!found || t > bt) { best = e.path(); bt = t; found = true; }
        }
        if (!found) { setMsg("No calculator JSON in save dir."); return; }
        std::ifstream f(best);
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto pr = matjson::parse(s);
        if (pr.isErr() || !applyPayload(pr.unwrap(), best.filename().string()))
            setMsg("Import file unreadable.");
    }

    bool initCalc() {
        loadDoc();
        m_gameFps = getDbl("game-fps", m_gameFps);
        m_windowFps = getDbl("window-fps", m_windowFps);
        m_respawn = getDbl("respawn-time", m_respawn);

        m_menu = CCMenu::create();
        m_menu->setPosition({0.f, 0.f});
        m_menu->setContentSize(m_size);
        m_mainLayer->addChild(m_menu);

        // ---- left: settings ----
        float y = 296.f;
        mkLabel("Game FPS", 16.f, y);
        m_gameFpsIn = mkInput(152.f, y, 64.f, fmtNum(m_gameFps), CommonFilter::Float,
            [this](std::string const& s) { m_gameFps = numOr(s, m_gameFps); rebuildRows(); });
        y -= 22.f;
        mkLabel("Window FPS", 16.f, y);
        m_windowFpsIn = mkInput(152.f, y, 64.f, fmtNum(m_windowFps), CommonFilter::Float,
            [this](std::string const& s) { m_windowFps = numOr(s, m_windowFps); });
        y -= 22.f;
        mkLabel("Respawn s", 16.f, y);
        m_respawnIn = mkInput(152.f, y, 64.f, fmtNum(m_respawn), CommonFilter::Float,
            [this](std::string const& s) { m_respawn = numOr(s, m_respawn); });
        y -= 22.f;
        mkToggler(28.f, y, m_useFrames, menu_selector(CalcPopup::onFramesTog));
        mkLabel("Use frames", 44.f, y);
        y -= 24.f;
        m_modeBtn = mkButton("Mode: Solve 24h", 105.f, y, menu_selector(CalcPopup::onMode), 0.6f);
        y -= 24.f;
        mkLabel("Target s", 16.f, y);
        m_targetIn = mkInput(152.f, y, 64.f, fmtNum(m_target), CommonFilter::Float,
            [this](std::string const& s) { m_target = numOr(s, m_target); });
        y -= 22.f;
        mkLabel("Skill s/s", 16.f, y);
        m_skillIn = mkInput(152.f, y, 64.f, fmtNum(m_skill), CommonFilter::Float,
            [this](std::string const& s) { m_skill = numOr(s, m_skill); });
        y -= 22.f;
        mkLabel("Tol s", 16.f, y);
        m_tolIn = mkInput(80.f, y, 46.f, fmtNum(m_tol), CommonFilter::Float,
            [this](std::string const& s) { m_tol = numOr(s, m_tol); });
        mkLabel("Iter", 128.f, y);
        m_iterIn = mkInput(172.f, y, 40.f, fmt::format("{}", m_iters), CommonFilter::Uint,
            [this](std::string const& s) {
                try { m_iters = std::max(1, std::stoi(s)); } catch (...) {}
            });
        y -= 22.f;
        mkToggler(28.f, y, m_ktOn, menu_selector(CalcPopup::onKtTog));
        mkLabel("kt", 44.f, y);
        m_ktIn = mkInput(128.f, y, 104.f, fmtNum(m_kt), CommonFilter::Float,
            [this](std::string const& s) { m_kt = numOr(s, m_kt); }, 0.72f);
        y -= 22.f;
        mkToggler(28.f, y, m_kuOn, menu_selector(CalcPopup::onKuTog));
        mkLabel("ku", 44.f, y);
        m_kuIn = mkInput(128.f, y, 104.f, fmtNum(m_ku), CommonFilter::Float,
            [this](std::string const& s) { m_ku = numOr(s, m_ku); }, 0.72f);
        y -= 22.f;
        mkToggler(28.f, y, m_kcOn, menu_selector(CalcPopup::onKcTog));
        mkLabel("kc", 44.f, y);
        m_kcIn = mkInput(128.f, y, 104.f, fmtNum(m_kc), CommonFilter::Float,
            [this](std::string const& s) { m_kc = numOr(s, m_kc); }, 0.72f);

        m_resultMain = CCLabelBMFont::create("-", "bigFont.fnt");
        m_resultMain->setAnchorPoint({0.f, 0.5f});
        m_resultMain->setPosition({14.f, 40.f});
        m_resultMain->setScale(0.5f);
        m_mainLayer->addChild(m_resultMain);
        m_resultSub = CCLabelBMFont::create("", "bigFont.fnt");
        m_resultSub->setAnchorPoint({0.f, 0.5f});
        m_resultSub->setPosition({14.f, 22.f});
        m_resultSub->setScale(0.34f);
        m_mainLayer->addChild(m_resultSub);

        // ---- right: rows ----
        mkLabel("#", 214.f, 302.f, 0.45f);
        mkLabel("time", 268.f, 302.f, 0.45f);
        mkLabel("window", 382.f, 302.f, 0.45f);
        m_rowsBox = CCMenu::create();
        m_rowsBox->setPosition({0.f, 0.f});
        m_rowsBox->setContentSize(m_size);
        m_mainLayer->addChild(m_rowsBox);

        mkButton("<", 240.f, 82.f, menu_selector(CalcPopup::onPrevPage), 0.55f);
        m_pageLabel = CCLabelBMFont::create("- rows", "bigFont.fnt");
        m_pageLabel->setPosition({320.f, 82.f});
        m_pageLabel->setScale(0.42f);
        m_mainLayer->addChild(m_pageLabel);
        mkButton(">", 400.f, 82.f, menu_selector(CalcPopup::onNextPage), 0.55f);
        mkButton("+ Add", 490.f, 82.f, menu_selector(CalcPopup::onAddRow), 0.5f);

        mkButton("Load run", 225.f, 48.f, menu_selector(CalcPopup::onLoadRun), 0.55f);
        mkButton("Import", 300.f, 48.f, menu_selector(CalcPopup::onImport), 0.55f);
        mkButton("Export", 372.f, 48.f, menu_selector(CalcPopup::onExport), 0.55f);
        mkButton("Measure", 448.f, 48.f, menu_selector(CalcPopup::onMeasure), 0.55f);
        mkButton("Calculate", 528.f, 48.f, menu_selector(CalcPopup::onCalculate), 0.55f);

        m_msg = CCLabelBMFont::create("", "bigFont.fnt");
        m_msg->setAnchorPoint({0.f, 0.5f});
        m_msg->setPosition({205.f, 16.f});
        m_msg->setScale(0.36f);
        m_msg->setColor({255, 150, 150});
        m_mainLayer->addChild(m_msg);

        rebuildRows();
        return true;
    }

public:
    static CalcPopup* create() {
        auto ret = new CalcPopup();
        if (ret->init(600.f, 360.f) && ret->initCalc()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ------------------------------------------------- auto-probe
// TAS-style trial measurement of TRUE frame windows, directly in the level:
// replays a passing input script from checkpoints with one input shifted by
// k ticks; PASS = still alive on the baseline trajectory at the target tick.
// Binary-searches both sides of every input. Same method as the manual
// frame-stepper technique (reddit "How to count frame perfects"), automated.

struct ProbeScriptInput {
    int tick = 0;
    bool down = true;
    int button = 1;
    bool p2 = false;
};

class ProbePopup; // defined below the engine

struct ProbeEngine {
    bool active = false;
    bool injecting = false;
    bool expectReset = false;
    CalcPopup* calc = nullptr;
    ProbePopup* ui = nullptr;
    PlayLayer* pl = nullptr;

    std::vector<ProbeScriptInput> script;
    std::vector<float> baseX, baseY;
    struct Cp { int tick = 0; CheckpointObject* obj = nullptr; };
    std::vector<Cp> cps;

    enum St { IDLE, BASE, SELF, TRIAL } st = IDLE;
    enum Pend { NONE, RESET, LOAD } pend = NONE;
    CheckpointObject* pendCp = nullptr;

    int probeTick = 0;
    size_t fireIdx = 0;
    bool trialDead = false;
    int curTarget = 0;
    int lastScriptTick = 0;
    int cpSpacing = 480;

    int fromIdx = 0, toIdx = -1, jobIdx = 0;
    int maxOff = 12;
    int phase = 0; // 0 early-exp, 1 early-bis, 2 late-exp, 3 late-bis
    int expK = 1, lo = 0, hi = 1, lastMid = 0;
    int earlyPass = 0, latePass = 0;
    int curShift = 0, curCpTick = -1;
    CheckpointObject* curCp = nullptr;
    int trials = 0, measured = 0;
    std::string progress = "Idle.";
    std::string logStr;

    void setOverlay(const std::string& s);
    void setLog(const std::string& s);
    void setProgress();

    size_t lowerBoundTick(int t) {
        size_t i = 0;
        while (i < script.size() && script[i].tick <= t) i++;
        return i;
    }

    void releaseCps() {
        for (auto& c : cps) { if (c.obj) c.obj->release(); }
        cps.clear();
    }

    void stop(const std::string& why) {
        active = false;
        st = IDLE;
        pend = NONE;
        injecting = false;
        expectReset = false;
        releaseCps();
        setLog(why);
        setOverlay("");
        refreshOverlay();
        pl = nullptr;
    }

    void abort(const std::string& why) { stop(why); }

    bool start(CalcPopup* c, int from, int to, int maxO, int spacing) {
        if (active) return false;
        if (g_doc.rows.empty()) return false;
        // build script, require non-decreasing times
        script.clear();
        int prev = -1;
        for (auto& r : g_doc.rows) {
            int t = (int)std::lround(r.time * 240.0);
            if (t < 0 || t < prev) return false;
            prev = t;
            script.push_back({t, r.down, r.button, r.p2});
        }
        if (script.empty()) return false;
        fromIdx = std::max(0, from - 1);
        toIdx = std::min((int)script.size() - 1, to - 1);
        if (fromIdx > toIdx) return false;
        maxOff = std::min(16, std::max(1, maxO));
        cpSpacing = std::max(120, spacing);
        lastScriptTick = script.back().tick;
        calc = c;
        pl = g_rec.owner;
        if (!pl || !pl->m_player1) return false;
        baseX.clear(); baseY.clear();
        releaseCps();
        trials = 0; measured = 0;
        active = true;
        st = BASE;
        pend = RESET;
        pendCp = nullptr;
        setLog("Baseline run...");
        setProgress();
        return true;
    }

    void inject(const ProbeScriptInput& in) {
        injecting = true;
        if (pl) pl->handleButton(in.down, in.button, !in.p2);
        injecting = false;
    }

    int trialStart() const {
        if (st == BASE) return -1;
        return curCpTick; // -1 when probing from attempt start
    }

    void queueLoad() {
        if (curCp && st != BASE) { pend = LOAD; pendCp = curCp; }
        else { pend = RESET; pendCp = nullptr; }
    }

    void onDeath() {
        if (!active) return;
        trialDead = true;
    }

    void finishBaseline() {
        // pick self-test checkpoint, else run self-test from attempt start
        CheckpointObject* scp = nullptr;
        int scpTick = -1;
        for (auto& c : cps) {
            if (c.tick + 90 <= (int)baseX.size() - 1) { scp = c.obj; scpTick = c.tick; }
        }
        if (!scp && !cps.empty() && lastScriptTick > 1200) {
            abort("Checkpoints unavailable - enable Practice mode and retry.");
            return;
        }
        st = SELF;
        curCp = scp;
        curCpTick = scpTick;
        curTarget = (scp ? scpTick : 0) + 90;
        if (curTarget >= (int)baseX.size()) curTarget = (int)baseX.size() - 1;
        queueLoad();
        setLog("Self-test (checkpoint restore)...");
        setProgress();
    }

    void beginTrial(int shift) {
        curShift = shift;
        trials++;
        queueLoad();
        setProgress();
    }

    void setupJob() {
        int tick_i = script[jobIdx].tick;
        curCp = nullptr;
        curCpTick = -1;
        for (auto& c : cps) {
            if (c.tick <= tick_i - maxOff - 5) { curCp = c.obj; curCpTick = c.tick; }
        }
        curTarget = (jobIdx + 2 < (int)script.size())
            ? script[jobIdx + 2].tick + 15
            : lastScriptTick + 60;
        if (curTarget >= (int)baseX.size()) curTarget = (int)baseX.size() - 1;
        if (curTarget <= tick_i + 10) {
            // not enough baseline tail to judge; skip (leave as-is)
            finishInput(true);
            return;
        }
        phase = 0; expK = 1; earlyPass = 0; latePass = 0;
        beginTrial(-1);
    }

    void finishInput(bool skipped = false) {
        if (!skipped) {
            int w = earlyPass + 1 + latePass;
            if (jobIdx >= 0 && jobIdx < (int)g_doc.rows.size()) {
                g_doc.rows[jobIdx].window = std::to_string(w);
                measured++;
                if (calc) calc->refreshFromDoc(
                    fmt::format("Measured #{} = {}f.", jobIdx + 1, w));
            }
        }
        jobIdx++;
        if (jobIdx > toIdx) {
            setLog(fmt::format("Done: {} measured, {} trials.", measured, trials));
            setOverlay("");
            bool hadCalc = calc != nullptr;
            stop("");
            if (hadCalc) refreshOverlay();
            return;
        }
        setupJob();
    }

    void continueBisect() {
        lastMid = (lo + hi) / 2;
        beginTrial(phase == 1 ? -lastMid : lastMid);
    }

    void onTrialVerdict(bool pass) {
        if (phase == 0) {
            if (pass) {
                earlyPass = expK; expK *= 2;
                if (expK > maxOff) { phase = 2; expK = 1; beginTrial(+expK); }
                else beginTrial(-expK);
            } else { lo = earlyPass; hi = expK; phase = 1; continueBisect(); }
        } else if (phase == 1) {
            if (pass) lo = lastMid; else hi = lastMid;
            if (hi - lo <= 1) { earlyPass = lo; phase = 2; expK = 1; beginTrial(+expK); }
            else continueBisect();
        } else if (phase == 2) {
            if (pass) {
                latePass = expK; expK *= 2;
                if (expK > maxOff) finishInput();
                else beginTrial(+expK);
            } else { lo = latePass; hi = expK; phase = 3; continueBisect(); }
        } else {
            if (pass) lo = lastMid; else hi = lastMid;
            if (hi - lo <= 1) { latePass = lo; finishInput(); }
            else continueBisect();
        }
    }

    void update(float dt) {
        if (!active || !pl) return;
        if (pend != NONE) {
            if (pend == RESET) {
                expectReset = true;
                probeTick = 0;
                fireIdx = 0;
                trialDead = false;
                pl->resetLevel();
            } else {
                probeTick = curCpTick;
                fireIdx = lowerBoundTick(probeTick);
                trialDead = false;
                if (pendCp) pl->loadFromCheckpoint(pendCp);
            }
            pend = NONE;
            return;
        }
        if (!pl->m_player1) return; // transient; clock pauses consistently
        probeTick += std::max(1, (int)std::llround(dt * 240.0));
        int tStart = trialStart();
        int jobPos = (st == TRIAL) ? jobIdx : -1;
        while (fireIdx < script.size()) {
            int t = script[fireIdx].tick;
            if ((int)fireIdx == jobPos) t += curShift;
            if (t > probeTick) break;
            if (t > tStart) inject(script[fireIdx]);
            fireIdx++;
        }
        auto pp = pl->m_player1->getPosition();
        if (st == BASE) {
            baseX.push_back(pp.x);
            baseY.push_back(pp.y);
            if (probeTick > 0 && probeTick % cpSpacing == 0) {
                auto cp = pl->markCheckpoint();
                if (cp) {
                    pl->storeCheckpoint(cp);
                    cp->retain();
                    cps.push_back({probeTick, cp});
                }
            }
            if (trialDead) { abort("Baseline died - run is not a clean pass."); return; }
            if (probeTick >= lastScriptTick + 30) { finishBaseline(); return; }
        } else {
            if (trialDead) {
                onTrialVerdict(false);
                return;
            }
            if (probeTick >= curTarget) {
                bool ok = true;
                if (curTarget >= 0 && curTarget < (int)baseX.size()) {
                    ok = std::fabs(pp.x - baseX[curTarget]) < 6.f
                      && std::fabs(pp.y - baseY[curTarget]) < 12.f;
                }
                if (st == SELF) {
                    if (ok) {
                        st = TRIAL;
                        jobIdx = fromIdx;
                        setupJob();
                        setLog("Self-test passed. Probing...");
                    } else {
                        abort("Checkpoint restore mismatch - try Practice mode.");
                    }
                    return;
                }
                onTrialVerdict(ok);
                return;
            }
        }
    }
};

static ProbeEngine g_probe;

// recordEvent() is defined earlier in the file (before the engine), so the
// hook calls through this wrapper to also honor probe mode.
static void recordEventChecked(int button, bool player1, bool down) {
    if (g_probe.active) return; // scripted/user inputs stay out of the recording
    recordEvent(button, player1, down);
}

class ProbePopup : public Popup {
    CalcPopup* m_calc = nullptr;
    TextInput* m_fromIn = nullptr;
    TextInput* m_toIn = nullptr;
    TextInput* m_maxIn = nullptr;
    TextInput* m_spIn = nullptr;
    CCLabelBMFont* m_prog = nullptr;
    CCLabelBMFont* m_log = nullptr;
    CCMenu* m_menu = nullptr;
    int m_from = 1, m_to = 1, m_maxO = 12;
    double m_sp = 2.0;

    void mkLab(const char* t, float x, float y, float s = 0.42f) {
        auto l = CCLabelBMFont::create(t, "bigFont.fnt");
        l->setAnchorPoint({0.f, 0.5f});
        l->setPosition({x, y});
        l->setScale(s);
        m_mainLayer->addChild(l);
    }

    TextInput* mkIn(float x, float y, float w, const std::string& v,
                    geode::Function<void(std::string const&)> &&cb) {
        auto in = TextInput::create(w, "");
        in->setPosition({x, y});
        in->setScale(0.8f);
        in->setCommonFilter(CommonFilter::Float);
        in->setString(v, false);
        in->setCallback(std::move(cb));
        m_mainLayer->addChild(in);
        return in;
    }

    void onStart(CCObject*) {
        if (g_probe.active) { setLog("Already running - Stop first."); return; }
        if (!m_calc) return;
        m_calc->syncDoc();
        int n = (int)g_doc.rows.size();
        if (n == 0) { setLog("Load run or Import first."); return; }
        int from = std::min(n, std::max(1, m_from));
        int to = std::min(n, std::max(1, m_to));
        if (from > to) { setLog("From must be <= To."); return; }
        int maxO = std::min(16, std::max(1, m_maxO));
        int spacing = std::min(2400, std::max(120, (int)std::llround(m_sp * 240.0)));
        if (!g_rec.owner || !g_rec.owner->m_player1) {
            setLog("Open a level first.");
            return;
        }
        g_probe.ui = this;
        if (!g_probe.start(m_calc, from, to, maxO, spacing)) {
            g_probe.ui = nullptr;
            setLog("Could not start (need passing run, ordered times).");
            return;
        }
        setLog("Running - close panels and resume to run.");
    }

    void onStop(CCObject*) {
        if (g_probe.active) g_probe.stop("Stopped by user.");
        else setLog("Idle.");
    }

    bool initUI(CalcPopup* calc) {
        m_calc = calc;
        int n = (int)g_doc.rows.size();
        m_from = 1;
        m_to = std::max(1, n);
        m_menu = CCMenu::create();
        m_menu->setPosition({0.f, 0.f});
        m_menu->setContentSize(m_size);
        m_mainLayer->addChild(m_menu);

        mkLab("From input", 20.f, 244.f);
        m_fromIn = mkIn(150.f, 244.f, 70.f, "1", [this](std::string const& s) {
            try { m_from = std::stoi(s); } catch (...) {}
        });
        mkLab("To input", 20.f, 220.f);
        m_toIn = mkIn(150.f, 220.f, 70.f, fmt::format("{}", m_to), [this](std::string const& s) {
            try { m_to = std::stoi(s); } catch (...) {}
        });
        mkLab("Max offset", 20.f, 196.f);
        m_maxIn = mkIn(150.f, 196.f, 70.f, "12", [this](std::string const& s) {
            try { m_maxO = std::stoi(s); } catch (...) {}
        });
        mkLab("Checkpt s", 20.f, 172.f);
        m_spIn = mkIn(150.f, 172.f, 70.f, "2", [this](std::string const& s) {
            try { m_sp = std::stod(s); } catch (...) {}
        });

        auto start = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Start"), this, menu_selector(ProbePopup::onStart));
        start->setPosition({100.f, 128.f});
        start->setScale(0.6f);
        m_menu->addChild(start);
        auto stopB = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Stop"), this, menu_selector(ProbePopup::onStop));
        stopB->setPosition({270.f, 128.f});
        stopB->setScale(0.6f);
        m_menu->addChild(stopB);

        m_prog = CCLabelBMFont::create("Idle.", "bigFont.fnt");
        m_prog->setAnchorPoint({0.f, 0.5f});
        m_prog->setPosition({20.f, 96.f});
        m_prog->setScale(0.45f);
        m_mainLayer->addChild(m_prog);
        m_log = CCLabelBMFont::create("", "bigFont.fnt");
        m_log->setAnchorPoint({0.f, 0.5f});
        m_log->setPosition({20.f, 74.f});
        m_log->setScale(0.36f);
        m_log->setColor({255, 180, 150});
        m_mainLayer->addChild(m_log);
        mkLab("Needs a passing run. Best at stable 240fps.", 20.f, 44.f, 0.32f);
        mkLab("Close panels + resume to run. Full levels take hours.", 20.f, 28.f, 0.32f);
        if (g_probe.active) {
            m_prog->setString(g_probe.progress.c_str());
            m_log->setString(g_probe.logStr.c_str());
        }
        return true;
    }

public:
    void setProgress(const std::string& s) {
        if (m_prog) m_prog->setString(s.c_str());
    }
    void setLog(const std::string& s) {
        if (m_log) m_log->setString(s.c_str());
    }
    void onClose(CCObject* sender) override {
        if (g_probe.ui == this) g_probe.ui = nullptr;
        Popup::onClose(sender);
    }
    static ProbePopup* create(CalcPopup* calc) {
        auto ret = new ProbePopup();
        if (ret->init(380.f, 300.f) && ret->initUI(calc)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

void ProbeEngine::setOverlay(const std::string& s) {
    if (g_rec.overlay) g_rec.overlay->setString(s.c_str());
}

void ProbeEngine::setLog(const std::string& s) {
    logStr = s;
    if (ui) ui->setLog(s);
    if (!s.empty()) log::info("[frame-windows][probe] {}", s);
}

void ProbeEngine::setProgress() {
    if (st == BASE) progress = fmt::format("Baseline t={}", probeTick);
    else if (st == SELF) progress = fmt::format("Self-test t={}", probeTick);
    else if (st == TRIAL) progress = fmt::format("#{}/{} E{} L{} trial {}",
        jobIdx + 1, toIdx + 1, earlyPass, latePass, trials);
    else progress = "Idle.";
    setOverlay(progress);
    if (ui) ui->setProgress(progress);
}

void CalcPopup::onClose(CCObject* sender) {
    syncDoc();
    if (g_probe.calc == this) g_probe.calc = nullptr;
    Popup::onClose(sender);
}

void CalcPopup::onMeasure(CCObject*) {
    syncDoc();
    if (g_doc.rows.empty()) { setMsg("Load run or Import first."); return; }
    auto p = ProbePopup::create(this);
    if (p) p->show();
}

// ----------------------------------------------------------------- hooks

class $modify(FWBaseHook, GJBaseGameLayer) {
    // Unified input entry point in 2.2081 bindings: one hook catches every
    // press AND release, with the GD button id and the P1/P2 flag.
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_probe.active && !g_probe.injecting) return; // swallowed while probing
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (g_rec.owner) {
            if (down || getBool("count-releases", true)) recordEventChecked(button, isPlayer1, down);
        }
    }
};

class $modify(FWPlayerHook, PlayerObject) {
    void playerDestroyed(bool noEffects) {
        PlayerObject::playerDestroyed(noEffects);
        g_probe.onDeath(); // early death signal for trials (no-op when idle)
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
        if (g_probe.active && g_rec.owner == this) g_probe.update(dt);
    }
    void resetLevel() {
        PlayLayer::resetLevel();
        if (g_probe.active) {
            if (g_probe.expectReset) {
                g_probe.expectReset = false;
            } else if (g_probe.st == ProbeEngine::BASE) {
                g_probe.abort("Attempt reset during baseline.");
            } else {
                // Natural post-death retry (or manual): restart current trial.
                g_probe.trialDead = false;
                g_probe.queueLoad();
            }
            return;
        }
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
        if (g_probe.active) g_probe.stop("Level exited.");
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
        auto popup = CalcPopup::create();
        if (!popup) return;
        popup->show();
        popup->loadRun();
    }
};

} // namespace frame_windows
