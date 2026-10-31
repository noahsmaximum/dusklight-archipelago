#include "ap_mode.hpp"

// The Archipelago game mode's public face: registration, activation, the per-frame entry
// points, and the hooks it installs. The mode itself lives in src/ap/mode/ (see
// mode/internal.hpp for its shared state).

#include "mode/internal.hpp"

namespace ap {

using namespace ap::internal;

// ---------------------------------------------------------------------------------------
// Hooks the mode installs (in activate below); their handlers are in mode/*.cpp

DEFINE_HOOK(&daDitem_c::set_mtx, ApDitemSetMtx);
DEFINE_HOOK(&daItem_c::setBaseMtx, ApItemSetBaseMtx);
DEFINE_HOOK(dStage_changeScene, ApChangeScene);
DEFINE_HOOK(&daAlink_c::procCoDeadInit, ApLinkDeadInit);
DEFINE_HOOK(&daAlink_c::procCoFogDeadInit, ApLinkFogDeadInit);
// Ganondorf's execute is file-local; hook it by translation unit alias.
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_b_gnd.cpp#daB_GND_Execute", int(b_gnd_class*), ApGanondorf);
// Midna's "is an NPC watching?" search is file-local too (Transform Anywhere, mode/hooks.cpp).
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_midna.cpp#daMidna_searchNpc", void*(fopAc_ac_c*, void*),
    ApMidnaSearchNpc);
DEFINE_HOOK(&dMsgFlow_c::query042, ApMsgQuery042);
DEFINE_HOOK(&dMeter2Draw_c::draw, ApMeter2Draw);
// Virtual, so hooked by its exact symbol: every item actor (ground, held up, shop, heart
// container, key) draws through it.
#ifdef _MSVC_LANG
DEFINE_HOOK_SYMBOL("?DrawBase@daItemBase_c@@UEAAHXZ", int(daItemBase_c*), ApItemDrawBase);
DEFINE_HOOK_SYMBOL("?exec@dMsgScrnItem_c@@UEAAXXZ", void(dMsgScrnItem_c*), ApMsgItemExec);
#else
DEFINE_HOOK_SYMBOL("_ZN12daItemBase_c8DrawBaseEv", int(daItemBase_c*), ApItemDrawBase);
DEFINE_HOOK_SYMBOL("_ZN14dMsgScrnItem_c4execEv", void(dMsgScrnItem_c*), ApMsgItemExec);
#endif


// =========================================================================================

GameModeDesc game_mode_desc() {
    return GameModeDesc{
        .struct_size = sizeof(GameModeDesc),
        .game_mode_id = kGameModeId,
        .full_name = "Archipelago",
        .save_name = "archipelago",
        .user_data = nullptr,
        .on_save_loaded = on_save_loaded,
        .on_new_save = on_new_save,
        .on_game_reset = on_game_reset,
    };
}

