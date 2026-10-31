// Slot data into lookup tables, and the seed rebuilt from it on a worker thread.

#include "internal.hpp"

namespace ap::internal {

// Used only in this file; defined below.
static int region_of(const YAML::Node& categories);
static void build_check_map();
static void build_tag_map();
static std::string sanitize(std::string s);
static bool generate_seed(const json& slotData, const std::string& slot, std::string& outHash, std::string& outError);


// The randomizer tracker's own groups (src/ui/rando_config.cpp), plus "Other".
const std::vector<TrackerRegion>& tracker_regions() {
    static const std::vector<TrackerRegion> regions = {
        {"Ordon", {"Ordona Province"}, false},
        {"Faron", {"Faron Province", "Faron Woods", "Hyrule Field - Faron Province", "Sacred Grove"},
            false},
        {"Eldin", {"Eldin Province", "Hyrule Field - Eldin", "Hyrule Field - Eldin Province",
                      "Eldin Lantern Cave", "Eldin Stockcave", "Death Mountain", "Kakariko Village",
                      "Kakariko Graveyard"},
            false},
        {"Lanayru", {"Lanayru Province", "Hyrule Field - Lanayru", "Hyrule Field - Lanayru Province",
                        "Castle Town", "Fishing Hole", "Lake Hylia", "Lake Lantern Cave",
                        "Upper Zoras River", "Zoras Domain"},
            false},
        {"Gerudo Desert", {"Gerudo Desert", "Bulblin Camp", "Mirror Chamber", "Cave Of Ordeals"}, false},
        {"Snowpeak", {"Snowpeak Province", "Snowpeak"}, false},
        {"Forest Temple", {"Forest Temple"}, true},
        {"Goron Mines", {"Goron Mines"}, true},
        {"Lakebed Temple", {"Lakebed Temple"}, true},
        {"Arbiter's Grounds", {"Arbiters Grounds"}, true},
        {"Snowpeak Ruins", {"Snowpeak Ruins"}, true},
        {"Temple of Time", {"Temple of Time"}, true},
        {"City in the Sky", {"City in the Sky"}, true},
        {"Palace of Twilight", {"Palace of Twilight"}, true},
        {"Hyrule Castle", {"Hyrule Castle"}, true},
        {"Other", {}, false},
    };
    return regions;
}


// Dungeons first, so a dungeon's checks never land in the province around it.
static int region_of(const YAML::Node& categories) {
    const auto& regions = tracker_regions();
    auto has = [&](const std::string& c) {
        for (const auto& n : categories) {
            if (n.as<std::string>() == c) {
                return true;
            }
        }
        return false;
    };
    for (const bool dungeons : {true, false}) {
        for (size_t i = 0; i < regions.size(); ++i) {
            if (regions[i].dungeon == dungeons &&
                std::ranges::any_of(regions[i].categories, has)) {
                return static_cast<int>(i);
            }
        }
    }
    return static_cast<int>(regions.size()) - 1;
}

static void build_check_map() {
    g_checkToLocations.clear();
    g_scan.clear();
    g_scanPos = 0;
    g_trackerChecks.clear();
    const auto locations = LOAD_EMBED_YAML(RANDO_DATA_PATH "locations.yaml");
    auto add = [](const std::string& check, const std::string& loc) {
        g_checkToLocations[check].push_back(loc);
    };
    for (const auto& node : locations) {
        const std::string name = node["Name"].as<std::string>();
        const auto idIt = g_locationIds.find(name);
        if (idIt == g_locationIds.end()) {
            continue;  // not an AP location in this slot (vanilla-locked or removed)
        }
        const YAML::Node meta = node["Metadata"];
        g_scan.push_back({name, idIt->second, meta});
        g_trackerChecks.push_back({name, idIt->second, region_of(node["Categories"])});
        if (!meta.IsMap()) {
            continue;
        }
        auto stage = [](const YAML::Node& n) -> std::string {
            const int s = n["Stage"].as<int>();
            return (s >= 0 && s < static_cast<int>(std::size(allStages))) ? allStages[s] : "";
        };
        for (const auto& c : meta["Chest"]) {
            add(fmt::format("chest:{}:{}", stage(c), c["Tbox Id"].as<int>()), name);
        }
        for (const auto& c : meta["Poe"]) {
            add(fmt::format("poe:{}:{}", stage(c), c["Flag"].as<int>()), name);
        }
        for (const auto& c : meta["Freestanding Item"]) {
            const int flag = c["Flag"].as<int>();
            add(fmt::format("freestanding:{}:{}", stage(c), flag), name);
            if (flag == 0x9F) {
                add(fmt::format("boss:{}", stage(c)), name);
            }
        }
        for (const auto& c : meta["Sky Character"]) {
            add(fmt::format("sky:{}:{}", stage(c), c["Room"].as<int>()), name);
        }
        for (const auto& c : meta["Golden Wolf"]) {
            add(fmt::format("golden_wolf:{}", c["Flag"].as<int>()), name);
        }
        for (const auto& c : meta["Bug Reward"]) {
            add(fmt::format("bug:{}", c["Item Id"].as<int>()), name);
        }
        for (const auto& c : meta["Shop"]) {
            add(fmt::format("shop:{}:{}:{}", stage(c), c["Room"].as<int>(), c["Item"].as<int>()), name);
        }
        for (const auto& c : meta["Name Lookup"]) {
            add(nameLookupOverride(c.as<std::string>()), name);
        }
    }
}

// Dusklight interns check names as give tags; asking for each of ours gives the tag the game
// will put on the actors for that check. Resolved by name: if a future Dusklight renames it,
// held items fall back to the last resolved location.
static void build_tag_map() {
    g_tagLocation.clear();
    using GiveTagFn = uint32_t (*)(const char*);
    static GiveTagFn giveTag = [] {
        void* address = nullptr;
        for (const char* name : {"?item_give_tag@mods@dusk@@YAIPEBD@Z",
                 "_ZN4dusk4mods13item_give_tagEPKc"}) {
            if (svc_mng.hook->resolve(svc_mng.mod_ctx, name, &address, nullptr) == MOD_OK &&
                address != nullptr) {
                return reinterpret_cast<GiveTagFn>(address);
            }
        }
        ap_log("dusk::mods::item_give_tag not found; held items use the last resolved location");
        return static_cast<GiveTagFn>(nullptr);
    }();
    if (giveTag == nullptr) {
        return;
    }
    for (const auto& [check, locs] : g_checkToLocations) {
        if (const uint32_t tag = giveTag(check.c_str()); tag != 0 && !locs.empty()) {
            g_tagLocation[tag] = locs.front();
        }
    }
}

std::string location_of_actor(const void* actor) {
    if (actor == nullptr) {
        return {};
    }
    const uint32_t tag = static_cast<const fopAc_ac_c*>(actor)->mItemGiveTag;
    const auto it = g_tagLocation.find(tag);
    return it != g_tagLocation.end() ? it->second : std::string{};
}

std::vector<std::string> locations_for_check(const char* check) {
    if (check == nullptr) {
        return {};
    }
    if (const auto it = g_checkToLocations.find(check); it != g_checkToLocations.end()) {
        return it->second;
    }
    // Ook drops the Gale Boomerang as a freestanding item in his own stage
    const std::string ook = fmt::format("freestanding:{}:", allStages[Ook]);
    if (std::strncmp(check, ook.c_str(), ook.size()) == 0) {
        return {"Forest Temple Gale Boomerang"};
    }
    return {};
}

bool load_slot_data(const json& slotData, std::string& err) {
    if (slotData.value("version", 0) != kSlotDataVersion) {
        err = "This slot was generated with an incompatible apworld version.";
        return false;
    }
    // The apworld fingerprints the logic data it generated from. A different fingerprint here
    // means the mod would rebuild this seed with different logic: items could end up behind
    // requirements Archipelago never thought they were behind. Refuse it loudly instead.
    const json theirs = slotData.value("data_version", json());
    if (theirs.is_number_integer() && theirs.get<uint32_t>() != data_version()) {
        err = "This multiworld was generated with a different version of the Twilight Princess "
              "(Dusklight) apworld than this mod. Update both to the same release.";
        ap_log(fmt::format("data version mismatch: seed {} vs mod {}", theirs.get<uint32_t>(),
            data_version()));
        return false;
    }
    g_locationIds.clear();
    g_apItemText.clear();
    // Each value() is named before iterating: items() of a temporary dangles before C++23's
    // range-for lifetime rules, which Linux builds read as nulls (issue #2).
    const json locationIds = slotData.value("location_ids", json::object());
    for (const auto& [name, id] : locationIds.items()) {
        // Strict: a location we can't report would silently never send its item.
        if (!id.is_number_integer()) {
            err = fmt::format("This room's data for your slot is broken (location '{}' has no id). "
                              "Regenerate the multiworld with the apworld from the same release "
                              "as the mod.", message_safe(name, 80));
            ap_log("slot data: location without an id: " + name);
            return false;
        }
        g_locationIds[name] = id.get<int64_t>();
    }
    g_expected.clear();
    g_placementOwner.clear();
    g_placementFlags.clear();
    const json placements = slotData.value("placements", json::object());
    for (const auto& [loc, v] : placements.items()) {
        if (v.is_object()) {
            g_placementOwner[loc] = v.value("player", "");
            g_placementFlags[loc] = json_num(v, "flags", 1);
        }
        g_expected[loc] = v.is_object() ? fmt::format("{} ({})", v.value("name", "?"),
                                              v.value("player", "?"))
                                        : v.get<std::string>();
        if (v.is_object()) {
            std::string who = message_safe(v.value("player", "someone"), 40);
            std::string what = message_safe(v.value("name", "item"), 60);
            if (who.empty()) {
                who = "someone";
            }
            if (what.empty()) {
                what = "an item";
            }
            g_apItemText[loc] = fmt::format("You found {}'s\n{}!", who, what);
        }
    }
    g_slotSeed = slotData.value("seed", "");
    build_check_map();
    build_tag_map();
    g_collectDungeons.clear();
    const json collect = slotData.value("collect_dungeons", json::array());
    for (const auto& d : collect) {
        if (!d.is_object()) {
            continue;
        }
        CollectDungeon cd;
        cd.name = d.value("dungeon", std::string{});
        const json triggers = d.value("triggers", json::array());
        for (const auto& t : triggers) {
            if (t.is_string()) {
                cd.triggers.push_back(t.get<std::string>());
            }
        }
        const json locs = d.value("locations", json::array());
        for (const auto& l : locs) {
            if (l.is_object() && l.contains("name") && l["name"].is_string()) {
                cd.locations.emplace_back(l["name"].get<std::string>(), json_num(l, "item", -1));
            }
        }
        if (!cd.name.empty() && !cd.triggers.empty()) {
            g_collectDungeons.push_back(std::move(cd));
        }
    }
    g_unshuffled.clear();
    g_unshuffledKnown = slotData.contains("unshuffled_dungeons");
    for (const auto& d : slotData.value("unshuffled_dungeons", json::array())) {
        if (d.is_string()) {
            g_unshuffled.push_back(d.get<std::string>());
        }
    }
    g_haveSlot = true;
    g_reachInputs = SIZE_MAX;
    ++g_trackerVersion;
    return true;
}


// ---------------------------------------------------------------------------------------
// Seed generation from slot data (runs on a worker thread)

static std::string sanitize(std::string s) {
    for (auto& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
            c = '_';
        }
    }
    return s;
}


