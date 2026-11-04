#pragma once

// Box art for other worlds' items: matching an Archipelago game to SteamGridDB, and turning the
// downloaded image into texels the game can draw. No dusklight headers, so tools/tls_test.cpp
// can test it on the host.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ap::cover_art {

// False for games with no box to find (the ones that only exist inside Archipelago).
bool has_box(const std::string& apGame);

// What to search SteamGridDB for. Most Archipelago game names are the store name; a few aren't.
std::string search_term(const std::string& apGame);

// The name without brackets or trailing edition and platform words ("Remake", "HD", "Switch",
// ...), to search again with when the full name finds nothing; "" if that's the same search.
std::string fallback_term(const std::string& apGame);

// Scores of 500 and up are a confident match (the name, the title under a series name, or the
// name at the start of a longer one); below that it's a best guess the player should check.
constexpr int kConfidentScore = 500;

struct Match {
    int64_t id = 0;
    std::string name;  // SteamGridDB's name for it
    int score = 0;
};

// The best game in a /search/autocomplete response for this Archipelago game, or nullopt when
// nothing is close enough (a wrong box is worse than the Sol). Falls back to fallback_term's
// words, preferring the newest game of that name for a remake.
std::optional<Match> match_game(const std::string& apGame, const nlohmann::json& response);
std::optional<int64_t> pick_game(const std::string& apGame, const nlohmann::json& response);

// Every game in a /search/autocomplete response, for the player to choose from.
struct Candidate {
    int64_t id = 0;
    std::string name;
    int year = 0;  // 0 if SteamGridDB doesn't say
};
std::vector<Candidate> candidates(const nlohmann::json& response);

// The image to download from a /grids response (a thumbnail, portrait art first), or "".
std::string pick_grid(const nlohmann::json& response);

// Percent-encodes everything but unreserved characters, for one URL path segment.
std::string url_encode(const std::string& s);

// File name (without extension) for a game's cached cover: readable, and unique per game.
std::string file_stem(const std::string& game);

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 0;
    std::vector<uint8_t> texels;  // RGBA8, level 0 then each smaller mip in turn
    uint8_t spine[3] = {};        // colour for the box's edges, from the art
    float aspect = 1.0f;          // the art's own width / height
    // The cover centred on a transparent square, for the item icon in the get-item text box.
    uint32_t iconSize = 0;
    uint32_t iconMips = 0;
    std::vector<uint8_t> icon;    // RGBA8, iconSize square, then its mips
};

// Decodes a JPEG or PNG into a texture: 256x384 for portrait art (384x256 for landscape), with
// mips, alpha forced opaque. nullopt if it isn't an image stb_image can read.
std::optional<Image> process(const uint8_t* data, size_t size);

}  // namespace ap::cover_art