ModResult activate() {
    g_client.onConnected = on_connected;
    g_client.onItems = on_items;
    g_client.onHints = on_hints;
    g_client.onPrint = on_print;
    g_client.onDisconnected = on_disconnected;
    g_client.onBounced = on_bounced;

    auto reg_string = [](const char* name, ConfigVarHandle& out) {
        ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
        d.name = name;
        d.type = CONFIG_VAR_STRING;
        d.default_string = "";
        svc_mng.config->register_var(svc_mng.mod_ctx, &d, &out);
    };
    if (g_cfgServer == 0) {
        reg_string("lastServer", g_cfgServer);
        reg_string("lastSlot", g_cfgSlot);
        ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
        d.name = "apItemModelScale";
        d.type = CONFIG_VAR_FLOAT;
        d.default_float = 0.3;
        svc_mng.config->register_var(svc_mng.mod_ctx, &d, &g_cfgModelScale);
        ConfigVarDesc debug = CONFIG_VAR_DESC_INIT;
        debug.name = "debugLog";
        debug.type = CONFIG_VAR_BOOL;
        debug.default_bool = false;
        svc_mng.config->register_var(svc_mng.mod_ctx, &debug, &g_cfgDebugLog);
        register_overlay_vars();
        ConfigVarDesc boxes = CONFIG_VAR_DESC_INIT;
        boxes.name = "gameBoxes";
        boxes.type = CONFIG_VAR_BOOL;
        boxes.default_bool = true;
        svc_mng.config->register_var(svc_mng.mod_ctx, &boxes, &g_cfgBoxes);
        reg_string("steamGridDbKey", g_cfgSgdbKey);
        ConfigVarDesc choice = CONFIG_VAR_DESC_INIT;
        choice.name = "gameBoxChoice";
        choice.type = CONFIG_VAR_INT;
        choice.default_int = 0;
        svc_mng.config->register_var(svc_mng.mod_ctx, &choice, &g_cfgBoxChoice);
        ConfigVarDesc skip = CONFIG_VAR_DESC_INIT;
        skip.name = "skipFinishedChecks";
        skip.type = CONFIG_VAR_BOOL;
        skip.default_bool = true;
        svc_mng.config->register_var(svc_mng.mod_ctx, &skip, &g_cfgSkipFinished);
    }

    svc_mng.item->observe_gives(svc_mng.mod_ctx, observe_give, nullptr, &g_observer);

    if (mods::hook::add_post<ApDitemSetMtx>(post_ditem_set_mtx) != MOD_OK ||
        mods::hook::add_post<ApItemSetBaseMtx>(post_item_set_base_mtx) != MOD_OK ||
        mods::hook::add_post<ApGanondorf>(post_ganondorf_execute) != MOD_OK ||
        mods::hook::add_pre<ApChangeScene>(pre_change_scene) != MOD_OK)
    {
        mods::log::error("archipelago: failed to install hooks");
        return MOD_ERROR;
    }
    // Separate from the hooks above: if these ever fail to resolve on a future Dusklight,
    // lose death link rather than the whole mode.
    if (mods::hook::add_post<ApLinkDeadInit>(post_link_dead_init) != MOD_OK ||
        mods::hook::add_post<ApLinkFogDeadInit>(post_link_fog_dead_init) != MOD_OK)
    {
        mods::log::error("archipelago: death link hooks failed to install; deaths won't be sent");
    }
    if (mods::hook::add_post<ApMidnaSearchNpc>(post_midna_search_npc) != MOD_OK ||
        mods::hook::add_post<ApMsgQuery042>(post_msg_query042) != MOD_OK)
    {
        mods::log::error("archipelago: transform anywhere hooks failed to install; turn on "
                         "Dusklight's Can Transform Anywhere cheat instead");
    }
    g_emojiFont = load_emoji_font();
    if (mods::hook::add_post<ApMeter2Draw>(post_meter2_draw) != MOD_OK) {
        mods::log::error("archipelago: overlay hook failed to install; the overlay won't show");
    }
    if (mods::hook::add_pre<ApItemDrawBase>(pre_item_draw_base) != MOD_OK) {
        mods::log::error("archipelago: item draw hook failed to install; items stay Sols");
    }
    if (mods::hook::add_post<ApMsgItemExec>(post_msg_item_exec) != MOD_OK) {
        mods::log::error("archipelago: item text box hook failed to install; its icon stays");
    }

    UiMenuTabDesc tab = UI_MENU_TAB_DESC_INIT;
    tab.label = "Archipelago";
    tab.on_selected = open_status_window;
    svc_mng.ui->register_menu_tab(svc_mng.mod_ctx, &tab, &g_menuTab);
    return MOD_OK;
}

