#include "ap_client.hpp"

#include "emoji.hpp"
#include "text_safe.hpp"

#include <mods/svc/log.hpp>

#include <algorithm>
#include <memory>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace ap {
namespace {

// Names resolved from GetDataPackage, per game.
std::unordered_map<std::string, std::unordered_map<int64_t, std::string>> s_itemNames;
std::unordered_map<std::string, std::unordered_map<int64_t, std::string>> s_locationNames;
std::unordered_map<int, std::string> s_slotGames;
std::unordered_map<std::string, std::string> s_slotGamesByName;  // slot name -> game
std::unordered_map<std::string, int> s_playerSlotsByName;       // slot name -> slot, players only

// ClientStatus.CLIENT_GOAL: the player has finished their game.
constexpr int kClientGoal = 30;

std::string make_uuid() {
    static std::string uuid;
    if (uuid.empty()) {
        std::random_device rd;
        std::mt19937_64 rng(rd());
        constexpr char hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) {
            uuid += hex[rng() & 0xF];
        }
    }
    return uuid;
}

bool is_local_host(const std::string& host) {
    return host.starts_with("localhost") || host.starts_with("127.") || host.starts_with("[::1]");
}

const std::string kEmpty;

}  // namespace

const std::string& Client::playerName(int slot) const {
    if (slot >= 0 && static_cast<size_t>(slot) < mPlayerNames.size() && !mPlayerNames[slot].empty()) {
        return mPlayerNames[slot];
    }
    return kEmpty;
}

std::string Client::gameOfSlotName(const std::string& slotName) const {
    const auto it = s_slotGamesByName.find(slotName);
    return it != s_slotGamesByName.end() ? it->second : std::string{};
}

bool Client::slotFinished(const std::string& slotName) const {
    const auto it = s_playerSlotsByName.find(slotName);
    return it != s_playerSlotsByName.end() && mFinished.contains(it->second);
}

std::vector<std::string> Client::games() const {
    std::vector<std::string> out;
    for (const auto& [slot, game] : s_slotGames) {
        if (!game.empty() && std::find(out.begin(), out.end(), game) == out.end()) {
            out.push_back(game);
        }
    }
    return out;
}

void Client::connect(const ConnectInfo& info) {
    disconnect();
    mInfo = info;
    mLastError.clear();
    mUrls.clear();
    mUrlIndex = 0;

    std::string server = info.server;
    while (!server.empty() && (server.back() == ' ' || server.back() == '/')) {
        server.pop_back();
    }
    while (!server.empty() && server.front() == ' ') {
        server.erase(server.begin());
    }
    if (server.starts_with("ws://") || server.starts_with("wss://")) {
        mUrls.push_back(server);
    } else {
        if (server.find(':') == std::string::npos) {
            server += ":38281";
        }
        // Rooms are served either plain or TLS on the same port, never both, so try the
        // likelier one first: hosted rooms are TLS, a server you run yourself usually is not.
        if (is_local_host(server)) {
            mUrls.push_back("ws://" + server);
            mUrls.push_back("wss://" + server);
        } else {
            mUrls.push_back("wss://" + server);
            mUrls.push_back("ws://" + server);
        }
    }
    open(mUrls[0]);
}

void Client::open(const std::string& url) {
    mods::log::info("archipelago: connecting to {}", url);
    const bool secure = url.starts_with("wss://");
    std::string rest = url.substr(secure ? 6 : 5);
    std::string path = "/";
    const auto slash = rest.find('/');
    if (slash != std::string::npos) {
        path = rest.substr(slash);
        rest = rest.substr(0, slash);
    }
    uint16_t port = 38281;
    const auto colon = rest.rfind(':');
    const auto bracket = rest.rfind(']');
    if (colon != std::string::npos && (bracket == std::string::npos || bracket < colon)) {
        port = static_cast<uint16_t>(std::strtoul(rest.c_str() + colon + 1, nullptr, 10));
        rest = rest.substr(0, colon);
    }
    mState = mSocket.connect(rest, port, path, secure) ? State::Connecting : State::Disconnected;
}

