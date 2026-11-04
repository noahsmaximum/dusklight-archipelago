#pragma once

// Covers for other worlds' items. Each game's box art comes from SteamGridDB with the player's
// own API key, is cached on disk once per game (randomizer/archipelago/covers, shared by every
// room and seed), and is decoded into a texture the item draw hook can use. Without a key, or
// for games SteamGridDB doesn't know, there's no cover and the item stays a Sol.
//
// Game thread only.

#include "cover_art.hpp"

#include <dolphin/gx/GXStruct.h>

#include <string>
#include <vector>

namespace ap::covers {

struct Cover {
    GXTexObj texture;          // RGBA8 with mips; its texels live as long as the Cover
    GXColor spine;             // the box's edges
    float aspect = 0.7f;       // the art's width / height
    std::vector<uint8_t> texels;
    // A ResTIMG (header, then RGBA8 texels with mips) of the cover on a transparent square, for
    // the get-item text box's icon picture.
    std::vector<uint8_t> iconTimg;
};

// Every frame: the current settings, then any finished downloads and decodes.
void tick(const std::string& apiKey, bool enabled);

// The games whose items are in this world. Cached covers load; missing ones queue a download.
void want(const std::vector<std::string>& games);

// The game's cover once it's ready, else nullptr (the item draws as a Sol).
const Cover* get(const std::string& game);

// One line for the settings page: what's ready, what's coming, what went wrong.
std::string status();

// Forget the downloaded covers (and what SteamGridDB didn't have) and fetch them again.
// Covers the player chose themselves are kept.
void refetch();

// The Covers tab: every game wanted this session, where its cover comes from, and the player's
// own picks, which are remembered for every room.
enum class Source {
    Pending,    // still downloading or decoding
    Automatic,  // matched by name
    Chosen,     // the player picked it
    Custom,     // the player's own image in covers/custom
    Sol,        // none: no art found, the player asked for the Sol, or it failed
};
struct GameCover {
    std::string game;
    Source source = Source::Pending;
    std::string matched;  // SteamGridDB's name for the game whose art it is ("" if none)
    bool unsure = false;  // matched automatically, but not confidently: worth checking
    bool solChosen = false;
};
std::vector<GameCover> games();
// Bumped whenever games(), the search results or status() change.
uint64_t version();

// A search the player runs for one game. Replaces the previous results.
void search(const std::string& term);
bool searching();
const std::vector<cover_art::Candidate>& results();
const std::string& search_message();  // why there are no results, or ""

// Use this SteamGridDB game's art for an Archipelago game, from now on.
void choose(const std::string& game, int64_t id, const std::string& name);
// No box for this game, from now on: its items are Sols.
void use_sol(const std::string& game);
// Forget the player's choice (or recheck covers/custom) and find the cover again.
void automatic(const std::string& game);
// Where the player's own image for this game goes.
std::string custom_path(const std::string& game);

}  // namespace ap::covers
