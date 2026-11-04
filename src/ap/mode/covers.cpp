// The Covers tab: every game in the room, the cover it got, and the player's way to change it
// (search SteamGridDB, use the Sol, or put their own image in covers/custom).

#include "internal.hpp"

namespace ap::internal {
namespace {

std::vector<covers::GameCover> g_coverGames;  // as the list shows them
std::string g_coverSelected;                  // the game whose cover the right pane is about
std::string g_coverDraft;                     // the search box
uint64_t g_coverShown = 0;                    // covers::version() the tab last showed
UiElementHandle g_coverTabStatus = 0;
UiElementHandle g_coverDetails = 0;
UiElementHandle g_coverSearchText = 0;
UiListHandle g_coverList = 0;
UiListHandle g_coverResults = 0;

}  // namespace

// Used only in this file; defined below.
static uint64_t game_key(const std::string& game);
static std::string game_label(const covers::GameCover& g);
static std::string details_rml();
static std::string search_rml();
static void select_game(const std::string& game);
static void refresh_covers_tab(ModContext* ctx);
static bool no_game(ModContext*, void*);


static uint64_t game_key(const std::string& game) {
    return std::hash<std::string>{}(game) | 1u;  // list keys are never 0
}

static std::string game_label(const covers::GameCover& g) {
    switch (g.source) {
    case covers::Source::Pending: return g.game + "  ·  downloading";
    case covers::Source::Custom: return g.game + "  ·  your image";
    case covers::Source::Chosen: return g.game + "  ·  " + g.matched;
    case covers::Source::Automatic:
        return g.game + "  ·  " + (g.unsure ? "best guess: " : "") + g.matched;
    case covers::Source::Sol: return g.game + (g.solChosen ? "  ·  Sol (your choice)" : "  ·  no cover");
    }
    return g.game;
}

static std::string details_rml() {
    const auto it = std::find_if(g_coverGames.begin(), g_coverGames.end(),
        [](const covers::GameCover& g) { return g.game == g_coverSelected; });
    if (it == g_coverGames.end()) {
        return R"(<div class="ap-quiet">Pick a game on the left to change its cover.</div>)";
    }
    const covers::GameCover& g = *it;
    std::string out = fmt::format(R"(<div class="ap-head">{}</div>)", rml_escape(g.game));
    const std::string matched = rml_escape(g.matched);
    switch (g.source) {
    case covers::Source::Pending: out += "<div>Its cover is downloading.</div>"; break;
    case covers::Source::Custom:
        out += "<div>Using your own image from the covers/custom folder.</div>";
        break;
    case covers::Source::Chosen:
        out += fmt::format("<div>Using the art for <b>{}</b>, which you picked.</div>", matched);
        break;
    case covers::Source::Automatic:
        out += fmt::format("<div>Using the art for <b>{}</b>, found by name.</div>", matched);
        if (g.unsure) {
            out += R"(<div class="ap-trap">That's a best guess. If it's the wrong game, search )"
                   R"(for the right one below.</div>)";
        }
        break;
    case covers::Source::Sol:
        out += g.solChosen ? "<div>Its items show as Sols, as you asked.</div>"
               : g.matched.empty()
                   ? "<div>SteamGridDB didn't find it by name, so its items are Sols. Search "
                     "for it below.</div>"
                   : fmt::format("<div>No art for <b>{}</b>, so its items are Sols.</div>", matched);
        break;
    }
    out += fmt::format(R"(<div class="ap-quiet">Your own art instead: save a PNG or JPG as )"
                       R"(<b>{}</b> and press "Find it automatically". It wins over )"
                       R"(SteamGridDB.</div>)",
        rml_escape(covers::custom_path(g.game)));
    return out;
}

static std::string search_rml() {
    if (covers::searching()) {
        return R"(<div class="ap-quiet">Searching...</div>)";
    }
    if (!covers::search_message().empty()) {
        return R"(<div class="ap-quiet">)" + rml_escape(covers::search_message()) + "</div>";
    }
    if (!covers::results().empty()) {
        return R"(<div class="ap-quiet">Pick the right game:</div>)";
    }
    return "";
}

static void select_game(const std::string& game) {
    g_coverSelected = game;
    // Start the search from the name without platform and edition words: the one most likely
    // to find it.
    const std::string shorter = cover_art::fallback_term(game);
    g_coverDraft = shorter.empty() ? cover_art::search_term(game) : shorter;
    covers::search("");  // the last game's results don't apply to this one
}

static void refresh_covers_tab(ModContext* ctx) {
    g_coverShown = covers::version();
    g_coverGames = covers::games();
    if (g_coverTabStatus != 0) {
        svc_mng.ui->elem_set_text(ctx, g_coverTabStatus, covers::status().c_str());
    }
    if (g_coverList != 0) {
        std::vector<std::string> labels;
        labels.reserve(g_coverGames.size());
        std::vector<UiListItem> items;
        for (const auto& g : g_coverGames) {
            labels.push_back(game_label(g));
        }
        for (size_t i = 0; i < g_coverGames.size(); ++i) {
            UiListItem item = UI_LIST_ITEM_INIT;
            item.key = game_key(g_coverGames[i].game);
            item.label = labels[i].c_str();
            items.push_back(item);
        }
        svc_mng.ui->list_set_items(ctx, g_coverList, items.data(), items.size());
    }
    if (g_coverDetails != 0) {
        svc_mng.ui->elem_set_rml(ctx, g_coverDetails, details_rml().c_str());
    }
    if (g_coverSearchText != 0) {
        svc_mng.ui->elem_set_rml(ctx, g_coverSearchText, search_rml().c_str());
    }
    if (g_coverResults != 0) {
        std::vector<std::string> labels;
        std::vector<UiListItem> items;
        const auto& results = covers::results();
        labels.reserve(results.size());
        for (const auto& r : results) {
            labels.push_back(r.year != 0 ? fmt::format("{} ({})", r.name, r.year) : r.name);
        }
        for (size_t i = 0; i < results.size(); ++i) {
            UiListItem item = UI_LIST_ITEM_INIT;
            item.key = i + 1;
            item.label = labels[i].c_str();
            items.push_back(item);
        }
        svc_mng.ui->list_set_items(ctx, g_coverResults, items.data(), items.size());
    }
}

static bool no_game(ModContext*, void*) {
    return g_coverSelected.empty();
}

ModResult build_covers_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left,
    UiElementHandle right, void*, ModError*) {
    g_coverTabStatus = g_coverDetails = g_coverSearchText = 0;
    g_coverList = g_coverResults = 0;
    // No help_rml on this tab's controls: a control's help replaces the right pane, and that's
    // where the chosen game's cover and its search live.
    svc_mng.ui->pane_add_text(ctx, left, covers::status().c_str(), &g_coverTabStatus);
    svc_mng.ui->pane_add_section(ctx, left, "Games in this room");
    UiListDesc list = UI_LIST_DESC_INIT;
    list.on_pressed = [](ModContext* c, UiListHandle, uint64_t key, void*) {
        for (const auto& g : g_coverGames) {
            if (game_key(g.game) == key) {
                select_game(g.game);
            }
        }
        refresh_covers_tab(c);
    };
    list.is_selected = [](ModContext*, UiListHandle, uint64_t key, void*) {
        return !g_coverSelected.empty() && key == game_key(g_coverSelected);
    };
    svc_mng.ui->pane_add_list(ctx, left, &list, &g_coverList);

    svc_mng.ui->pane_add_rml(ctx, right, details_rml().c_str(), &g_coverDetails);
    UiControlDesc field = UI_CONTROL_DESC_INIT;
    field.kind = UI_CONTROL_STRING;
    field.label = "Search SteamGridDB for";
    field.max_length = 120;
    field.get = get_str;
    field.set = set_str;
    field.user_data = &g_coverDraft;
    field.string_set_mode = UI_STRING_SET_ON_CHANGE;
    field.is_disabled = no_game;
    svc_mng.ui->pane_add_control(ctx, right, &field, nullptr);
    UiControlDesc go = UI_CONTROL_DESC_INIT;
    go.kind = UI_CONTROL_BUTTON;
    go.label = "Search";
    go.on_pressed = [](ModContext*, void*) { covers::search(message_safe(g_coverDraft, 120)); };
    go.is_disabled = no_game;
    svc_mng.ui->pane_add_control(ctx, right, &go, nullptr);
    svc_mng.ui->pane_add_rml(ctx, right, search_rml().c_str(), &g_coverSearchText);
    UiListDesc results = UI_LIST_DESC_INIT;
    results.on_pressed = [](ModContext* c, UiListHandle, uint64_t key, void*) {
        const auto& r = covers::results();
        if (key >= 1 && key <= r.size() && !g_coverSelected.empty()) {
            covers::choose(g_coverSelected, r[key - 1].id, r[key - 1].name);
            covers::search("");
            refresh_covers_tab(c);
        }
    };
    svc_mng.ui->pane_add_list(ctx, right, &results, &g_coverResults);

    UiRowDesc rowDesc = UI_ROW_DESC_INIT;
    rowDesc.wrap = true;
    UiElementHandle row = 0;
    svc_mng.ui->pane_add_row(ctx, right, &rowDesc, &row);
    UiControlDesc sol = UI_CONTROL_DESC_INIT;
    sol.kind = UI_CONTROL_BUTTON;
    sol.label = "Use the Sol";
    sol.on_pressed = [](ModContext*, void*) { covers::use_sol(g_coverSelected); };
    sol.is_disabled = no_game;
    svc_mng.ui->pane_add_control(ctx, row, &sol, nullptr);
    UiControlDesc again = UI_CONTROL_DESC_INIT;
    again.kind = UI_CONTROL_BUTTON;
    again.label = "Find it automatically";
    again.on_pressed = [](ModContext*, void*) { covers::automatic(g_coverSelected); };
    again.is_disabled = no_game;
    svc_mng.ui->pane_add_control(ctx, row, &again, nullptr);

    refresh_covers_tab(ctx);
    return MOD_OK;
}

ModResult update_covers_tab(ModContext* ctx, void*, ModError*) {
    if (covers::version() != g_coverShown) {
        refresh_covers_tab(ctx);
    }
    return MOD_OK;
}

void forget_covers_tab() {
    g_coverTabStatus = g_coverDetails = g_coverSearchText = 0;
    g_coverList = g_coverResults = 0;
}

}  // namespace ap::internal