void Client::disconnect() {
    mSocket.close();
    if (mState != State::Refused) {
        mState = State::Disconnected;
    }
}

void Client::send(const json& packets) {
    const std::string text = packets.dump(-1, ' ', false, json::error_handler_t::replace);
    if (!mSocket.send_text(text)) {
        mods::log::error("archipelago: send failed for {} bytes", text.size());
    }
}

void Client::on_open() {
    mods::log::info("archipelago: socket open");
    mState = State::Handshaking;
}

void Client::on_message(std::string_view text) {
    json packets = json::parse(text, nullptr, false);
    if (packets.is_discarded() || !packets.is_array()) {
        mods::log::warn("archipelago: malformed packet");
        return;
    }
    for (const auto& p : packets) {
        const std::string cmd = p.is_object() ? p.value("cmd", std::string{"?"}) : std::string{"?"};
        try {
            handle(p);
        } catch (const std::exception& e) {
            mods::log::error("archipelago: error handling {}: {}", cmd, e.what());
            if (cmd == "Connected" && mState != State::Refused) {
                mLastError = std::string("couldn't read the server's reply to connecting (") +
                             e.what() + ")";
                mState = State::Refused;  // no automatic retry: it would fail the same way
                mSocket.close();
                if (onDisconnected) {
                    onDisconnected(mLastError);
                }
                return;
            }
        }
    }
}

void Client::on_closed(std::string reason) {
    if (mState == State::Connecting && mUrlIndex + 1 < mUrls.size()) {
        open(mUrls[++mUrlIndex]);
        return;
    }
    if (reason.empty()) {
        reason = "connection closed";
    }
    mods::log::info("archipelago: socket closed ({})", reason);
    if (mState != State::Refused) {
        mState = State::Disconnected;
        mLastError = std::move(reason);
    }
    if (onDisconnected) {
        onDisconnected(mLastError);
    }
}

void Client::poll() {
    TcpWebSocket::Event ev;
    while (mSocket.poll(ev)) {
        switch (ev.type) {
        case TcpWebSocket::EventType::Open:
            on_open();
            break;
        case TcpWebSocket::EventType::Message:
            on_message(ev.text);
            break;
        case TcpWebSocket::EventType::Closed:
            on_closed(ev.error);
            break;
        default:
            break;
        }
    }
}

