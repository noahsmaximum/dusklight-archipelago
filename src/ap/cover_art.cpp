#include "cover_art.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../third_party/stb_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace ap::cover_art {
namespace {

using json = nlohmann::json;

// Archipelago names that don't find the right game on their own, and the year to prefer
// when SteamGridDB has several games of that name (0: any).
struct Alias {
    const char* term;
    int year = 0;
};

const std::unordered_map<std::string, Alias>& aliases() {
    static const std::unordered_map<std::string, Alias> kAliases = {
        {"A Link to the Past", {"The Legend of Zelda: A Link to the Past"}},
        {"Ocarina of Time", {"The Legend of Zelda: Ocarina of Time"}},
        {"Links Awakening DX", {"The Legend of Zelda: Link's Awakening DX"}},
        {"The Minish Cap", {"The Legend of Zelda: The Minish Cap"}},
        {"The Wind Waker", {"The Legend of Zelda: The Wind Waker"}},
        {"Twilight Princess", {"The Legend of Zelda: Twilight Princess"}},
        {"Twilight Princess (Dusklight)", {"The Legend of Zelda: Twilight Princess"}},
        {"Majora's Mask Recompiled", {"The Legend of Zelda: Majora's Mask"}},
        {"The Legend of Zelda - Oracle of Seasons", {"The Legend of Zelda: Oracle of Seasons"}},
        {"The Legend of Zelda - Oracle of Ages", {"The Legend of Zelda: Oracle of Ages"}},
        {"Pokemon Red and Blue", {"Pokemon Red Version"}},
        {"Pokemon FireRed and LeafGreen", {"Pokemon FireRed"}},
        {"SMZ3", {"Super Metroid"}},
        {"Starcraft 2", {"StarCraft II: Wings of Liberty"}},
        {"Civilization VI", {"Sid Meier's Civilization VI"}},
        {"DOOM 1993", {"Doom"}},
        {"Castlevania 64", {"Castlevania", 1999}},
        {"Celeste (Open World)", {"Celeste"}},
        {"DLCQuest", {"DLC Quest"}},
        {"Lufia II Ancient Cave", {"Lufia II: Rise of the Sinistrals"}},
        {"Kingdom Hearts 2", {"Kingdom Hearts II"}},
        {"Sonic Adventure 2 Battle", {"Sonic Adventure 2"}},
        {"Donkey Kong Country 3", {"Donkey Kong Country 3: Dixie Kong's Double Trouble!"}},
        {"Yoshi's Island", {"Super Mario World 2: Yoshi's Island"}},
        {"Yu-Gi-Oh! 2006", {"Yu-Gi-Oh! Ultimate Masters: World Championship Tournament 2006"}},
        {"MegaMan Battle Network 3", {"Mega Man Battle Network 3"}},
        {"Mario & Luigi Superstar Saga", {"Mario & Luigi: Superstar Saga"}},
    };
    return kAliases;
}

int alias_year(const std::string& apGame) {
    const auto it = aliases().find(apGame);
    return it != aliases().end() ? it->second.year : 0;
}

// Lowercase words, for comparing names: accents on e dropped (Pokemon/Pokémon), "&" as "and",
// punctuation as spaces, and II/III/IV as digits.
std::vector<std::string> words(const std::string& s) {
    std::string flat;
    for (size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == 0xC3 && i + 1 < s.size()) {
            const auto next = static_cast<unsigned char>(s[i + 1]);
            if (next == 0xA9 || next == 0x89 || next == 0xA8 || next == 0x88) {  // é É è È
                flat += 'e';
                ++i;
                continue;
            }
        }
        if (c == '&') {
            flat += " and ";
        } else if (c == '\'') {
            // "Link's" and "Links" compare equal
        } else if (c < 0x80 && std::isalnum(c)) {
            flat += static_cast<char>(std::tolower(c));
        } else {
            flat += ' ';
        }
    }
    std::vector<std::string> out;
    std::istringstream in(flat);
    for (std::string w; in >> w;) {
        if (w == "ii") {
            w = "2";
        } else if (w == "iii") {
            w = "3";
        } else if (w == "iv") {
            w = "4";
        }
        out.push_back(std::move(w));
    }
    return out;
}

