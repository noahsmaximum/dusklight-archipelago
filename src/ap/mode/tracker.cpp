// The tracker: logic built and searched on worker threads, and its tab.

#include "internal.hpp"

namespace ap::internal {
namespace {


// kInLogicNow lists every check in logic under region headings; otherwise one region's checks,
// in logic first and done last.
constexpr int kInLogicNow = -2;
constexpr const char* kLegendRml =
    R"(<div class="ap-legend"><span class="ap-in"><span class="ap-dot"></span>in logic</span>)"
    R"(<span class="ap-out"><span class="ap-dot"></span>not yet</span>)"
    R"(<span class="ap-done"><span class="ap-dot"></span>done</span>)"
    R"(<span class="ap-skip"><span class="ap-dot"></span>not needed</span></div>)";
int g_trackerShownRegion = kInLogicNow;
std::pair<uint64_t, size_t> g_trackerShown{0, 0};
std::pair<uint64_t, bool> g_finishedSeen{0, true};  // client status version, toggle

}  // namespace

// Used only in this file; defined below.
static void start_logic_build(const std::filesystem::path& base);
static void start_reach_search();
static bool skip_finished_on();
static CheckState check_state(const TrackerCheck& c);
static std::string tracker_summary();
static std::string region_label(int region);
static std::string check_row(const TrackerCheck& c, CheckState st);
static std::string region_rml(int region);
static float done_fraction();
static ModResult build_region_pane(ModContext* ctx, UiElementHandle pane, void* ud, ModError*);

void reset_tracker() {
    ++g_trackerEpoch;
    {
        std::lock_guard lock{g_workers.mutex};
        g_workers.epoch = g_trackerEpoch;
        g_workers.logicDone = g_workers.reachDone = false;
        g_workers.logic.reset();
        g_workers.reach.clear();
    }
    g_logic.reset();
    g_logicKey.clear();
    g_logicBuilding = false;
    g_logicError.clear();
    g_inLogic.clear();
    g_reachInputs = SIZE_MAX;
    g_reachBusy = false;
    ++g_trackerVersion;
}

static void start_logic_build(const std::filesystem::path& base) {
    g_logicBuilding = true;
    const uint64_t epoch = g_trackerEpoch;
    std::thread([base, epoch] {
        std::string error;
        std::shared_ptr<tracker::Logic> logic = tracker::Logic::build(base, error);
        std::lock_guard lock{g_workers.mutex};
        if (epoch == g_workers.epoch) {
            g_workers.logic = std::move(logic);
            g_workers.logicError = error.empty() ? "the seed could not be rebuilt" : error;
            g_workers.logicDone = true;
        }
    }).detach();
}

static void start_reach_search() {
    std::vector<uint16_t> received;
    for (const auto& it : g_serverItems) {
        const int64_t id = it.item - kItemIdBase;
        if (id >= 0 && id <= 0xFFFF) {
            received.push_back(static_cast<uint16_t>(id));
        }
    }
    std::unordered_set<std::string> checks, checked;
    for (const auto& c : g_trackerChecks) {
        checks.insert(c.name);
        if (g_checked.contains(c.id)) {
            checked.insert(c.name);
        }
    }
    g_reachBusy = true;
    std::thread([logic = g_logic, epoch = g_trackerEpoch, received = std::move(received),
                    checks = std::move(checks), checked = std::move(checked),
                    unshuffled = g_unshuffled, known = g_unshuffledKnown] {
        auto reach = logic->reachable(
            received, checks, checked, known ? unshuffled : logic->guess_unshuffled(checks));
        std::lock_guard lock{g_workers.mutex};
        if (epoch == g_workers.epoch) {
            g_workers.reach = std::move(reach);
            g_workers.reachDone = true;
        }
    }).detach();
}

void tick_tracker() {
    // A player finishing (or the toggle) changes which checks are struck out, not logic.
    const std::pair<uint64_t, bool> finished{g_client.statusVersion(), skip_finished_on()};
    if (finished != g_finishedSeen) {
        g_finishedSeen = finished;
        ++g_trackerVersion;
    }
    {
        std::lock_guard lock{g_workers.mutex};
        if (g_workers.logicDone) {
            g_workers.logicDone = false;
            g_logic = std::move(g_workers.logic);
            g_logicError = g_logic ? "" : g_workers.logicError;
            g_logicBuilding = false;
            g_reachInputs = SIZE_MAX;
            ++g_trackerVersion;
            ap_log(g_logic ? "tracker: logic ready" : "tracker: logic failed: " + g_logicError);
        }
        if (g_workers.reachDone) {
            g_workers.reachDone = false;
            g_inLogic = std::move(g_workers.reach);
            g_reachBusy = false;
            ++g_trackerVersion;
        }
    }
    if (g_phase != Phase::Playing || g_needsRegen || g_state.seed.empty()) {
        return;
    }
    const std::string key = g_state.seed + "-" + g_state.slot;
    if (key != g_logicKey) {
        reset_tracker();
        g_logicKey = key;
        const auto base = seed_base(g_state.seed, g_state.slot);
        std::error_code ec;
        if (std::filesystem::exists(base / "plando.yaml", ec)) {
            start_logic_build(base);
        } else {
            g_logicError = "this save's seed files aren't on this computer";
        }
        return;
    }
    if (!g_logic || !g_haveSlot || g_reachBusy) {
        return;
    }
    // Items only ever arrive and checks only ever get done, so the two counts say when the
    // answer can have changed.
    const size_t inputs = g_serverItems.size() * 100003u + g_checked.size();
    if (inputs != g_reachInputs) {
        g_reachInputs = inputs;
        start_reach_search();
    }
}

static bool skip_finished_on() {
    bool on = true;
    if (g_cfgSkipFinished != 0) {
        svc_mng.config->get_bool(svc_mng.mod_ctx, g_cfgSkipFinished, &on);
    }
    return on;
}

static CheckState check_state(const TrackerCheck& c) {
    if (g_checked.contains(c.id)) {
        return CheckState::Done;
    }
    // Our own items never count: g_placementOwner only lists other players' items.
    if (g_finishedSeen.second) {
        const auto owner = g_placementOwner.find(c.name);
        if (owner != g_placementOwner.end() && g_client.slotFinished(owner->second)) {
            return CheckState::Skippable;
        }
    }
    if (!g_logic || g_reachInputs == SIZE_MAX) {
        return CheckState::Unknown;
    }
    return g_inLogic.contains(c.name) ? CheckState::InLogic : CheckState::NotYet;
}

RegionCounts region_counts(int region) {
    RegionCounts n;
    for (const auto& c : g_trackerChecks) {
        if (region >= 0 && c.region != region) {
            continue;
        }
        ++n.total;
        const auto st = check_state(c);
        n.done += st == CheckState::Done;
        n.inLogic += st == CheckState::InLogic;
        n.skippable += st == CheckState::Skippable;
    }
    return n;
}

static std::string tracker_summary() {
    if (!g_haveSlot) {
        return "Connect to your room to see your checks.";
    }
    const auto n = region_counts(-1);
    std::string s = fmt::format("{} of {} checks done", n.done, n.total);
    if (n.skippable > 0) {
        s += fmt::format(", {} not needed", n.skippable);
    }
    if (g_logic) {
        s += fmt::format(", {} in logic now", n.inLogic);
    } else if (g_logicBuilding) {
        s += ". Working out logic...";
    } else if (!g_logicError.empty()) {
        s += ". Logic unavailable: " + g_logicError + ".";
    }
    return s;
}

static std::string region_label(int region) {
    const auto n = region_counts(region);
    std::string label = fmt::format("{}   {}/{}", tracker_regions()[region].label, n.done, n.total);
    if (g_logic && n.inLogic > 0) {
        label += fmt::format("   ({} in logic)", n.inLogic);
    }
    return label;
}

static std::string check_row(const TrackerCheck& c, CheckState st) {
    const char* cls = st == CheckState::Done        ? "ap-done"
                      : st == CheckState::InLogic   ? "ap-in"
                      : st == CheckState::NotYet    ? "ap-out"
                      : st == CheckState::Skippable ? "ap-skip"
                                                    : "ap-unknown";
    return fmt::format(R"(<div class="ap-row {}"><span class="ap-dot"></span>{}</div>)", cls,
        rml_escape(c.name));
}

static std::string region_rml(int region) {
    if (!g_haveSlot) {
        return "";
    }
    std::string out;
    if (region == kInLogicNow) {
        if (!g_logic) {
            return R"(<div class="ap-quiet">)" + rml_escape(tracker_summary()) + "</div>";
        }
        for (int r = 0; r < static_cast<int>(tracker_regions().size()); ++r) {
            std::string rows;
            size_t n = 0;
            for (const auto& c : g_trackerChecks) {
                if (c.region == r && check_state(c) == CheckState::InLogic) {
                    rows += check_row(c, CheckState::InLogic);
                    ++n;
                }
            }
            if (n > 0) {
                out += fmt::format(R"(<div class="ap-head">{} ({})</div>)",
                    rml_escape(tracker_regions()[r].label), n);
                out += rows;
            }
        }
        return out.empty() ? R"(<div class="ap-quiet">Nothing in logic right now.</div>)" : out;
    }
    const auto& regions = tracker_regions();
    if (region < 0 || region >= static_cast<int>(regions.size())) {
        return "";
    }
    out += fmt::format(R"(<div class="ap-head">{}</div>)", rml_escape(region_label(region)));
    for (const auto want :
        {CheckState::InLogic, CheckState::Unknown, CheckState::NotYet, CheckState::Skippable,
            CheckState::Done}) {
        for (const auto& c : g_trackerChecks) {
            if (c.region == region && check_state(c) == want) {
                out += check_row(c, want);
            }
        }
    }
    return out;
}

static float done_fraction() {
    const auto n = region_counts(-1);
    return n.total == 0 ? 0.0f : static_cast<float>(n.done) / static_cast<float>(n.total);
}

static ModResult build_region_pane(ModContext* ctx, UiElementHandle pane, void* ud, ModError*) {
    g_trackerShownRegion = static_cast<int>(reinterpret_cast<intptr_t>(ud));
    g_trackerList = 0;
    return svc_mng.ui->pane_add_rml(
        ctx, pane, region_rml(g_trackerShownRegion).c_str(), &g_trackerList);
}

ModResult build_tracker_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left,
    UiElementHandle right, void*, ModError*) {
    g_trackerSummary = g_trackerProgress = g_trackerList = 0;
    g_trackerGroups.clear();
    svc_mng.ui->pane_add_text(ctx, left, tracker_summary().c_str(), &g_trackerSummary);
    svc_mng.ui->pane_add_progress(ctx, left, done_fraction(), &g_trackerProgress);
    svc_mng.ui->pane_add_rml(ctx, left, kLegendRml, nullptr);

    UiGroupDesc now = UI_GROUP_DESC_INIT;
    now.label = "In logic now";
    now.build = build_region_pane;
    now.user_data = reinterpret_cast<void*>(static_cast<intptr_t>(kInLogicNow));
    svc_mng.ui->pane_add_group(ctx, left, right, &now, nullptr);

    svc_mng.ui->pane_add_section(ctx, left, "Regions");
    const auto& regions = tracker_regions();
    for (int r = 0; r < static_cast<int>(regions.size()); ++r) {
        if (region_counts(r).total == 0) {
            continue;
        }
        const std::string label = region_label(r);
        UiGroupDesc g = UI_GROUP_DESC_INIT;
        g.label = label.c_str();
        g.build = build_region_pane;
        g.user_data = reinterpret_cast<void*>(static_cast<intptr_t>(r));
        UiElementHandle elem = 0;
        svc_mng.ui->pane_add_group(ctx, left, right, &g, &elem);
        g_trackerGroups.emplace_back(elem, r);
    }

    // Open on what's in logic: the question mid-run is usually "what can I do now?".
    g_trackerShownRegion = kInLogicNow;
    svc_mng.ui->pane_add_rml(ctx, right, region_rml(kInLogicNow).c_str(), &g_trackerList);
    g_trackerShown = {g_trackerVersion, g_checked.size()};
    return MOD_OK;
}

ModResult update_tracker_tab(ModContext* ctx, void*, ModError*) {
    const std::pair<uint64_t, size_t> now{g_trackerVersion, g_checked.size()};
    if (now == g_trackerShown) {
        return MOD_OK;
    }
    g_trackerShown = now;
    if (g_trackerSummary != 0) {
        svc_mng.ui->elem_set_text(ctx, g_trackerSummary, tracker_summary().c_str());
    }
    if (g_trackerProgress != 0) {
        svc_mng.ui->elem_set_progress(ctx, g_trackerProgress, done_fraction());
    }
    for (const auto& [elem, region] : g_trackerGroups) {
        if (elem != 0) {
            svc_mng.ui->control_set_label(ctx, elem, region_label(region).c_str());
        }
    }
    if (g_trackerList != 0) {
        svc_mng.ui->elem_set_rml(ctx, g_trackerList, region_rml(g_trackerShownRegion).c_str());
    }
    return MOD_OK;
}

}  // namespace ap::internal
