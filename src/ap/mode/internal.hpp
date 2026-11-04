#pragma once

// Internal to the Archipelago game mode (src/ap/ap_mode.cpp and src/ap/mode/*.cpp): the
// mode's shared state, and the functions its files call across each other. Nothing outside
// the mode includes this; its public face is ap_mode.hpp.

#include "../ap_mode.hpp"

#include "../ap_box.hpp"
#include "../ap_client.hpp"
#include "../ap_covers.hpp"
#include "../ap_overlay.hpp"
#include "../ap_tracker.hpp"
#include "../emoji.hpp"
#include "../data_version.hpp"
#include "../text_safe.hpp"

#include "../../../generator/randomizer.hpp"
#include "../../../generator/utility/yaml.hpp"
#include "../../item_ids.h"
#include "../../paths.hpp"
#include "../../randomizer_context.hpp"
#include "../../session.hpp"
#include "../../stages.h"
#include "../../tools.h"
#include "../../ui/rando_config.hpp"
#include "../../verify_item_functions.h"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_b_gnd.h"
#include "d/actor/d_a_demo_item.h"
#include "d/actor/d_a_itembase.h"
#include "d/actor/d_a_obj_item.h"
#include "d/d_com_inf_game.h"
#include "d/d_file_select.h"
#include "d/d_kankyo.h"
#include "d/d_meter2_draw.h"
#include "d/d_msg_flow.h"
#include "d/d_msg_scrn_item.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "d/d_s_play.h"
#include "d/d_stage.h"
#include "m_Do/m_Do_audio.h"

#include <mods/items.h>
#include <mods/svc/flow.hpp>
#include <mods/svc/hook.hpp>
#include <mods/svc/log.hpp>

#include <yaml-cpp/yaml.h>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

RandomizerContext WriteSeedData(randomizer::logic::world::World* world);

