// Game boxes in the mode: which game an item belongs to, the item draw and text box hooks, and the first-run boxes-or-Sols prompt. Drawing is ap_box, covers ap_covers.

#include "internal.hpp"

namespace ap::internal {
namespace {


// First run: game boxes (with a SteamGridDB key) or Sols. Asked once, on the connection
// window or in play, and never again once answered (or once a key is set some other way).
std::string g_keyDraft;
bool g_boxPromptShown = false;

}  // namespace

// Used only in this file; defined below.
static std::string game_for_location(const std::string& location);
static std::string game_for_actor(const void* actor);
static void choose_boxes(ModContext*, UiDialogHandle, void*);
static void choose_sols(ModContext*, UiDialogHandle, void*);
static void prompt_boxes();


// The game that owns the AP item at a location, or "".
static std::string game_for_location(const std::string& location) {
    const auto owner = g_placementOwner.find(location);
    return owner != g_placementOwner.end() ? g_client.gameOfSlotName(owner->second) : std::string{};
}


// The game that owns the AP item an actor shows, or "" when we can't tell.
static std::string game_for_actor(const void* actor) {
    std::string location;
    if (fopAcM_GetName(const_cast<void*>(actor)) == fpcNm_Demo_Item_e) {
        // Held up: its give tag names its check. Without one we know, the pickup resolved just
        // before this actor appeared (see g_heldActor).
        const uint32_t tag = static_cast<const fopAc_ac_c*>(actor)->mItemGiveTag;
        if (g_heldActor != actor || g_heldTag != tag) {
            g_heldActor = actor;
            g_heldTag = tag;
            const std::string own = location_of_actor(actor);
            g_heldLocation = own.empty() ? g_lastResolvedApLocation : own;
        }
        location = g_heldLocation;
    } else if (const auto loc = g_actorLocation.find(actor); loc != g_actorLocation.end()) {
        location = loc->second;
    }
    const auto owner = g_placementOwner.find(location);
    if (owner == g_placementOwner.end()) {
        return {};
    }
    return g_client.gameOfSlotName(owner->second);
}

HookAction pre_item_draw_base(ModContext*, void* args, void* retval, void*) {
    auto* item = mods::arg<daItemBase_c*>(args, 0);
    if (item == nullptr || item->getDisplayItemNo() != kApItem || !cfg_on(g_cfgBoxes)) {
        return HOOK_CONTINUE;
    }
    const std::string game = game_for_actor(item);
    const covers::Cover* cover = game.empty() ? nullptr : covers::get(game);
    const bool held = fopAcM_GetName(item) == fpcNm_Demo_Item_e;
    if (cover == nullptr || !box::draw(item, *cover, held)) {
        return HOOK_CONTINUE;
    }
    *static_cast<int*>(retval) = 1;
    return HOOK_SKIP_ORIGINAL;
}

static void choose_boxes(ModContext*, UiDialogHandle, void*) {
    svc_mng.config->set_string(svc_mng.mod_ctx, g_cfgSgdbKey, trim_key(g_keyDraft).c_str());
    svc_mng.config->set_bool(svc_mng.mod_ctx, g_cfgBoxes, true);
    svc_mng.config->set_int(svc_mng.mod_ctx, g_cfgBoxChoice, 1);
}

static void choose_sols(ModContext*, UiDialogHandle, void*) {
    svc_mng.config->set_bool(svc_mng.mod_ctx, g_cfgBoxes, false);
    svc_mng.config->set_int(svc_mng.mod_ctx, g_cfgBoxChoice, 2);
}

static void prompt_boxes() {
    static const UiDialogAction actions[] = {
        {sizeof(UiDialogAction), "Use game boxes", choose_boxes, nullptr, false,
            [](ModContext*, void*) { return trim_key(g_keyDraft).empty(); }},
        {sizeof(UiDialogAction), "Keep Sols", choose_sols, nullptr, false, nullptr},
    };
    UiDialogDesc desc = UI_DIALOG_DESC_INIT;
    desc.title = "Other players' items";
    desc.icon = "question-mark";
    desc.body_rml =
        "Items that belong to other players can show as the box of the game they're for, with "
        "its cover art, instead of a Sol.<br/><br/>Covers come from SteamGridDB and need your "
        "own API key, which is free: sign in at steamgriddb.com, open Preferences, then API, "
        "and paste the key below.<br/><br/>You can change this any time on the Status page of "
        "the Archipelago tab (F1).";
    desc.actions = actions;
    desc.action_count = std::size(actions);
    desc.build = [](ModContext* ctx, UiElementHandle pane, void*, ModError*) -> ModResult {
        UiControlDesc input = UI_CONTROL_DESC_INIT;
        input.kind = UI_CONTROL_STRING;
        input.label = "SteamGridDB API key";
        input.string_set_mode = UI_STRING_SET_ON_CHANGE;
        input.get = get_str;
        input.set = set_str;
        input.user_data = &g_keyDraft;
        return svc_mng.ui->pane_add_control(ctx, pane, &input, nullptr);
    };
    svc_mng.ui->dialog_push(svc_mng.mod_ctx, &desc, nullptr);
}

void maybe_prompt_boxes() {
    if (g_boxPromptShown || g_cfgBoxChoice == 0 || cfg_int(g_cfgBoxChoice, 0) != 0) {
        return;
    }
    if (!trim_key(config_string(g_cfgSgdbKey)).empty()) {
        svc_mng.config->set_int(svc_mng.mod_ctx, g_cfgBoxChoice, 1);  // set up before this asked
        return;
    }
    if (g_phase == Phase::NewSave || (g_phase == Phase::Playing && in_gameplay())) {
        g_boxPromptShown = true;
        prompt_boxes();
    }
}


// The get-item text box: its icon (the Sol's rupee-shaped stand-in) becomes the cover. The
// box builds its pictures from the icon it loads, sized for that; swapping the picture's
// texture afterwards keeps the size and changes only the image.
void post_msg_item_exec(ModContext*, void* args, void*, void*) {
    auto* scrn = mods::arg<dMsgScrnItem_c*>(args, 0);
    if (scrn == nullptr || !cfg_on(g_cfgBoxes)) {
        return;
    }
    // The box works out its item from the message: ours is the Foolish Item's (0x13).
    constexpr int kDonorItem = kApItemDonorMessage - 0x65;
    if (scrn->mItemIndex == kDonorItem && g_apTextServed) {
        g_apTextServed = false;
        g_apTextBox = scrn;
    }
    // Ours only while it still shows our (borrowed) message: boxes are reallocated at the
    // same address, so a later box for anything else must not inherit the claim.
    const bool ours = scrn == g_apTextBox && scrn->mItemIndex == kDonorItem;
    if (scrn == g_apTextBox && !ours) {
        g_apTextBox = nullptr;
    }
    if (scrn->mItemIndex != kApItem && !ours) {
        return;
    }
    J2DPicture* pane = scrn->mpItemPane[0];
    const covers::Cover* cover = covers::get(game_for_location(g_heldLocation));
    if (pane == nullptr || cover == nullptr || cover->iconTimg.empty()) {
        return;
    }
    const auto* timg = reinterpret_cast<const ResTIMG*>(cover->iconTimg.data());
    if (const JUTTexture* tex = pane->getTexture(0); tex != nullptr && tex->getTexInfo() == timg) {
        return;  // already showing it
    }
    pane->changeTexture(timg, 0);
    // The box draws the icon at a size taken from the stand-in's shape (taller than wide); the
    // cover's icon is square, so draw it square, as big as the icon slot allows. It centres
    // itself in the slot.
    const float side = std::min(scrn->field_0x170, scrn->field_0x174);
    scrn->field_0x178 = side;
    scrn->field_0x17c = side;
    // The icon's colours were set for the stand-in (tinted); the cover shows as it is.
    pane->setBlackWhite(JUtility::TColor(0, 0, 0, 0), JUtility::TColor(255, 255, 255, 255));
    pane->setCornerColor(JUtility::TColor(255, 255, 255, 255));
}


// Covers for every game with an item in this world (theirs, or another Twilight Princess).
void want_covers() {
    std::vector<std::string> games;
    for (const auto& [loc, owner] : g_placementOwner) {
        const std::string game = g_client.gameOfSlotName(owner);
        if (!game.empty() && std::find(games.begin(), games.end(), game) == games.end()) {
            games.push_back(game);
        }
    }
    covers::want(games);
}

}  // namespace ap::internal