// Where a seed's settings and plando live; the tracker rebuilds its logic from the same files.
std::filesystem::path seed_base(const std::string& seedName, const std::string& slot) {
    return randomizer::paths::GetRandomizerPath() / "archipelago" / sanitize(seedName + "-" + slot);
}

static bool generate_seed(const json& slotData, const std::string& slot, std::string& outHash,
    std::string& outError) {
    namespace fs = std::filesystem;
    std::lock_guard lock{tracker::g_generatorMutex};
    try {
        const std::string seedName = slotData.value("seed", "");
        const fs::path base = seed_base(seedName, slot);
        fs::create_directories(base);

        YAML::Node settings;
        settings["Seed"] = "AP-" + seedName + "-" + slot;
        settings["Plandomizer"] = true;
        settings["Generate Spoiler Log"] = false;
        settings["Starting Inventory"] = YAML::Node(YAML::NodeType::Map);
        settings["Excluded Locations"] = YAML::Node(YAML::NodeType::Sequence);
        settings["Mixed Entrance Pools"] = YAML::Node(YAML::NodeType::Sequence);
        const json slotSettings = slotData.value("settings", json::object());
        for (const auto& [k, v] : slotSettings.items()) {
            settings[k] = v.get<std::string>();
        }
        std::ofstream(base / "settings.yaml") << YAML::Dump(settings);

        YAML::Node plando;
        YAML::Node locs(YAML::NodeType::Map);
        const json placements = slotData.value("placements", json::object());
        for (const auto& [loc, v] : placements.items()) {
            locs[loc] = v.is_string() ? v.get<std::string>() : v.value("item", "Archipelago Item");
        }
        plando["World 1"]["Locations"] = locs;
        const fs::path plandoPath = base / "plando.yaml";
        std::ofstream(plandoPath) << YAML::Dump(plando);

        YAML::Node prefs;
        prefs["Plandomizer Path"] = plandoPath.generic_string();
        std::ofstream(base / "preferences.yaml") << YAML::Dump(prefs);

        randomizer::g_archipelagoMode = true;
        randomizer::Randomizer rando{base};
        auto err = rando.Generate();
        randomizer::g_archipelagoMode = false;
        if (err.has_value()) {
            outError = *err;
            return false;
        }
        RandomizerContext ctx = WriteSeedData(rando.GetWorld());
        ctx.mHash = rando.GetConfig().GetHash();
        fs::create_directories(ctx.GetSeedDataPath().parent_path());
        if (auto werr = ctx.WriteToFile(); werr.has_value()) {
            outError = *werr;
            return false;
        }
        outHash = ctx.mHash;
        return true;
    } catch (const std::exception& e) {
        randomizer::g_archipelagoMode = false;
        outError = e.what();
        return false;
    }
}