void deactivate() {
    g_client.disconnect();
    reset_tracker();
    g_log.clear();
    g_hints.clear();
    g_selectedHint = 0;
    ++g_hintsVersion;
    g_textOverrides.clear();
    if (g_resolver != 0) {
        svc_mng.item->clear_check_resolver(svc_mng.mod_ctx, g_resolver);
        g_resolver = 0;
    }
    if (g_observer != 0) {
        svc_mng.item->unobserve_gives(svc_mng.mod_ctx, g_observer);
        g_observer = 0;
    }
    mods::hook::uninstall<ApDitemSetMtx>();
    mods::hook::uninstall<ApItemSetBaseMtx>();
    mods::hook::uninstall<ApChangeScene>();
    mods::hook::uninstall<ApGanondorf>();
    mods::hook::uninstall<ApLinkDeadInit>();
    mods::hook::uninstall<ApLinkFogDeadInit>();
    mods::hook::uninstall<ApMidnaSearchNpc>();
    mods::hook::uninstall<ApMsgQuery042>();
    mods::hook::uninstall<ApMeter2Draw>();
    mods::hook::uninstall<ApItemDrawBase>();
    mods::hook::uninstall<ApMsgItemExec>();
    g_actorLocation.clear();
    g_pendingDeath.reset();
    g_killFrames = 0;
    g_deathSent = false;
    g_deathLinkSlot = false;
    g_transformAnywhereSlot = false;
    if (g_menuTab != 0) {
        svc_mng.ui->unregister_menu_tab(svc_mng.mod_ctx, g_menuTab);
        g_menuTab = 0;
    }
    g_phase = Phase::Idle;
}

void update() {
    g_client.poll();
    tick_generation();
}

void tick() {
    if (g_armedFrames > 0) {
        --g_armedFrames;
    }

    if (g_phase == Phase::Playing) {
        // Keep an AP save connected: retry every ~10 s after a drop (not after a refusal).
        if (g_client.state() == State::Disconnected && !g_conn.slot.empty()) {
            if (++g_reconnectFrames > 600) {
                g_reconnectFrames = 0;
                g_client.connect({g_conn.server, g_conn.slot, g_conn.password});
            }
        } else {
            g_reconnectFrames = 0;
        }
        log_stage_changes();
        scan_locations();
        tick_dungeon_collect();
        flush_checks();
        deliver_items();
        tick_death_link();
    }
    tick_tracker();
    maybe_prompt_boxes();
    covers::tick(trim_key(config_string(g_cfgSgdbKey)), cfg_on(g_cfgBoxes));
    box::tick();
}

ModResult open_connect_gate(void* fileSelect) {
    return open_gate_window(fileSelect);
}

// The location of the item Link is getting right now: the item event partner's give tag
// names it; otherwise the last check resolved to an AP item.
static std::string pickup_location(void* link) {
    const std::string own =
        location_of_actor(fopAcM_getItemEventPartner(static_cast<fopAc_ac_c*>(link)));
    return own.empty() ? g_lastResolvedApLocation : own;
}

ItemImportance ap_item_importance(void* link) {
    // Unknown counts as progression, the fanfare it always had.
    const auto it = g_placementFlags.find(pickup_location(link));
    const int flags = it != g_placementFlags.end() ? it->second : 1;
    if (flags & 1) {
        return ItemImportance::Progression;
    }
    if (flags & 2) {
        return ItemImportance::Useful;
    }
    if (flags & 4) {
        return ItemImportance::Trap;
    }
    return ItemImportance::Filler;
}

void on_get_item_demo(void* link) {
    auto* alink = static_cast<daAlink_c*>(link);
    if (alink->field_0x32cc != 0) {
        return;
    }
    if (alink->mProcVar2.field_0x300c != kApItem) {
        if (alink->mProcVar2.field_0x300c == dItemNo_Randomizer_FOOLISH_ITEM_e) {
            g_armedFrames = 0;  // a real Foolish Item uses the same message
        }
        // Any other pickup's text box is its own, even at the address ours had.
        g_apTextServed = false;
        g_apTextBox = nullptr;
        return;
    }
    // Always re-arm from the check being collected right now; the placeholder item has no
    // message of its own, so it must never fall back to whatever was shown last.
    g_armedText.clear();
    const std::string location = pickup_location(link);
    if (!location.empty()) {
        if (const auto it = g_apItemText.find(location); it != g_apItemText.end()) {
            g_armedText = it->second;
        }
    }
    if (g_armedText.empty()) {
        g_armedText = "You found another player's item!";
    }
    g_heldLocation = location;
    g_armedFrames = 600;
    g_apTextServed = false;  // this pickup's text box claims it when the text is served
    g_apTextBox = nullptr;
    alink->field_0x32cc = kApItemDonorMessage;
}

}  // namespace ap