// Where `needle` starts inside `hay` as whole words, or npos.
size_t find_words(const std::vector<std::string>& hay, const std::vector<std::string>& needle) {
    if (needle.empty() || needle.size() > hay.size()) {
        return std::string::npos;
    }
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (std::equal(needle.begin(), needle.end(), hay.begin() + static_cast<std::ptrdiff_t>(i))) {
            return i;
        }
    }
    return std::string::npos;
}

// The part of a name after its last ':' or " - ", the title under a series name.
std::string subtitle(const std::string& name) {
    const size_t colon = name.rfind(':');
    const size_t dash = name.rfind(" - ");
    size_t cut = std::string::npos;
    if (colon != std::string::npos) {
        cut = colon + 1;
    }
    if (dash != std::string::npos && (cut == std::string::npos || dash + 3 > cut)) {
        cut = dash + 3;
    }
    return cut == std::string::npos ? std::string{} : name.substr(cut);
}

// How well a SteamGridDB name matches what we're after. <= 0 is no match.
int score(const std::string& original, const std::vector<std::string>& candidate,
    const std::vector<std::string>& query) {
    if (candidate.empty() || query.empty()) {
        return 0;
    }
    int s = 0;
    const size_t at = find_words(candidate, query);
    if (candidate == query) {
        s = 1000;
    } else if (at == std::string::npos) {
        return 0;
    } else if (words(subtitle(original)) == query) {
        s = 800;  // "The Legend of Zelda: Ocarina of Time" for "Ocarina of Time"
    } else if (at == 0) {
        s = 600;  // "Hollow Knight: Voidheart Edition" for "Hollow Knight"
    } else {
        s = 400;
    }
    // Fan projects and alternate releases share names with the game itself.
    static const char* const kOffBrand[] = {"randomizer", "beta", "demo", "online", "mod", "hack",
        "remake", "remaster", "remastered", "romhack", "multiplayer", "coop", "archipelago",
        "rando", "experience", "3d", "hd", "reforged", "fan"};
    for (const auto& w : candidate) {
        if (std::find(query.begin(), query.end(), w) != query.end()) {
            continue;
        }
        for (const char* bad : kOffBrand) {
            if (w == bad) {
                s -= 300;
            }
        }
    }
    // Among the rest, the fewest extra words.
    s -= 15 * static_cast<int>(candidate.size() - std::min(candidate.size(), query.size()));
    return s;
}

// Box filter halving, for mips and for shrinking big art before the final resample.
struct Pixels {
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> px;  // RGBA8
};