namespace ap::internal {

using randomizer::session::svc_mng;
inline constexpr const char* kGameModeId = "archipelago";
inline constexpr const char* kConnBlob = "ap_conn";
inline constexpr const char* kStateBlob = "ap_state";
inline constexpr const char* kSeedHashBlob = "seed_hash";  // written by the randomizer session
inline constexpr int kSlotDataVersion = 1;
inline constexpr uint8_t kApItem = dItemNo_Randomizer_NOENTRY_220_e;  // "Archipelago Item" (0xDC)
inline constexpr uint16_t kApItemDonorMessage = 120;  // Foolish Item get text; overridden while armed
inline constexpr int64_t kItemIdBase = 0x54500000;

// ---------------------------------------------------------------------------------------
// State

inline Client g_client;
struct Conn {
    std::string server = "archipelago.gg:38281";
    std::string slot;
    std::string password;
};
struct SaveState {
    int received = 0;     // server item index delivered into this save
    bool goal = false;
    std::string seed;     // AP seed name this save belongs to
    std::string slot;
    int deathLink = -1;   // -1 follow the YAML, 0 off, 1 on (toggled from the Archipelago tab)
    bool transformAnywhere = false;  // the slot's Logic Transform Anywhere, kept for offline play
    // Collect Dungeon on Completion: dungeons already collected, and the locations that were
    // collected that way (their chests then hold a green rupee, even offline).
    std::vector<std::string> redeemedDungeons;
    std::vector<std::string> redeemedLocations;
};
enum class Phase {
    Idle,           // title / no AP save loaded
    NewSave,        // new-save window open, not yet generated
    Generating,     // seed generation thread running
    AwaitNewSave,   // generated; waiting for the host to create the save
    Playing,        // an AP save is loaded
};
inline Phase g_phase = Phase::Idle;
inline Conn g_conn;
inline SaveState g_state;
inline bool g_needsRegen = false;       // save loaded but its seed files are missing on this machine
inline std::string g_loadedHash;

// Slot data
inline bool g_haveSlot = false;
inline std::string g_slotSeed;
inline std::unordered_map<std::string, int64_t> g_locationIds;       // location name -> AP id
inline std::unordered_map<std::string, std::string> g_apItemText;    // location name -> get text
// Other worlds' items in this world: location -> the slot that owns it (for its game's box).
inline std::unordered_map<std::string, std::string> g_placementOwner;
// ... and its Archipelago classification (1 progression, 2 useful, 4 trap; 0 filler).
inline std::unordered_map<std::string, int> g_placementFlags;
// Item actors that resolved to an AP item -> their location (from the check resolver).
inline std::unordered_map<const void*, std::string> g_actorLocation;
// Give tag (fopAc_ac_c::mItemGiveTag) -> location. The item Link holds up carries its check's
// tag, so it's matched to its own location rather than to whichever AP item resolved last:
// another one spawning nearby mid-pickup used to swap the held box's cover (issue #4).
inline std::unordered_map<uint32_t, std::string> g_tagLocation;
// The item Link is holding up: its demo actor carries the pickup's committed result, which
// never passes the resolver, so it's matched to the pickup's location here instead.
inline const void* g_heldActor = nullptr;
inline uint32_t g_heldTag = 0;
inline std::string g_heldLocation;
inline std::unordered_map<std::string, std::string> g_expected;      // location name -> item AP expects
inline std::unordered_map<std::string, std::vector<std::string>> g_checkToLocations;
struct ScanLoc {
    std::string name;
    int64_t id;
    YAML::Node meta;
};
inline std::vector<ScanLoc> g_scan;
inline size_t g_scanPos = 0;
inline std::unordered_set<int64_t> g_checked;  // known to the server (sent or reported back)
inline std::vector<int64_t> g_toSend;

// Items
inline std::vector<NetworkItem> g_serverItems;
inline int g_outstanding = -1;
// Collect Dungeon on Completion (slot data "collect_dungeons"): per shuffled dungeon, the
// checks that mean its boss is beaten, and every check in it with our own item's number (or
// -1 when the item is another world's).
struct CollectDungeon {
    std::string name;
    std::vector<std::string> triggers;
    std::vector<std::pair<std::string, int>> locations;
};
inline std::vector<CollectDungeon> g_collectDungeons;
inline std::unordered_set<std::string> g_redeemedLocations;  // mirrors g_state.redeemedLocations
// Items handed over for a collected dungeon that the give queue hasn't confirmed yet. Server
// items wait meanwhile, so these confirmations are never counted as server items.
inline int g_redeemPending = 0;
inline std::string g_outstandingDesc;

// New-save UI
inline UiWindowHandle g_window = 0;
inline UiElementHandle g_statusText = 0;
inline std::string g_status = "Enter your Archipelago connection, then press Connect.";

// Generation thread
inline std::atomic<int> g_genStatus{0};  // 0 idle, 1 running, 2 ok, 3 failed
inline std::mutex g_genMutex;
inline std::string g_genHash;
inline std::string g_genError;

// AP item text
inline std::string g_armedText;
// The get-item text box showing our text: it borrows the Foolish Item's message, so the box
// thinks it's a Foolish Item (see post_msg_item_exec). Set when our text is served; the box
// that sees it first claims it.
inline bool g_apTextServed = false;
inline const void* g_apTextBox = nullptr;
inline int g_armedFrames = 0;
inline std::string g_lastResolvedApLocation;
inline std::vector<mods::flow::MessageOverride> g_textOverrides;
inline ItemCheckHandle g_resolver = 0;
inline ItemGiveHandle g_observer = 0;

// Menu tab
inline UiMenuTabHandle g_menuTab = 0;
inline UiWindowHandle g_statusWindow = 0;
inline UiElementHandle g_statusWindowText = 0;
inline UiElementHandle g_coverStatusText = 0;  // Overlay tab: how the covers are doing

// Death link (see "Death link" below)
inline bool g_deathLinkSlot = false;   // the YAML's choice, from slot_data
inline bool g_transformAnywhereSlot = false;  // the slot's Logic Transform Anywhere, from slot_data
inline bool g_deathSent = false;       // this death is handled; cleared once Link is alive again
inline double g_lastDeathTime = 0.0;   // time on the death we sent, to recognise its echo
inline int g_killFrames = 0;           // > 0: a death now is the one we were sent, not a new one
inline std::optional<std::string> g_pendingDeath;

// Tracker (see "Tracker" below; the logic itself is in ap_tracker.cpp)
struct TrackerCheck {
    std::string name;
    int64_t id = 0;
    int region = 0;  // index into tracker_regions()
};
inline std::vector<TrackerCheck> g_trackerChecks;  // this slot's checks, in locations.yaml order
inline std::vector<std::string> g_unshuffled;      // slot_data "unshuffled_dungeons"
inline bool g_unshuffledKnown = false;             // slot data from before 1.6.0 doesn't say
inline std::shared_ptr<tracker::Logic> g_logic;
inline std::string g_logicKey;                     // seed-slot the logic is for (or being built for)
inline bool g_logicBuilding = false;
inline std::string g_logicError;
inline std::unordered_set<std::string> g_inLogic;  // reachable checks, done or not
inline size_t g_reachInputs = SIZE_MAX;            // the inputs g_inLogic (or the search running) is for
inline bool g_reachBusy = false;
inline uint64_t g_trackerEpoch = 0;                // bumped on reset: late worker results are dropped
inline uint64_t g_trackerVersion = 1;              // bumped whenever what the tracker shows changes

// Message log (the Messages tab)
inline constexpr size_t kLogLines = 200;
inline std::deque<std::string> g_log;              // RML, oldest first
inline uint64_t g_logVersion = 1;
inline std::string g_chatDraft;

// Hints (the Hints tab): the server's list for our slot, kept current by SetNotify.
inline std::vector<Hint> g_hints;          // sorted: open first, by status; found last
inline uint64_t g_hintsVersion = 1;
inline uint64_t g_selectedHint = 0;        // hint_key() of the hint shown in the details pane
inline std::string g_hintItemDraft;
inline std::string g_hintLocationDraft;

// Emoji font (see load_emoji_font): when it's loaded, every UI string can show emoji as text
inline bool g_emojiFont = false;

// Overlay (see "Overlay" below): settings are config vars, so they persist and bind to the tab
struct Recent {
    std::string item;  // the item, or the whole chat line
    std::string from;  // who sent the item (empty for chat)
    std::chrono::steady_clock::time_point at;
};
inline std::deque<Recent> g_recentItems;
inline std::deque<Recent> g_recentChat;
inline ConfigVarHandle g_ovEnabled = 0, g_ovCorner = 0, g_ovX = 0, g_ovY = 0, g_ovScale = 0,
                g_ovOpacity = 0, g_ovStatus = 0, g_ovChecks = 0, g_ovItems = 0, g_ovHints = 0,
                g_ovChat = 0, g_ovDeathLink = 0, g_ovItemCount = 0, g_ovKeep = 0,
                g_ovHintCount = 0;

// Config
inline ConfigVarHandle g_cfgServer = 0;
inline ConfigVarHandle g_cfgSlot = 0;
inline ConfigVarHandle g_cfgModelScale = 0;
inline ConfigVarHandle g_cfgDebugLog = 0;
inline ConfigVarHandle g_cfgBoxes = 0;     // other worlds' items as game boxes
inline ConfigVarHandle g_cfgSgdbKey = 0;   // the player's SteamGridDB API key
inline ConfigVarHandle g_cfgBoxChoice = 0; // 0: not asked yet, 1: game boxes, 2: Sols
inline ConfigVarHandle g_cfgSkipFinished = 0; // strike out checks holding finished players' items
template <typename T>
bool read_blob(const char* name, T& out) {
    size_t size = 0;
    if (svc_mng.save->get_blob(svc_mng.mod_ctx, name, nullptr, &size) != MOD_OK || size == 0) {
        return false;
    }
    std::string buf(size, '\0');
    if (svc_mng.save->get_blob(svc_mng.mod_ctx, name, buf.data(), &size) != MOD_OK) {
        return false;
    }
    json j = json::parse(buf, nullptr, false);
    if (j.is_discarded()) {
        return false;
    }
    out = T{};
    if constexpr (std::is_same_v<T, Conn>) {
        out.server = j.value("server", out.server);
        out.slot = j.value("slot", "");
        out.password = j.value("password", "");
    } else {
        out.received = j.value("received", 0);
        out.goal = j.value("goal", false);
        out.seed = j.value("seed", "");
        out.slot = j.value("slot", "");
        out.deathLink = j.value("death_link", -1);
        out.transformAnywhere = j.value("transform_anywhere", false);
        for (const char* key : {"redeemed_dungeons", "redeemed_locations"}) {
            auto& list = std::string_view(key) == "redeemed_dungeons" ? out.redeemedDungeons
                                                                       : out.redeemedLocations;
            const json names = j.value(key, json::array());
            for (const auto& n : names) {
                if (n.is_string()) {
                    list.push_back(n.get<std::string>());
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Slot data -> lookup tables

struct TrackerRegion {
    const char* label;
    std::vector<std::string> categories;
    bool dungeon;
};

// ---------------------------------------------------------------------------------------
// Client callbacks

inline json g_lastSlotData;

// ---------------------------------------------------------------------------------------
// New-save window

inline std::string g_inServer, g_inSlot, g_inPassword;

// ---------------------------------------------------------------------------------------
// Tracker
//
// The seed's world is rebuilt once per save on a worker thread (ap_tracker.cpp), from the same
// files the seed was generated from, then asked "which checks can I reach?" on another worker
// whenever items or checks change. The game thread only hands inputs over and takes results.

struct WorkerResults {
    std::mutex mutex;
    uint64_t epoch = 0;  // results are kept only while this matches the worker's
    bool logicDone = false;
    std::shared_ptr<tracker::Logic> logic;
    std::string logicError;
    bool reachDone = false;
    std::unordered_set<std::string> reach;
};
inline WorkerResults g_workers;
// Skippable: not done, but its item belongs to a player who has already finished.
enum class CheckState { Done, InLogic, NotYet, Unknown, Skippable };
struct RegionCounts {
    size_t total = 0, done = 0, inLogic = 0, skippable = 0;
};

// The window's styles: tracker rows, and Archipelago's usual colors for the message log.
inline constexpr const char* kWindowRcss = R"(
.ap-row { display: block; padding: 3dp 0dp; }
.ap-dot { display: inline-block; width: 10dp; height: 10dp; border-radius: 5dp; margin-right: 10dp; }
.ap-in { color: #ffffff; }
.ap-in .ap-dot { background-color: #6fd08c; }
.ap-out { color: #a6a6a6; }
.ap-out .ap-dot { border: 2dp #a6a6a6; }
.ap-unknown { color: #d0d0d0; }
.ap-unknown .ap-dot { border: 2dp #d0d0d0; }
.ap-done { color: #6c6c6c; text-decoration: line-through; }
.ap-done .ap-dot { background-color: #6c6c6c; }
.ap-skip { color: #6c6c6c; text-decoration: line-through; }
.ap-skip .ap-dot { border: 2dp #6c6c6c; }
.ap-head { display: block; margin-top: 10dp; margin-bottom: 2dp; font-weight: bold; color: #e8c867; }
.ap-legend > span { margin-right: 18dp; }
.ap-quiet { color: #a6a6a6; }
.ap-msg { display: block; padding: 3dp 0dp; }
.ap-me { color: #ee00ee; }
.ap-player { color: #fafad2; }
.ap-prog { color: #af99ef; }
.ap-useful { color: #6d8be8; }
.ap-trap { color: #fa8072; }
.ap-item { color: #00eeee; }
.ap-loc { color: #00ff7f; }
.ap-ent { color: #6495ed; }
)";

// Tracker tab
inline UiElementHandle g_trackerSummary = 0;
inline UiElementHandle g_trackerProgress = 0;
inline UiElementHandle g_trackerList = 0;
inline std::vector<std::pair<UiElementHandle, int>> g_trackerGroups;  // group control, region

// Messages tab
inline UiElementHandle g_logElem = 0;
inline uint64_t g_logShown = 0;

// Hints tab
inline UiElementHandle g_hintsSummary = 0;
inline UiListHandle g_hintsList = 0;
inline UiElementHandle g_hintDetails = 0;
inline std::tuple<uint64_t, uint64_t, int> g_hintsShown{0, 0, -1};
inline std::vector<std::string> g_hintLabels;  // backing store for the list's labels
inline constexpr uint32_t kOvText = 0xF2EEE2FF, kOvDim = 0xA9A493FF, kOvGreen = 0x6FD08CFF,
                   kOvYellow = 0xE8C867FF, kOvRed = 0xE8837AFF, kOvBlue = 0x9DB8FFFF;


// ---------------------------------------------------------------------------------------
// Status window (menu bar tab)

inline std::string g_editServer;

// Auto-reconnect
inline int g_reconnectFrames = 0;

// ---------------------------------------------------------------------------------------
// Functions shared between the mode's files, by file

// mode/common.cpp
void ap_log(const std::string& line);
const std::string& item_name(int id);
void toast(const std::string& title, const std::string& body, const char* type = nullptr, uint32_t ms = 0);
void log_rml(std::string rml);
std::string config_string(ConfigVarHandle var);
void write_conn();
void write_state();
std::string status_line();
std::string trim_key(std::string key);
bool in_gameplay();
void get_str(ModContext*, void* ud, UiControlValue* out);
void set_str(ModContext*, void* ud, const UiControlValue* v);
bool cfg_on(ConfigVarHandle h);
int64_t cfg_int(ConfigVarHandle h, int64_t fallback);

// mode/slot.cpp
const std::vector<TrackerRegion>& tracker_regions();
std::vector<std::string> locations_for_check(const char* check);
// The location of the check an actor was created for (its give tag), or "" if unknown.
std::string location_of_actor(const void* actor);
bool load_slot_data(const json& slotData, std::string& err);
std::filesystem::path seed_base(const std::string& seedName, const std::string& slot);
void start_generation(const json& slotData, const std::string& slot);
bool seed_files_exist(const std::string& hash);
void tick_generation();
void after_seed_activated();

// mode/items.cpp
void on_connected(const json& p);
void on_items(int index, const std::vector<NetworkItem>& items);
void on_hints(const std::vector<Hint>& hints);
void on_print(const std::string& text, const json& msg);
void on_disconnected(const std::string& reason);
bool resolve_check(ModContext*, const ItemCheckInfo* info, ItemCheckResolution* out, void*);
void observe_give(ModContext*, const ItemGiveInfo* info, void*);
bool ap_item_text(ModContext*, const MessageOverrideContext*, MessageTextData* out, void*);
void complete_goal(const char* how);
void post_ganondorf_execute(ModContext*, void* args, void*, void*);
HookAction pre_change_scene(ModContext*, void*, void*, void*);
void log_stage_changes();
void scan_locations();
void flush_checks();
void tick_dungeon_collect();
void deliver_items();

// mode/hooks.cpp
void post_ditem_set_mtx(ModContext*, void* args, void*, void*);
void post_item_set_base_mtx(ModContext*, void* args, void*, void*);
void post_midna_search_npc(ModContext*, void*, void* retval, void*);
void post_msg_query042(ModContext*, void*, void* retval, void*);

// mode/death_link.cpp
bool death_link_on();
void apply_death_link_tags();
void post_link_dead_init(ModContext*, void* args, void*, void*);
void post_link_fog_dead_init(ModContext*, void* args, void*, void*);
void on_bounced(const json& p);
void tick_death_link();

// mode/boxes.cpp
HookAction pre_item_draw_base(ModContext*, void* args, void* retval, void*);
void maybe_prompt_boxes();
void post_msg_item_exec(ModContext*, void* args, void*, void*);
void want_covers();

// mode/saves.cpp
ModResult open_gate_window(void* fileSelect);
ModResult on_new_save(void* ud, ModError* err);
ModResult on_save_loaded(void* ud, ModError* err);
ModResult on_game_reset(void*, ModError*);

// mode/tracker.cpp
void reset_tracker();
void tick_tracker();
RegionCounts region_counts(int region);
ModResult build_tracker_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*);
ModResult update_tracker_tab(ModContext* ctx, void*, ModError*);

// mode/messages.cpp
bool load_emoji_font();
bool not_connected(ModContext*, void*);
ModResult build_messages_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*);
ModResult update_messages_tab(ModContext* ctx, void*, ModError*);

// mode/hints.cpp
std::string player_label(int slot);
ModResult build_hints_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*);
ModResult update_hints_tab(ModContext* ctx, void*, ModError*);

// mode/overlay.cpp
void post_meter2_draw(ModContext*, void*, void*, void*);
void register_overlay_vars();
void add_bound(ModContext* ctx, UiElementHandle pane, UiControlKind kind, const char* label, ConfigVarHandle var, const char* help = nullptr, int64_t min = 0, int64_t max = 0, int64_t step = 1, const char* suffix = nullptr);
ModResult build_overlay_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle, void*, ModError*);

// mode/status.cpp
void open_status_window(ModContext*, void*);

// mode/covers.cpp
ModResult build_covers_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*);
ModResult update_covers_tab(ModContext* ctx, void*, ModError*);
void forget_covers_tab();

}  // namespace ap::internal
