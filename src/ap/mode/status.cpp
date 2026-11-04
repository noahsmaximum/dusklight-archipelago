// The Status tab and the Archipelago window that holds every tab.

#include "internal.hpp"

namespace ap::internal {
namespace {



}  // namespace

// Used only in this file; defined below.
static void press_reconnect(ModContext*, void*);
static void press_disconnect(ModContext*, void*);
static std::string status_text();
static ModResult build_status_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle, void*, ModError*);
static ModResult update_status_tab(ModContext* ctx, void*, ModError*);

static void press_reconnect(ModContext*, void*) {
    if (!g_editServer.empty() && g_editServer != g_conn.server) {
        g_conn.server = g_editServer;
        if (g_phase == Phase::Playing) {
            write_conn();
        }
    }
    if (!g_conn.slot.empty()) {
        g_client.connect({g_conn.server, g_conn.slot, g_conn.password});
    }
}

static void press_disconnect(ModContext*, void*) {
    g_client.disconnect();
}

static std::string status_text() {
    std::string s = status_line();
    if (g_phase == Phase::Playing) {
        s += fmt::format("\nItems received: {} / {}", g_state.received, g_serverItems.size());
        if (g_haveSlot) {
            s += fmt::format("\nChecks sent: {} / {}", g_checked.size(), g_locationIds.size());
        }
        if (g_state.transformAnywhere) {
            s += "\nTransform anywhere: on (from your YAML)";
        }
        if (g_state.goal) {
            s += "\nGoal complete!";
        }
        if (g_needsRegen) {
            s += "\nWaiting to rebuild this save's seed from the server.";
        }
    }
    return s;
}

static ModResult build_status_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle,
    void*, ModError*) {
    g_editServer = g_conn.server;
    svc_mng.ui->pane_add_section(ctx, left, "Connection");
    g_statusWindowText = 0;
    svc_mng.ui->pane_add_text(ctx, left, status_text().c_str(), &g_statusWindowText);
    UiControlDesc d = UI_CONTROL_DESC_INIT;
    d.kind = UI_CONTROL_STRING;
    d.label = "Server";
    d.help_rml = "Change this if the room moved to a new port, then press Reconnect.";
    d.get = get_str;
    d.set = set_str;
    d.user_data = &g_editServer;
    svc_mng.ui->pane_add_control(ctx, left, &d, nullptr);
    UiControlDesc b = UI_CONTROL_DESC_INIT;
    b.kind = UI_CONTROL_BUTTON;
    b.label = "Reconnect";
    b.on_pressed = press_reconnect;
    svc_mng.ui->pane_add_control(ctx, left, &b, nullptr);
    UiControlDesc goal = UI_CONTROL_DESC_INIT;
    goal.kind = UI_CONTROL_BUTTON;
    goal.label = "Send goal complete";
    goal.help_rml = "Tells the server you finished the game, in case it wasn't detected.";
    goal.on_pressed = [](ModContext*, void*) { complete_goal("manual"); };
    goal.is_disabled = [](ModContext*, void*) {
        return g_state.goal || g_client.state() != State::Connected;
    };
    svc_mng.ui->pane_add_control(ctx, left, &goal, nullptr);
    UiControlDesc dl = UI_CONTROL_DESC_INIT;
    dl.kind = UI_CONTROL_TOGGLE;
    dl.label = "Death link";
    dl.help_rml = "When anyone else with death link dies, so do you, and the other way round. "
                  "Starts from your YAML; changing it here sticks to this save.";
    dl.get = [](ModContext*, void*, UiControlValue* out) { out->bool_value = death_link_on(); };
    dl.set = [](ModContext*, void*, const UiControlValue* v) {
        g_state.deathLink = v->bool_value ? 1 : 0;
        write_state();
        apply_death_link_tags();
    };
    dl.is_disabled = [](ModContext*, void*) { return g_phase != Phase::Playing; };
    svc_mng.ui->pane_add_control(ctx, left, &dl, nullptr);
    UiControlDesc b2 = UI_CONTROL_DESC_INIT;
    b2.kind = UI_CONTROL_BUTTON;
    b2.label = "Disconnect";
    b2.on_pressed = press_disconnect;
    svc_mng.ui->pane_add_control(ctx, left, &b2, nullptr);

    svc_mng.ui->pane_add_section(ctx, left, "Tracker");
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Strike out finished players' checks",
        g_cfgSkipFinished,
        "Checks holding an item for a player who has already finished their game are struck "
        "out in the tracker and left off the in-logic list: nobody needs those items any more.");

    svc_mng.ui->pane_add_section(ctx, left, "Other players' items");
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Show as game boxes", g_cfgBoxes,
        "Items that belong to other worlds show as the box of the game they're for, with its "
        "cover art, instead of a Sol. Covers come from SteamGridDB with your own API key and "
        "download once per game. Games without art stay Sols.");
    add_bound(ctx, left, UI_CONTROL_STRING, "SteamGridDB API key", g_cfgSgdbKey,
        "Free: sign in at steamgriddb.com, then Preferences, API, and copy your key here. "
        "It's only sent to SteamGridDB. Leave it empty and items stay Sols.");
    g_coverStatusText = 0;
    svc_mng.ui->pane_add_text(ctx, left, covers::status().c_str(), &g_coverStatusText);
    UiControlDesc again = UI_CONTROL_DESC_INIT;
    again.kind = UI_CONTROL_BUTTON;
    again.label = "Download covers again";
    again.help_rml = "Forgets the saved covers (and which games had none) and downloads them "
                     "again; covers you picked stay picked. To pick a different cover for a "
                     "game, or use your own art, go to the Covers tab.";
    again.on_pressed = [](ModContext*, void*) { covers::refetch(); };
    svc_mng.ui->pane_add_control(ctx, left, &again, nullptr);
    return MOD_OK;
}