Pixels halve(const Pixels& in) {
    Pixels out;
    out.w = std::max<uint32_t>(1, in.w / 2);
    out.h = std::max<uint32_t>(1, in.h / 2);
    out.px.resize(static_cast<size_t>(out.w) * out.h * 4);
    for (uint32_t y = 0; y < out.h; ++y) {
        const uint32_t y0 = std::min(in.h - 1, y * 2);
        const uint32_t y1 = std::min(in.h - 1, y * 2 + 1);
        for (uint32_t x = 0; x < out.w; ++x) {
            const uint32_t x0 = std::min(in.w - 1, x * 2);
            const uint32_t x1 = std::min(in.w - 1, x * 2 + 1);
            for (int c = 0; c < 4; ++c) {
                const uint32_t sum = in.px[(static_cast<size_t>(y0) * in.w + x0) * 4 + c] +
                                     in.px[(static_cast<size_t>(y0) * in.w + x1) * 4 + c] +
                                     in.px[(static_cast<size_t>(y1) * in.w + x0) * 4 + c] +
                                     in.px[(static_cast<size_t>(y1) * in.w + x1) * 4 + c];
                out.px[(static_cast<size_t>(y) * out.w + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
            }
        }
    }
    return out;
}

Pixels bilinear(const Pixels& in, uint32_t w, uint32_t h) {
    Pixels out;
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * h * 4);
    const float sx = static_cast<float>(in.w) / static_cast<float>(w);
    const float sy = static_cast<float>(in.h) / static_cast<float>(h);
    for (uint32_t y = 0; y < h; ++y) {
        const float fy = std::clamp((static_cast<float>(y) + 0.5f) * sy - 0.5f, 0.0f,
            static_cast<float>(in.h - 1));
        const auto y0 = static_cast<uint32_t>(fy);
        const uint32_t y1 = std::min(in.h - 1, y0 + 1);
        const float ty = fy - static_cast<float>(y0);
        for (uint32_t x = 0; x < w; ++x) {
            const float fx = std::clamp((static_cast<float>(x) + 0.5f) * sx - 0.5f, 0.0f,
                static_cast<float>(in.w - 1));
            const auto x0 = static_cast<uint32_t>(fx);
            const uint32_t x1 = std::min(in.w - 1, x0 + 1);
            const float tx = fx - static_cast<float>(x0);
            for (int c = 0; c < 4; ++c) {
                auto at = [&](uint32_t px, uint32_t py) {
                    return static_cast<float>(in.px[(static_cast<size_t>(py) * in.w + px) * 4 + c]);
                };
                const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                out.px[(static_cast<size_t>(y) * w + x) * 4 + c] =
                    static_cast<uint8_t>(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    return out;
}

}  // namespace

bool has_box(const std::string& apGame) {
    // Games that only exist inside Archipelago: whatever SteamGridDB matches would be wrong.
    static const char* const kNoBox[] = {"Archipelago", "ArchipIDLE", "ChecksFinder", "Clique",
        "Bumper Stickers", "Yacht Dice", "Sudoku", "Autopelago", "APQuest"};
    for (const char* name : kNoBox) {
        if (apGame == name) {
            return false;
        }
    }
    return !apGame.empty();
}

std::string search_term(const std::string& apGame) {
    const auto it = aliases().find(apGame);
    return it != aliases().end() ? std::string{it->second.term} : apGame;
}

std::string fallback_term(const std::string& apGame) {
    // Brackets go ("Celeste (Open World)"), then edition words at the end ("Resident Evil 2
    // Remake" is plain "Resident Evil 2" on SteamGridDB, alongside the 1998 one).
    std::string term = search_term(apGame);
    for (size_t open = term.find('('); open != std::string::npos; open = term.find('(')) {
        const size_t close = term.find(')', open);
        term.erase(open, close == std::string::npos ? std::string::npos : close - open + 1);
    }
    // Platforms too: "Link's Awakening Switch" is listed as "The Legend of Zelda: Link's
    // Awakening".
    static const char* const kEditions[] = {"remake", "remastered", "remaster", "hd",
        "definitive", "enhanced", "edition", "deluxe", "classic", "redux", "reloaded", "switch",
        "nintendo", "ns", "nsw", "n64", "gba", "gbc", "gb", "snes", "sfc", "nes", "famicom",
        "genesis", "ps1", "ps2", "ps3", "ps4", "psx", "psp", "vita", "ds", "3ds", "wii", "wiiu",
        "gc", "gcn", "gamecube", "pc", "dreamcast", "saturn", "xbox"};
    for (bool trimmed = true; trimmed;) {
        trimmed = false;
        while (!term.empty() && (term.back() == ' ' || term.back() == '-' || term.back() == ':')) {
            term.pop_back();
        }
        const size_t space = term.rfind(' ');
        if (space == std::string::npos) {
            break;
        }
        std::string last = term.substr(space + 1);
        std::transform(last.begin(), last.end(), last.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* edition : kEditions) {
            if (last == edition) {
                term.erase(space);
                trimmed = true;
                break;
            }
        }
    }
    return term == search_term(apGame) ? std::string{} : term;
}

namespace {

int year_of(const json& g) {
    const bool dated = g.contains("release_date") && g["release_date"].is_number_integer();
    return dated ? static_cast<int>(1970 + g["release_date"].get<int64_t>() / 31556952) : 0;
}

std::optional<Match> best_match(const json& response,
    const std::vector<std::vector<std::string>>& queries, int year, bool preferNewest) {
    std::optional<Match> best;
    int bestScore = 0;
    for (const auto& g : response.value("data", json::array())) {
        if (!g.is_object() || !g.contains("id") || !g["id"].is_number_integer()) {
            continue;
        }
        const std::string original = g.value("name", std::string{});
        const auto name = words(original);
        int s = 0;
        for (const auto& query : queries) {
            s = std::max(s, score(original, name, query));
        }
        if (s <= 0) {
            continue;
        }
        const int released = year_of(g);
        if (year != 0 && released == year) {
            s += 200;  // the one from the right year, of several with this name
        }
        if (preferNewest && released != 0) {
            s += released - 1970;  // a remake: of the same name, the newer game
        }
        if (g.value("verified", false)) {
            s += 10;
        }
        if (g.contains("types") && g["types"].is_array() && !g["types"].empty()) {
            s += 40;  // on a real storefront
        }
        if (s > bestScore) {
            bestScore = s;
            best = Match{g["id"].get<int64_t>(), original, s};
        }
    }
    return bestScore >= 300 ? best : std::nullopt;
}

}  // namespace

std::optional<int64_t> pick_game(const std::string& apGame, const json& response) {
    const auto match = match_game(apGame, response);
    return match ? std::optional<int64_t>{match->id} : std::nullopt;
}

std::vector<Candidate> candidates(const json& response) {
    std::vector<Candidate> out;
    if (!response.is_object() || !response.value("success", false)) {
        return out;
    }
    for (const auto& g : response.value("data", json::array())) {
        if (g.is_object() && g.contains("id") && g["id"].is_number_integer()) {
            out.push_back({g["id"].get<int64_t>(), g.value("name", std::string{}), year_of(g)});
        }
    }
    return out;
}

std::optional<Match> match_game(const std::string& apGame, const json& response) {
    if (!response.is_object() || !response.value("success", false)) {
        return std::nullopt;
    }
    const int year = alias_year(apGame);
    if (auto exact = best_match(response, {words(search_term(apGame)), words(apGame)}, year, false)) {
        return exact;
    }
    // Nothing by the full name: the next best thing, without brackets and edition words.
    const std::string fallback = fallback_term(apGame);
    if (fallback.empty()) {
        return std::nullopt;
    }
    const auto remade = words(search_term(apGame));
    const bool remake = std::find(remade.begin(), remade.end(), "remake") != remade.end();
    return best_match(response, {words(fallback)}, year, remake);
}

std::string pick_grid(const json& response) {
    if (!response.is_object() || !response.value("success", false)) {
        return {};
    }
    std::string fallback;
    for (const auto& g : response.value("data", json::array())) {
        if (!g.is_object()) {
            continue;
        }
        std::string url = g.value("thumb", std::string{});
        if (url.empty()) {
            url = g.value("url", std::string{});
        }
        if (url.empty() || !url.starts_with("https://")) {
            continue;
        }
        if (g.value("height", 0) > g.value("width", 0)) {
            return url;  // listed best first; portrait looks like a box
        }
        if (fallback.empty()) {
            fallback = url;
        }
    }
    return fallback;
}

std::string url_encode(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c < 0x80 && std::isalnum(c) != 0) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

std::string file_stem(const std::string& game) {
    std::string stem;
    for (const char ch : game) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80 && std::isalnum(c) != 0) {
            stem += static_cast<char>(std::tolower(c));
        } else if (!stem.empty() && stem.back() != '-') {
            stem += '-';
        }
        if (stem.size() >= 48) {
            break;
        }
    }
    while (!stem.empty() && stem.back() == '-') {
        stem.pop_back();
    }
    uint32_t hash = 2166136261u;  // FNV-1a: two names that flatten alike still get two files
    for (const char ch : game) {
        hash = (hash ^ static_cast<unsigned char>(ch)) * 16777619u;
    }
    char suffix[10];
    std::snprintf(suffix, sizeof(suffix), "%08x", hash);
    return (stem.empty() ? std::string{"game"} : stem) + "-" + suffix;
}

std::optional<Image> process(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || size > 32u * 1024 * 1024) {
        return std::nullopt;
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
    if (decoded == nullptr) {
        return std::nullopt;
    }
    if (w < 8 || h < 8 || w > 8192 || h > 8192) {
        stbi_image_free(decoded);
        return std::nullopt;
    }
    Pixels src;
    src.w = static_cast<uint32_t>(w);
    src.h = static_cast<uint32_t>(h);
    src.px.assign(decoded, decoded + static_cast<size_t>(w) * h * 4);
    stbi_image_free(decoded);

    Image out;
    out.aspect = static_cast<float>(w) / static_cast<float>(h);
    const bool landscape = w > h;
    const uint32_t tw = landscape ? 384 : 256;
    const uint32_t th = landscape ? 256 : 384;
    while (src.w >= tw * 2 && src.h >= th * 2) {
        src = halve(src);
    }
    Pixels level = bilinear(src, tw, th);

    uint64_t sum[3] = {};
    for (size_t i = 0; i < level.px.size(); i += 4) {
        level.px[i + 3] = 255;
        for (int c = 0; c < 3; ++c) {
            sum[c] += level.px[i + c];
        }
    }
    // Edges: the art's average colour, a little richer and darker so the cover stands out.
    const size_t count = level.px.size() / 4;
    const float avg[3] = {static_cast<float>(sum[0]) / count, static_cast<float>(sum[1]) / count,
        static_cast<float>(sum[2]) / count};
    const float grey = (avg[0] + avg[1] + avg[2]) / 3.0f;
    for (int c = 0; c < 3; ++c) {
        const float rich = grey + (avg[c] - grey) * 1.4f;
        out.spine[c] = static_cast<uint8_t>(std::clamp(rich * 0.6f, 0.0f, 255.0f));
    }

    out.width = tw;
    out.height = th;
    out.mips = 7;  // down to 4x6: every level halves exactly
    Pixels half;
    for (uint32_t i = 0; i < out.mips; ++i) {
        out.texels.insert(out.texels.end(), level.px.begin(), level.px.end());
        if (i + 1 < out.mips) {
            level = halve(level);
            if (i == 0) {
                half = level;  // 128x192 (or 192x128): the icon's art
            }
        }
    }

    // Icon: 192x192, the art centred on its long side, transparent around it.
    Pixels icon;
    icon.w = 192;
    icon.h = 192;
    icon.px.assign(static_cast<size_t>(icon.w) * icon.h * 4, 0);
    const uint32_t ox = (icon.w - half.w) / 2;
    const uint32_t oy = (icon.h - half.h) / 2;
    for (uint32_t y = 0; y < half.h; ++y) {
        std::memcpy(&icon.px[(static_cast<size_t>(y + oy) * icon.w + ox) * 4],
            &half.px[static_cast<size_t>(y) * half.w * 4], static_cast<size_t>(half.w) * 4);
    }
    out.iconSize = icon.w;
    out.iconMips = 6;  // 192 down to 6
    for (uint32_t i = 0; i < out.iconMips; ++i) {
        out.icon.insert(out.icon.end(), icon.px.begin(), icon.px.end());
        if (i + 1 < out.iconMips) {
            icon = halve(icon);
        }
    }
    return out;
}

}  // namespace ap::cover_art