void Client::handle(const json& p) {
    const std::string cmd = p.value("cmd", "");
    mods::log::debug("archipelago: <- {}", cmd);
    if (cmd == "RoomInfo") {
        mSeedName = p.value("seed_name", "");
        mHintCostPercent = json_num(p, "hint_cost", 0);
        json games = p.value("games", json::array());
        // One frame for both: Connect first, then the names for PrintJSON.
        send(json::array({{
            {"cmd", "Connect"},
            {"password", mInfo.password},
            {"game", kGame},
            {"name", mInfo.slot},
            {"uuid", make_uuid()},
            {"version", {{"major", 0}, {"minor", 6}, {"build", 2}, {"class", "Version"}}},
            // Other worlds' items + starting inventory. Items at our own locations are given
            // in-game by the rebuilt seed itself.
            {"items_handling", 0b101},
            {"tags", mTags},
            {"slot_data", true},
        }, {{"cmd", "GetDataPackage"}, {"games", games}}}));
    } else if (cmd == "Connected") {
        // Who we are: without these nothing else works, so a bad value ends the connection.
        if (!p.contains("slot") || !p["slot"].is_number_integer() || !p.contains("team") ||
            !p["team"].is_number_integer()) {
            throw std::runtime_error("it has no slot or team number");
        }
        mSlot = p["slot"].get<int>();
        mTeam = p["team"].get<int>();
        mHintPoints = json_num(p, "hint_points", 0);
        mSlotLocations = p.value("missing_locations", json::array()).size() +
                         p.value("checked_locations", json::array()).size();
        mPlayerNames.clear();
        for (const auto& pl : p.value("players", json::array())) {
            const int s = json_num(pl, "slot", -1);
            if (s < 0 || s > 100000) {
                continue;
            }
            if (s >= static_cast<int>(mPlayerNames.size())) {
                mPlayerNames.resize(s + 1);
            }
            mPlayerNames[s] = pl.value("alias", pl.value("name", ""));
        }
        s_slotGames.clear();
        s_slotGamesByName.clear();
        s_playerSlotsByName.clear();
        mFinished.clear();
        ++mStatusVersion;
        if (p.contains("slot_info")) {
            for (const auto& [k, v] : p["slot_info"].items()) {
                const int s = std::stoi(k);
                s_slotGames[s] = v.value("game", "");
                s_slotGamesByName[v.value("name", "")] = v.value("game", "");
                // Item link groups (type 2) never report a status of their own.
                if ((json_num(v, "type", 1) & 2) == 0 && s != mSlot) {
                    s_playerSlotsByName[v.value("name", "")] = s;
                }
            }
        }
        mState = State::Connected;
        if (onConnected) {
            onConnected(p);
        }
        // Hints live in the server's data storage; read them now and hear about every change.
        // So are everyone else's statuses, which say when a player has finished (the tracker
        // strikes out checks holding their items).
        const std::string hintsKey = "_read_hints_" + std::to_string(mTeam) + "_" + std::to_string(mSlot);
        json keys = json::array({hintsKey});
        for (const auto& [name, s] : s_playerSlotsByName) {
            keys.push_back(statusKeyPrefix() + std::to_string(s));
        }
        send(json::array({{{"cmd", "Get"}, {"keys", keys}}, {{"cmd", "SetNotify"}, {"keys", keys}}}));
    } else if (cmd == "ConnectionRefused") {
        std::string errs;
        for (const auto& e : p.value("errors", json::array())) {
            errs += (errs.empty() ? "" : ", ") + e.get<std::string>();
        }
        mLastError = "refused: " + (errs.empty() ? std::string("unknown") : errs);
        mState = State::Refused;
        mSocket.close();
        if (onDisconnected) {
            onDisconnected(mLastError);
        }
    } else if (cmd == "ReceivedItems") {
        std::vector<NetworkItem> items;
        for (const auto& it : p.value("items", json::array())) {
            // Kept even if malformed, so the item index stays in step with the server's; an
            // item id of 0 is simply not something we can give.
            items.push_back({json_num(it, "item", int64_t{0}), json_num(it, "location", int64_t{0}),
                json_num(it, "player", 0), json_num(it, "flags", 0)});
        }
        if (onItems) {
            onItems(json_num(p, "index", 0), items);
        }
    } else if (cmd == "DataPackage") {
        // Names for display only. An entry without a number (some apworlds list events this
        // way) can never be sent or checked, so it's skipped rather than failing the table.
        const json games = p.contains("data") && p["data"].is_object()
                               ? p["data"].value("games", json::object())
                               : json::object();
        for (const auto& [game, gd] : games.items()) {
            if (!gd.is_object()) {
                continue;
            }
            int skipped = 0;
            auto& in = s_itemNames[game];
            // Named, not iterated in place: items() of a temporary dangles before C++23's
            // range-for lifetime rules (it read freed memory as null on Linux builds).
            const json itemIds = gd.value("item_name_to_id", json::object());
            for (const auto& [name, id] : itemIds.items()) {
                if (id.is_number_integer()) {
                    in[id.get<int64_t>()] = name;
                } else {
                    ++skipped;
                }
            }
            auto& ln = s_locationNames[game];
            const json locationIds = gd.value("location_name_to_id", json::object());
            for (const auto& [name, id] : locationIds.items()) {
                if (id.is_number_integer()) {
                    ln[id.get<int64_t>()] = name;
                } else {
                    ++skipped;
                }
            }
            if (skipped > 0) {
                mods::log::warn("archipelago: data package for '{}' has {} names without an id; "
                                "skipped", game, skipped);
            }
        }
    } else if (cmd == "PrintJSON") {
        if (onPrint) {
            onPrint(flatten_print(p.value("data", json::array()), *this), p);
        }
    } else if (cmd == "Bounced") {
        if (onBounced) {
            onBounced(p);
        }
    } else if (cmd == "RoomUpdate") {
        if (p.contains("hint_points") && p["hint_points"].is_number_integer()) {
            mHintPoints = p["hint_points"].get<int>();
        }
        if (p.contains("hint_cost") && p["hint_cost"].is_number_integer()) {
            mHintCostPercent = p["hint_cost"].get<int>();
        }
    } else if (cmd == "Retrieved" || cmd == "SetReply") {
        const std::string hintsKey = "_read_hints_" + std::to_string(mTeam) + "_" + std::to_string(mSlot);
        const json* value = nullptr;
        if (cmd == "SetReply" && p.value("key", "") == hintsKey && p.contains("value")) {
            value = &p["value"];
        } else if (cmd == "Retrieved" && p.contains("keys") && p["keys"].contains(hintsKey)) {
            value = &p["keys"][hintsKey];
        }
        if (value != nullptr && onHints) {
            onHints(parse_hints(*value));
        }
        if (cmd == "SetReply" && p.contains("value")) {
            note_status(p.value("key", ""), p["value"]);
        } else if (cmd == "Retrieved" && p.contains("keys") && p["keys"].is_object()) {
            for (const auto& [key, v] : p["keys"].items()) {
                note_status(key, v);
            }
        }
    }
}

