#pragma once

// Minimal Archipelago network protocol client over the mod's own WebSocket client
// (see ws_tcp.hpp). Runs entirely on the game thread: call poll() once per frame.

#include "ws_tcp.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace ap {

using json = nlohmann::json;

// A number field from a server packet, or `fallback` when it's missing, null or not a number.
// Only for fields where a default is harmless; anything identity-bearing is checked strictly.
template <typename T>
T json_num(const json& j, const char* key, T fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->template get<T>() : fallback;
}

struct NetworkItem {
    int64_t item = 0;
    int64_t location = 0;
    int player = 0;
    int flags = 0;
};

// One hint, as the server keeps it (NetUtils.Hint).
struct Hint {
    int receivingPlayer = 0;
    int findingPlayer = 0;
    int64_t location = 0;
    int64_t item = 0;
    bool found = false;
    std::string entrance;
    int itemFlags = 0;
    int status = 0;  // HintStatus: 0 unspecified, 10 no priority, 20 avoid, 30 priority, 40 found
};

struct ConnectInfo {
    std::string server;  // "host:port", optionally with ws:// or wss://
    std::string slot;
    std::string password;
};

enum class State {
    Disconnected,
    Connecting,    // socket opening
    Handshaking,   // waiting for RoomInfo / Connected
    Connected,
    Refused,
};

class Client {
public:
    // Callbacks (game thread)
    std::function<void(const json& connected)> onConnected;
    std::function<void(int index, const std::vector<NetworkItem>& items)> onItems;
    std::function<void(const std::string& text, const json& msg)> onPrint;
    std::function<void(const json& bounced)> onBounced;
    // Every hint that concerns our slot, whenever the server's list changes.
    std::function<void(const std::vector<Hint>& hints)> onHints;
    std::function<void(const std::string& reason)> onDisconnected;

    void connect(const ConnectInfo& info);
    void disconnect();
    void poll();

    void sendLocations(const std::vector<int64_t>& locations);
    void sendGoal();
    void sendSync();
    void sendBounce(const json& bounce);
    // Tags go out with Connect, so a reconnect keeps them; changing them while connected
    // sends ConnectUpdate. "DeathLink" here is what puts us on the death link channel.
    void setTags(const std::vector<std::string>& tags);
    void say(const std::string& text);
    // Priority / avoid / no priority on a hint for one of our items (UpdateHint).
    void updateHint(int findingPlayer, int64_t location, int status);

    State state() const { return mState; }
    const std::string& lastError() const { return mLastError; }
    const ConnectInfo& info() const { return mInfo; }
    int slot() const { return mSlot; }
    const std::string& playerName(int slot) const;
    const std::string& seedName() const { return mSeedName; }
    int team() const { return mTeam; }
    int hintPoints() const { return mHintPoints; }
    // Points one hint costs this slot (MultiServer.get_hint_cost), 0 if hints are free.
    int hintCost() const;
    // Names from the data package, by the game of the slot that owns the id.
    std::string itemName(int64_t id, int ownerSlot) const;
    std::string locationName(int64_t id, int ownerSlot) const;
    // The game a slot plays, by its slot name (as slot_info lists it), or "" if unknown.
    std::string gameOfSlotName(const std::string& slotName) const;
    // Every game in the room, one entry each.
    std::vector<std::string> games() const;
    // Whether the player with this slot name has reached their goal. Bumps statusVersion()
    // whenever any player's answer changes (the server notifies us live).
    bool slotFinished(const std::string& slotName) const;
    uint64_t statusVersion() const { return mStatusVersion; }

    static constexpr const char* kGame = "Twilight Princess (Dusklight)";

private:
    void open(const std::string& url);
    void on_open();
    void on_message(std::string_view text);
    void on_closed(std::string reason);
    void handle(const json& packet);
    void send(const json& packets);
    std::string nextUrl();
    static std::vector<Hint> parse_hints(const json& list);
    std::string statusKeyPrefix() const;
    void note_status(const std::string& key, const json& value);

    ConnectInfo mInfo{};
    State mState = State::Disconnected;
    std::string mLastError;
    std::vector<std::string> mUrls;
    size_t mUrlIndex = 0;
    TcpWebSocket mSocket;
    std::vector<std::string> mTags;
    int mSlot = -1;
    std::string mSeedName;
    int mTeam = 0;
    int mHintPoints = 0;
    int mHintCostPercent = 0;
    size_t mSlotLocations = 0;
    std::vector<std::string> mPlayerNames;
    std::unordered_set<int> mFinished;  // slots whose status is CLIENT_GOAL
    uint64_t mStatusVersion = 0;
};

// Parses AP's PrintJSON "data" parts into plain text, resolving player/item/location ids with
// the names the server gave us (item/location names come in the text for our game via slot data).
std::string flatten_print(const json& data, const Client& client);

// The same message as RML for the Archipelago window: every part escaped and bounded, players,
// items (by importance), locations and entrances wrapped in spans with ap-* classes.
std::string print_rml(const json& data, const Client& client);

}  // namespace ap