void start_generation(const json& slotData, const std::string& slot) {
    g_genStatus = 1;
    std::thread([slotData, slot] {
        std::string hash, error;
        const bool ok = generate_seed(slotData, slot, hash, error);
        {
            std::lock_guard lock{g_genMutex};
            g_genHash = hash;
            g_genError = error;
        }
        g_genStatus = ok ? 2 : 3;
    }).detach();
}

bool seed_files_exist(const std::string& hash) {
    std::error_code ec;
    return !hash.empty() &&
           std::filesystem::exists(randomizer::paths::GetRandomizerSeedsPath() / hash / "seed.dat", ec);
}

void tick_generation() {
    const int st = g_genStatus.load();
    if (st < 2) {
        return;
    }
    g_genStatus = 0;
    std::string hash, error;
    {
        std::lock_guard lock{g_genMutex};
        hash = g_genHash;
        error = g_genError;
    }
    if (st == 3) {
        mods::log::error("archipelago: seed generation failed: {}", error);
        g_status = "Seed generation failed: " + error;
        if (g_needsRegen) {
            toast("Archipelago", g_status, "warning", 10000);
        }
        g_phase = g_needsRegen ? Phase::Playing : Phase::NewSave;
        return;
    }
    if (g_needsRegen) {
        // Loaded save on a machine without its seed files: activate the rebuilt seed now.
        if (hash != g_loadedHash) {
            toast("Archipelago", "Rebuilt seed doesn't match this save.", "warning", 10000);
        } else {
            randomizer::session::deactivateSeed();
            randomizer::session::activateSeed(hash.c_str());
            loadAncientDocumentNum();
            g_needsRegen = false;
            after_seed_activated();
        }
        g_phase = Phase::Playing;
        return;
    }
    randomizer::session::g_pending_seed_hash = hash;
    g_status = "Seed ready!";
    g_phase = Phase::AwaitNewSave;
    randomizer::ui::g_file_select_window_ctx.is_proceed = true;  // continue to name entry
    mDoAud_seStartMenu(Z2SE_SY_NEW_FILE);
    if (g_window != 0) {
        const auto w = g_window;
        g_window = 0;
        svc_mng.ui->window_close(svc_mng.mod_ctx, w);
    }
}

void after_seed_activated() {
    // Our resolver must run after the randomizer's (registered on seed activation), so it sees
    // the resolved "Archipelago Item"; message overrides likewise stack on top of its text.
    if (g_resolver != 0) {
        svc_mng.item->clear_check_resolver(svc_mng.mod_ctx, g_resolver);
        g_resolver = 0;
    }
    svc_mng.item->set_check_resolver(svc_mng.mod_ctx, nullptr, resolve_check, nullptr, &g_resolver);
    g_textOverrides.clear();
    for (auto lang : {MESSAGE_LANGUAGE_ENGLISH, MESSAGE_LANGUAGE_GERMAN, MESSAGE_LANGUAGE_FRENCH,
             MESSAGE_LANGUAGE_SPANISH, MESSAGE_LANGUAGE_ITALIAN, MESSAGE_LANGUAGE_JAPANESE})
    {
        g_textOverrides.push_back(
            mods::flow::override_message_fn(0, kApItemDonorMessage, lang, ap_item_text));
    }
}

}  // namespace ap::internal