std::string Client::statusKeyPrefix() const {
    return "_read_client_status_" + std::to_string(mTeam) + "_";
}

void Client::note_status(const std::string& key, const json& value) {
    const std::string prefix = statusKeyPrefix();
    if (!key.starts_with(prefix)) {
        return;
    }
    const std::string rest = key.substr(prefix.size());
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (rest.empty() || rest.size() > 6 || !std::all_of(rest.begin(), rest.end(), digit)) {
        return;
    }
    const int slot = std::stoi(rest);
    // Statuses only move forward, but a slot can be reported more than once.
    const bool finished = value.is_number_integer() && value.get<int64_t>() == kClientGoal;
    if (finished && mFinished.insert(slot).second) {
        ++mStatusVersion;
    } else if (!finished && mFinished.erase(slot) > 0) {
        ++mStatusVersion;
    }
}

std::vector<Hint> Client::parse_hints(const json& list) {
    std::vector<Hint> hints;
    if (!list.is_array()) {
        return hints;
    }
    for (const auto& h : list) {
        if (!h.is_object()) {
            continue;
        }
        Hint hint;
        hint.receivingPlayer = json_num(h, "receiving_player", 0);
        hint.findingPlayer = json_num(h, "finding_player", 0);
        hint.location = json_num(h, "location", int64_t{0});
        hint.item = json_num(h, "item", int64_t{0});
        hint.found = h.value("found", false);
        hint.entrance = h.value("entrance", "");
        hint.itemFlags = json_num(h, "item_flags", 0);
        hint.status = json_num(h, "status", hint.found ? 40 : 0);
        hints.push_back(std::move(hint));
    }
    return hints;
}

void Client::updateHint(int findingPlayer, int64_t location, int status) {
    if (mState == State::Connected) {
        send(json::array({{{"cmd", "UpdateHint"}, {"player", findingPlayer},
            {"location", location}, {"status", status}}}));
    }
}

int Client::hintCost() const {
    if (mHintCostPercent <= 0) {
        return 0;
    }
    return std::max(1, static_cast<int>(mHintCostPercent * 0.01 * static_cast<double>(mSlotLocations)));
}

