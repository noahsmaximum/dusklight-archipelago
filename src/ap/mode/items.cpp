// Checks and items: the client callbacks, the item resolver and give observer, sending checks, delivering received items, Collect Dungeon on Completion, and the goal.

#include "internal.hpp"

namespace ap::internal {

// Used only in this file; defined below.
static int hint_rank(const Hint& h);
static void report_locations(const std::vector<std::string>& locs);

void on_connected(const json& p) {
    const json slotData = p.value("slot_data", json::object());
    std::string err;
    if (!load_slot_data(slotData, err)) {
        g_status = err;
        toast("Archipelago", err, "warning");
        g_client.disconnect();
        return;
    }
    g_lastSlotData = slotData;
    const json deathLink = slotData.value("death_link", json(false));
    g_deathLinkSlot = deathLink.is_boolean() ? deathLink.get<bool>()
                                             : deathLink.is_number() && deathLink.get<int>() != 0;
    apply_death_link_tags();
    const json settings = slotData.value("settings", json::object());
    g_transformAnywhereSlot = settings.value("Logic Transform Anywhere", std::string{}) == "On";
    if (g_phase == Phase::Playing && g_state.transformAnywhere != g_transformAnywhereSlot) {
        g_state.transformAnywhere = g_transformAnywhereSlot;
        write_state();
    }
    want_covers();
    g_checked.clear();
    for (const auto& id : p.value("checked_locations", json::array())) {
        if (id.is_number_integer()) {
            g_checked.insert(id.get<int64_t>());
        }
    }
    g_reachInputs = SIZE_MAX;
    ++g_trackerVersion;

    ap_log(fmt::format("connected in phase {}: {} locations, {} ap placements",
        static_cast<int>(g_phase), g_locationIds.size(), g_apItemText.size()));
    if (g_phase == Phase::NewSave) {
        g_status = "Connected. Building your seed...";
        g_phase = Phase::Generating;
        start_generation(slotData, g_client.info().slot);
        return;
    }
    if (g_phase == Phase::Playing) {
        if (!g_state.seed.empty() && g_state.seed != g_slotSeed) {
            toast("Archipelago", "This save belongs to a different multiworld seed. Disconnected.",
                "warning", 8000);
            g_client.disconnect();
            g_haveSlot = false;
            return;
        }
        if (g_needsRegen) {
            toast("Archipelago", "Rebuilding this save's seed from the server...");
            g_phase = Phase::Generating;
            start_generation(slotData, g_client.info().slot);
            return;
        }
        g_client.sendSync();  // also proves the transport can send after the handshake
        toast("Archipelago", status_line(), "success", 3000);
        if (g_state.goal) {
            g_client.sendGoal();
        }
    }
}

void on_items(int index, const std::vector<NetworkItem>& items) {
    if (index == 0) {
        g_serverItems.clear();
    }
    if (static_cast<size_t>(index) != g_serverItems.size()) {
        g_client.sendSync();  // gap: ask for the full list again
        return;
    }
    g_serverItems.insert(g_serverItems.end(), items.begin(), items.end());
}

static int hint_rank(const Hint& h) {
    if (h.found) {
        return 4;
    }
    switch (h.status) {
    case 30: return 0;  // priority
    case 0: return 1;   // unspecified
    case 10: return 2;  // no priority
    default: return 3;  // avoid
    }
}

void on_hints(const std::vector<Hint>& hints) {
    g_hints = hints;
    std::stable_sort(g_hints.begin(), g_hints.end(),
        [](const Hint& a, const Hint& b) { return hint_rank(a) < hint_rank(b); });
    ++g_hintsVersion;
}

void on_print(const std::string& text, const json& msg) {
    log_rml(print_rml(msg.value("data", json::array()), g_client));
    const std::string type = msg.value("type", "");
    const int me = g_client.slot();
    if (type == "Hint" && !msg.value("found", false)) {
        const int receiver = json_num(msg, "receiving", -1);
        const int finder = msg.contains("item") ? json_num(msg["item"], "player", -1) : -1;
        if (receiver == me || finder == me) {
            toast("Hint", text, nullptr, 6000);
        }
        return;
    }
    if (type == "ItemSend" || type == "ItemCheat") {
        const int receiver = json_num(msg, "receiving", -1);
        const int sender = msg.contains("item") ? json_num(msg["item"], "player", -1) : -1;
        if (receiver == me || sender == me) {
            if (receiver == me && sender == me) {
                return;  // our own item at our own location: the game already showed it
            }
            toast("Archipelago", text, nullptr, 4000);
        }
    } else if (type == "Goal" || type == "Release" || type == "Collect" || type == "Countdown") {
        toast("Archipelago", text, nullptr, 4000);
    } else if (type == "Chat" || type == "ServerChat") {
        toast("Archipelago", text, nullptr, 5000);
        g_recentChat.push_back({text, {}, std::chrono::steady_clock::now()});
        while (g_recentChat.size() > 8) {
            g_recentChat.pop_front();
        }
    }
}

void on_disconnected(const std::string& reason) {
    log_rml("<span class=\"ap-quiet\">" + rml_escape(message_safe("Disconnected: " + reason, 300)) +
            "</span>");
    if (g_phase == Phase::NewSave || g_phase == Phase::Generating) {
        g_status = "Could not connect: " + reason;
    } else if (g_phase == Phase::Playing) {
        toast("Archipelago", "Disconnected: " + reason, "warning", 4000);
    }
}


// ---------------------------------------------------------------------------------------
// ItemService callbacks

bool resolve_check(ModContext*, const ItemCheckInfo* info, ItemCheckResolution* out, void*) {
    // Collect Dungeon on Completion already handed this check over: what's left is a green
    // rupee, so going back for it doesn't give its item a second time.
    if (!g_redeemedLocations.empty() && info->name != nullptr) {
        const auto locs = locations_for_check(info->name);
        if (!locs.empty() && g_redeemedLocations.contains(locs.front())) {
            out->item = dItemNo_Randomizer_GREEN_RUPEE_e;
            out->display_item = dItemNo_Randomizer_GREEN_RUPEE_e;
            out->was_resolved = true;
            return true;
        }
    }
    // Runs after the randomizer's resolver; only remembers where the last AP item came from.
    if (info->current_item == kApItem) {
        const auto locs = locations_for_check(info->name);
        if (!locs.empty()) {
            g_lastResolvedApLocation = locs.front();
            if (info->giver_actor != nullptr) {
                g_actorLocation[info->giver_actor] = locs.front();  // for its box (see draw)
            }
            // A new pickup: the next held item is this one, unless its give tag says otherwise.
            g_heldActor = nullptr;
        }
    }
    return false;
}

static void report_locations(const std::vector<std::string>& locs) {
    for (const auto& loc : locs) {
        const auto it = g_locationIds.find(loc);
        ap_log(fmt::format("report_location '{}' known={}", loc, it != g_locationIds.end()));
        {
            if (!g_checked.contains(it->second)) {
                g_toSend.push_back(it->second);
                g_checked.insert(it->second);
            }
        }
    }
}

void observe_give(ModContext*, const ItemGiveInfo* info, void*) {
    std::string where = "(none)";
    std::string expected = "-";
    if (info->check_name != nullptr) {
        where = info->check_name;
        const auto locs = locations_for_check(info->check_name);
        if (!locs.empty()) {
            where += " -> " + locs.front();
            if (const auto e = g_expected.find(locs.front()); e != g_expected.end()) {
                expected = e->second;
            }
        }
    }
    ap_log(fmt::format("give item=0x{:02X} ({}) origin={} check='{}' ap_expects='{}'", info->item,
        item_name(info->item), info->origin, where, expected));
    if (info->check_name != nullptr) {
        const auto locs = locations_for_check(info->check_name);
        report_locations(locs);
        if (info->item == kApItem && !locs.empty()) {
            g_lastResolvedApLocation = locs.front();
        }
        return;
    }
    if ((info->origin == ITEM_GIVE_ORIGIN_QUEUE || info->origin == ITEM_GIVE_ORIGIN_QUEUE_SILENT) &&
        g_redeemPending > 0)
    {
        --g_redeemPending;  // a collected dungeon's item, not a server item
        return;
    }
    if ((info->origin == ITEM_GIVE_ORIGIN_QUEUE || info->origin == ITEM_GIVE_ORIGIN_QUEUE_SILENT) &&
        g_outstanding >= 0)
    {
        g_outstanding = -1;
        ++g_state.received;
        write_state();
    }
}

bool ap_item_text(ModContext*, const MessageOverrideContext*, MessageTextData* out, void*) {
    if (g_armedFrames <= 0 || g_armedText.empty()) {
        return false;
    }
    // Sanitized again at the sink: whatever the source, the renderer only ever sees printable
    // text of a sane length.
    const std::string text = message_safe(g_armedText, 160);
    thread_local std::vector<uint8_t> buf;
    buf.assign(text.begin(), text.end());
    buf.push_back(0);
    out->text = buf.data();
    out->text_size = buf.size();
    // Consume it: the next Archipelago item re-arms with its own text, and a real Foolish
    // Item (which shares this message) must not inherit it.
    g_armedFrames = 0;
    g_apTextServed = true;
    return true;
}

void complete_goal(const char* how) {
    if (g_state.goal) {
        return;
    }
    g_state.goal = true;
    write_state();
    g_client.sendGoal();
    ap_log(fmt::format("goal complete ({})", how));
    toast("Archipelago", "Goal complete!", "success", 6000);
}

void post_ganondorf_execute(ModContext*, void* args, void*, void*) {
    // The ending plays out inside the arena (no stage or scene change), so watch Ganondorf
    // himself: the finishing blow puts him in the end action with the ending camera running.
    constexpr s16 kActionEnd = 22;   // ACTION_END in d_a_b_gnd.cpp
    constexpr s16 kEndingCamera = 60;
    auto* gnd = mods::arg<b_gnd_class*>(args, 0);
    if (gnd != nullptr && gnd->mActionMode == kActionEnd && gnd->mDemoCamMode >= kEndingCamera) {
        complete_goal("Ganondorf defeated");
    }
}

HookAction pre_change_scene(ModContext*, void*, void*, void*) {
    const char* stage = dComIfGp_getStartStageName();
    ap_log(fmt::format("changeScene from stage '{}'", stage != nullptr ? stage : "?"));
    // Leaving Dark Lord Ganondorf's arena only happens through the ending.
    if (g_phase == Phase::Playing && stage != nullptr && std::strcmp(stage, "D_MN09C") == 0) {
        complete_goal("left D_MN09C");
    }
    return HOOK_CONTINUE;
}

void log_stage_changes() {
    static std::string last;
    const char* stage = dComIfGp_getStartStageName();
    if (stage != nullptr && last != stage) {
        last = stage;
        ap_log(fmt::format("stage -> {}", last));
    }
}

void scan_locations() {
    if (!g_haveSlot || g_scan.empty() || !in_gameplay()) {
        return;
    }
    for (int n = 0; n < 24; ++n) {
        const auto& loc = g_scan[g_scanPos];
        g_scanPos = (g_scanPos + 1) % g_scan.size();
        if (g_checked.contains(loc.id)) {
            continue;
        }
        if (loc.meta.IsMap() && isLocationMetadataObtained(loc.meta, loc.name)) {
            g_toSend.push_back(loc.id);
            g_checked.insert(loc.id);
        }
    }
}

void flush_checks() {
    if (!g_toSend.empty() && g_client.state() == State::Connected) {
        ap_log(fmt::format("sending {} location check(s)", g_toSend.size()));
        g_client.sendLocations(g_toSend);
        g_toSend.clear();
    }
}


// Collect Dungeon on Completion: once a dungeon's boss is beaten (its reward or the boss's
// heart container is checked, here or from the server), every check left in it is sent, and
// our own items among them are handed over quietly with one summary.
void tick_dungeon_collect() {
    if (g_collectDungeons.empty() || !in_gameplay() || g_outstanding >= 0 || g_redeemPending > 0) {
        return;
    }
    for (const auto& d : g_collectDungeons) {
        if (std::find(g_state.redeemedDungeons.begin(), g_state.redeemedDungeons.end(), d.name) !=
            g_state.redeemedDungeons.end()) {
            continue;
        }
        const bool beaten = std::any_of(d.triggers.begin(), d.triggers.end(), [](const auto& t) {
            const auto it = g_locationIds.find(t);
            return it != g_locationIds.end() && g_checked.contains(it->second);
        });
        if (!beaten) {
            continue;
        }
        int sent = 0;
        std::vector<std::string> mine;
        for (const auto& [loc, item] : d.locations) {
            const auto id = g_locationIds.find(loc);
            if (id == g_locationIds.end() || g_checked.contains(id->second)) {
                continue;
            }
            g_toSend.push_back(id->second);
            g_checked.insert(id->second);
            g_state.redeemedLocations.push_back(loc);
            g_redeemedLocations.insert(loc);
            ++sent;
            // Our own item: the chest won't give it now, so hand it over. A Foolish Item stays
            // in the chest's place as nothing; it would only hurt.
            if (item > 0 && item <= 0xFE && item != dItemNo_Randomizer_FOOLISH_ITEM_e) {
                const auto give = static_cast<uint8_t>(verifyProgressiveItem(static_cast<u32>(item)));
                if (svc_mng.item->give_item(svc_mng.mod_ctx, nullptr, give, ITEM_GIVE_SILENT) == MOD_OK) {
                    ++g_redeemPending;
                    mine.push_back(item_name(give));
                }
            }
        }
        g_state.redeemedDungeons.push_back(d.name);
        write_state();
        ap_log(fmt::format("collected {}: {} checks, {} own items", d.name, sent, mine.size()));
        if (sent > 0) {
            std::string body = fmt::format("{} cleared: collected {} check{}.", d.name, sent,
                sent == 1 ? "" : "s");
            if (!mine.empty()) {
                body += " Yours: ";
                for (size_t i = 0; i < mine.size() && i < 4; ++i) {
                    body += (i ? ", " : "") + mine[i];
                }
                if (mine.size() > 4) {
                    body += fmt::format(" and {} more", mine.size() - 4);
                }
                body += ".";
            }
            toast("Archipelago", body, "success", 7000);
        }
        g_reachInputs = SIZE_MAX;
        ++g_trackerVersion;
        return;  // one dungeon per tick; the rest follow once these gives are confirmed
    }
}

void deliver_items() {
    if (!in_gameplay() || g_outstanding >= 0 || g_redeemPending > 0 ||
        g_state.received >= static_cast<int>(g_serverItems.size()))
    {
        return;
    }
    const auto& it = g_serverItems[g_state.received];
    const int64_t id = it.item - kItemIdBase;
    if (id < 0 || id > 0xFE) {
        ++g_state.received;  // not a giveable item for this game; skip
        write_state();
        return;
    }
    const auto give = static_cast<uint8_t>(verifyProgressiveItem(static_cast<u32>(id)));
    const bool progression = (it.flags & 1) != 0;
    const bool fromOther = it.player != g_client.slot() && it.location >= 0;
    const uint32_t flags = (progression && fromOther) ? 0u : ITEM_GIVE_SILENT;
    if (svc_mng.item->give_item(svc_mng.mod_ctx, nullptr, give, flags) != MOD_OK) {
        return;
    }
    g_outstanding = give;
    {
        const std::string sender = g_client.playerName(it.player);
        g_recentItems.push_back({item_name(give), fromOther ? sender : std::string{},
            std::chrono::steady_clock::now()});
        while (g_recentItems.size() > 8) {
            g_recentItems.pop_front();
        }
    }
    if (flags & ITEM_GIVE_SILENT) {
        const std::string from = g_client.playerName(it.player);
        toast("Received item", fmt::format("{}{}", item_name(give),
                                   fromOther && !from.empty() ? " from " + from : ""),
            nullptr, 3000);
    }
}

}  // namespace ap::internal