static ModResult update_status_tab(ModContext* ctx, void*, ModError*) {
    if (g_coverStatusText != 0) {
        static std::string shownCovers;
        const std::string now = covers::status();
        if (now != shownCovers) {
            shownCovers = now;
            svc_mng.ui->elem_set_text(ctx, g_coverStatusText, now.c_str());
        }
    }
    if (g_statusWindowText != 0) {
        static std::string shown;
        const std::string now = status_text();
        if (now != shown) {
            shown = now;
            svc_mng.ui->elem_set_text(ctx, g_statusWindowText, now.c_str());
        }
    }
    return MOD_OK;
}

void open_status_window(ModContext*, void*) {
    static UiTabDesc tabs[6] = {UI_TAB_DESC_INIT, UI_TAB_DESC_INIT, UI_TAB_DESC_INIT,
        UI_TAB_DESC_INIT, UI_TAB_DESC_INIT, UI_TAB_DESC_INIT};
    tabs[0].title = "Status";
    tabs[0].build = build_status_tab;
    tabs[0].update = update_status_tab;
    tabs[1].title = "Tracker";
    tabs[1].build = build_tracker_tab;
    tabs[1].update = update_tracker_tab;
    tabs[2].title = "Hints";
    tabs[2].build = build_hints_tab;
    tabs[2].update = update_hints_tab;
    tabs[3].title = "Messages";
    tabs[3].build = build_messages_tab;
    tabs[3].update = update_messages_tab;
    tabs[4].title = "Overlay";
    tabs[4].build = build_overlay_tab;
    tabs[5].title = "Covers";
    tabs[5].build = build_covers_tab;
    tabs[5].update = update_covers_tab;
    // The picker's buttons show their emoji as a background image.
    static const std::string rcss = [] {
        std::string out = kWindowRcss;
        out += ".ap-emoji-btn { padding-left: 42dp; font-size: 15dp; text-align: left; }\n";
        out += ".ap-emoji-glyph { font-size: 26dp; min-width: 52dp; padding: 4dp 8dp; }\n";
        for (const auto& e : emoji::picker()) {
            out += fmt::format(".ap-e-{} {{ decorator: image({} contain left center); }}\n", e.file,
                emoji::image_source(e.file));
        }
        return out;
    }();
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 6;
    desc.rcss = rcss.c_str();
    desc.on_closed = [](ModContext*, UiWindowHandle, void*) {
        g_statusWindow = 0;
        g_statusWindowText = 0;
        g_coverStatusText = 0;
        g_trackerSummary = g_trackerProgress = g_trackerList = 0;
        g_trackerGroups.clear();
        g_logElem = 0;
        g_hintsSummary = g_hintDetails = 0;
        g_hintsList = 0;
        forget_covers_tab();
    };
    svc_mng.ui->window_push(svc_mng.mod_ctx, &desc, &g_statusWindow);
}

}  // namespace ap::internal