void Client::sendLocations(const std::vector<int64_t>& locations) {
    if (mState != State::Connected || locations.empty()) {
        return;
    }
    send(json::array({{{"cmd", "LocationChecks"}, {"locations", locations}}}));
}

void Client::sendGoal() {
    if (mState == State::Connected) {
        send(json::array({{{"cmd", "StatusUpdate"}, {"status", 30}}}));
    }
}

void Client::sendSync() {
    if (mState == State::Connected) {
        send(json::array({{{"cmd", "Sync"}}}));
    }
}

void Client::setTags(const std::vector<std::string>& tags) {
    if (tags == mTags) {
        return;
    }
    mTags = tags;
    if (mState == State::Connected) {
        send(json::array({{{"cmd", "ConnectUpdate"}, {"tags", mTags}}}));
    }
}

void Client::sendBounce(const json& bounce) {
    if (mState == State::Connected) {
        json b = bounce;
        b["cmd"] = "Bounce";
        send(json::array({b}));
    }
}

void Client::say(const std::string& text) {
    if (mState == State::Connected) {
        send(json::array({{{"cmd", "Say"}, {"text", text}}}));
    }
}

namespace {

// The text a PrintJSON part stands for: ids resolved to names where we know them.
std::string part_text(const json& part, const Client& client) {
    const std::string type = part.value("type", "text");
    const std::string text = part.value("text", "");
    if (type == "player_id") {
        const auto& name = client.playerName(std::atoi(text.c_str()));
        return name.empty() ? text : name;
    }
    if (type == "item_id" || type == "location_id") {
        const int64_t id = std::strtoll(text.c_str(), nullptr, 10);
        const int owner = json_num(part, "player", 0);
        return type == "item_id" ? client.itemName(id, owner) : client.locationName(id, owner);
    }
    return text;
}

std::string lookup_name(const std::unordered_map<std::string,
                            std::unordered_map<int64_t, std::string>>& table,
    int64_t id, int ownerSlot) {
    if (const auto gameIt = s_slotGames.find(ownerSlot); gameIt != s_slotGames.end()) {
        if (const auto g = table.find(gameIt->second); g != table.end()) {
            if (const auto n = g->second.find(id); n != g->second.end()) {
                return n->second;
            }
        }
    }
    return std::to_string(id);
}

}  // namespace

std::string Client::itemName(int64_t id, int ownerSlot) const {
    return lookup_name(s_itemNames, id, ownerSlot);
}

std::string Client::locationName(int64_t id, int ownerSlot) const {
    return lookup_name(s_locationNames, id, ownerSlot);
}

std::string flatten_print(const json& data, const Client& client) {
    std::string out;
    for (const auto& part : data) {
        out += part_text(part, client);
    }
    return out;
}

std::string print_rml(const json& data, const Client& client) {
    std::string out;
    for (const auto& part : data) {
        const std::string type = part.value("type", "text");
        const std::string text =
            emoji::emojify(rml_escape(message_safe(part_text(part, client), 300)));
        const char* cls = nullptr;
        if (type == "player_id") {
            cls = std::atoi(part.value("text", "").c_str()) == client.slot() ? "ap-me" : "ap-player";
        } else if (type == "player_name") {
            cls = "ap-player";
        } else if (type == "item_id" || type == "item_name") {
            const int flags = json_num(part, "flags", 0);
            cls = (flags & 1) ? "ap-prog" : (flags & 2) ? "ap-useful" : (flags & 4) ? "ap-trap" : "ap-item";
        } else if (type == "location_id" || type == "location_name") {
            cls = "ap-loc";
        } else if (type == "entrance_name") {
            cls = "ap-ent";
        }
        if (cls != nullptr) {
            out += std::string{"<span class=\""} + cls + "\">" + text + "</span>";
        } else {
            out += text;
        }
    }
    return out;
}

}  // namespace ap
