// Host-side test for the mod's network client: the TLS layer (src/ap/tls.cpp), the text
// sanitizer (src/ap/text_safe.hpp), WebSocket decompression (src/ap/ws_deflate.cpp) and cover
// matching (src/ap/cover_art.cpp).
//
// TlsStream never touches a socket itself, so it can be driven over ordinary blocking
// sockets here, against real servers, without launching the game. Run with no arguments
// for the default suite, which checks both that good certificates are accepted and that
// bad ones are rejected:
//
//     tls_test
//     tls_test archipelago.gg:443=ok expired.badssl.com:443=fail
//     tls_test --offline      (everything but the TLS cases: no network needed)
//
// Exits non-zero if any case did not behave as expected.
//
//     tls_test --covers <search.json> [images...]
//
// runs cover matching over saved SteamGridDB searches ({"game": response, ...}) and decodes
// images the way the mod does, printing what it picked (rig/cover_probe.py makes the file).

#include "ap/cover_art.hpp"
#include "ap/emoji.hpp"
#include "ap/text_safe.hpp"
#include "ap/tls.hpp"
#include "ap/ws_deflate.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#define close_socket ::close
#endif

namespace {

struct Case {
    std::string host;
    std::string port;
    bool expectSuccess = true;
};

socket_t dial(const std::string& host, const std::string& port, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &results) != 0 || results == nullptr) {
        error = "could not resolve " + host;
        return kInvalidSocket;
    }
    socket_t fd = kInvalidSocket;
    for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kInvalidSocket) {
            continue;
        }
        if (connect(fd, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            break;
        }
        close_socket(fd);
        fd = kInvalidSocket;
    }
    freeaddrinfo(results);
    if (fd == kInvalidSocket) {
        error = "could not connect to " + host + ":" + port;
    }
    return fd;
}

// Returns true when the case behaved as expected.
bool run(const Case& item) {
    std::printf("%-32s ", (item.host + ":" + item.port).c_str());
    std::fflush(stdout);

    std::string error;
    const socket_t fd = dial(item.host, item.port, error);
    if (fd == kInvalidSocket) {
        std::printf("SKIP  (%s)\n", error.c_str());
        return true;  // a network problem is not a TLS result
    }

#ifdef _WIN32
    DWORD timeout = 10000;
#else
    timeval timeout{10, 0};
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    ap::TlsStream tls;
    bool writeFailed = false;
    const bool started = tls.start(item.host, [&](const char* data, size_t size) {
        size_t sent = 0;
        while (sent < size) {
            const int n = static_cast<int>(send(fd, data + sent, static_cast<int>(size - sent), 0));
            if (n <= 0) {
                writeFailed = true;
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    });

    bool ok = started;
    char buffer[8192];
    while (ok && !tls.handshake_done()) {
        const int n = static_cast<int>(recv(fd, buffer, sizeof(buffer), 0));
        if (n <= 0) {
            ok = false;
            if (tls.error().empty()) {
                error = writeFailed ? "the connection dropped while sending" :
                                      "the connection dropped during the handshake";
            }
            break;
        }
        tls.feed(buffer, static_cast<size_t>(n));
        ok = tls.pump();
    }

    std::string firstLine;
    if (ok && tls.handshake_done()) {
        // Prove application data survives the round trip in both directions.
        const std::string request = "HEAD / HTTP/1.1\r\nHost: " + item.host +
                                    "\r\nConnection: close\r\nUser-Agent: tls_test\r\n\r\n";
        ok = tls.write(request.data(), request.size());
        while (ok && firstLine.empty()) {
            const int n = static_cast<int>(recv(fd, buffer, sizeof(buffer), 0));
            if (n <= 0) {
                break;
            }
            tls.feed(buffer, static_cast<size_t>(n));
            if (!tls.pump()) {
                ok = false;
                break;
            }
            const std::string text = tls.take_plaintext();
            const auto end = text.find("\r\n");
            if (end != std::string::npos) {
                firstLine = text.substr(0, end);
            }
        }
    }

    close_socket(fd);

    const bool succeeded = ok && tls.handshake_done();
    const std::string reason = !tls.error().empty() ? tls.error() : error;
    if (succeeded == item.expectSuccess) {
        if (succeeded) {
            std::printf("PASS  connected, server said: %s\n",
                firstLine.empty() ? "(no response line)" : firstLine.c_str());
        } else {
            std::printf("PASS  rejected: %s\n", reason.c_str());
        }
        return true;
    }
    if (succeeded) {
        std::printf("FAIL  handshake succeeded but should have been rejected\n");
    } else {
        std::printf("FAIL  %s\n", reason.empty() ? "handshake failed" : reason.c_str());
    }
    return false;
}

// The other half of what protects us from a hostile room: everything the server says goes
// through message_safe() before it reaches the game's message renderer or a toast.
bool check_text_safety() {
    struct Check {
        const char* what;
        std::string input;
        size_t cap;
        std::string expected;
    };
    const std::vector<Check> checks{
        {"plain text survives", "Link's Sword", 64, "Link's Sword"},
        {"newlines survive", "a\nb", 64, "a\nb"},
        {"tag escapes are dropped", "a\x1A\x05qqqqb", 64, "aqqqqb"},
        {"nulls are dropped", std::string("a\0b", 3), 64, "ab"},
        // Split literals: a hex escape swallows every hex digit that follows it.
        {"control bytes are dropped", "a\x01\x02\x7F" "b", 64, "ab"},
        {"length is capped", std::string(500, 'x'), 16, std::string(16, 'x')},
        {"utf-8 is not cut in half", "aaa\xC3\xA9", 4, "aaa"},
    };

    bool ok = true;
    for (const Check& check : checks) {
        const std::string got = ap::message_safe(check.input, check.cap);
        std::printf("%-32s ", check.what);
        if (got == check.expected) {
            std::printf("PASS\n");
        } else {
            std::printf("FAIL  got %zu bytes, expected %zu\n", got.size(), check.expected.size());
            ok = false;
        }
    }
    return ok;
}

// Emoji become inline images (the game's fonts have none); everything else must pass through
// byte for byte, since this runs on text that has already been escaped for RML.
bool check_emoji() {
    const auto img = [](const char* file) {
        return "<img class=\"ap-emoji\" src=\"" + ap::emoji::image_source(file) +
               "\" style=\"width: 1.3em; height: 1.3em; vertical-align: -0.25em;\"/>";
    };
    struct Check {
        const char* what;
        std::string got;
        std::string expected;
    };
    using ap::emoji::emojify;
    using ap::emoji::shortcodes_to_unicode;
    const std::vector<Check> checks{
        {"emoji: plain text untouched", emojify("gg &amp; 12:30:45 caf\xC3\xA9"),
            "gg &amp; 12:30:45 caf\xC3\xA9"},
        {"emoji: single codepoint", emojify("hi \xF0\x9F\x98\x82!"), "hi " + img("1f602") + "!"},
        {"emoji: VS16 dropped for lookup", emojify("\xE2\x9D\xA4\xEF\xB8\x8F"), img("2764")},
        {"emoji: ZWJ sequence", emojify("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB"),
            img("1f468-200d-1f4bb")},
        {"emoji: skin tone -> default", emojify("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"), img("1f44d")},
        {"emoji: flag pair", emojify("\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5"), img("1f1ef-1f1f5")},
        {"emoji: keycap", emojify("#\xEF\xB8\x8F\xE2\x83\xA3 5"), img("23-20e3") + " 5"},
        {"emoji: shortcode shown", emojify("gg :joy: :nope:"), "gg " + img("1f602") + " :nope:"},
        {"emoji: shortcode sent as emoji", shortcodes_to_unicode(":joy: :heart: :nope:"),
            "\xF0\x9F\x98\x82 \xE2\x9D\xA4\xEF\xB8\x8F :nope:"},
        {"emoji: broken utf-8 kept", emojify(std::string("a\xF0\x9F" "b")),
            std::string("a\xF0\x9F" "b")},
    };
    bool ok = true;
    for (const Check& check : checks) {
        std::printf("%-32s ", check.what);
        if (check.got == check.expected) {
            std::printf("PASS\n");
        } else {
            std::printf("FAIL  got '%s'\n", check.got.c_str());
            ok = false;
        }
    }
    return ok;
}

bool check_covers() {
    using ap::cover_art::file_stem;
    using ap::cover_art::pick_game;
    using ap::cover_art::pick_grid;
    using ap::cover_art::url_encode;
    using nlohmann::json;
    const json oot = json::parse(R"({"success":true,"data":[
        {"id":5355393,"name":"Ocarina of Time Spaceworld '97 Beta Experience","verified":true,"types":[]},
        {"id":5465593,"name":"Ocarina of Time + Majora's Mask Combo Randomizer","verified":true,"types":[]},
        {"id":21202,"name":"The Legend of Zelda: Ocarina of Time","verified":true,"types":["eshop"]},
        {"id":36271,"name":"The Legend of Zelda: Ocarina of Time 3D","verified":true,"types":[]}]})");
    const json hk = json::parse(R"({"success":true,"data":[
        {"id":7545,"name":"Hollow Knight","verified":true,"types":["steam","gog"]},
        {"id":1043,"name":"Hollow Knight: Silksong","verified":true,"types":["steam"]}]})");
    const json unrelated = json::parse(R"({"success":true,"data":[
        {"id":1,"name":"Checkers Deluxe","verified":true,"types":["steam"]}]})");
    const json re2 = json::parse(R"({"success":true,"data":[
        {"id":25577,"name":"Resident Evil 2","verified":true,"types":["steam"],"release_date":900000000},
        {"id":29143,"name":"Resident Evil 2","verified":true,"types":["steam"],"release_date":1548374400},
        {"id":32193,"name":"Resident Evil 2: 1-Shot Demo","verified":true,"types":["steam"],"release_date":1547000000}]})");
    const json grids = json::parse(R"({"success":true,"data":[
        {"width":920,"height":430,"thumb":"https://cdn/wide.jpg"},
        {"width":600,"height":900,"thumb":"https://cdn/tall.jpg"}]})");
    auto id = [](std::optional<int64_t> v) { return v ? std::to_string(*v) : std::string{"none"}; };
    struct Check {
        const char* what;
        std::string got;
        std::string expected;
    };
    const std::vector<Check> checks = {
        {"covers: franchise prefix", id(pick_game("Ocarina of Time", oot)), "21202"},
        {"covers: exact beats sequel", id(pick_game("Hollow Knight", hk)), "7545"},
        {"covers: no weak match", id(pick_game("ChecksFinder", unrelated)), "none"},
        {"covers: failed search", id(pick_game("Hollow Knight", json::parse(R"({"success":false})"))), "none"},
        {"covers: remake -> newest", id(pick_game("Resident Evil 2 Remake", re2)), "29143"},
        {"covers: fallback term", ap::cover_art::fallback_term("Resident Evil 2 Remake"), "Resident Evil 2"},
        {"covers: platform dropped", ap::cover_art::fallback_term("Link's Awakening Switch"), "Link's Awakening"},
        {"covers: platform words", ap::cover_art::fallback_term("Link's Awakening (Nintendo Switch)"), "Link's Awakening"},
        {"covers: title kept", ap::cover_art::fallback_term("Hollow Knight"), ""},
        {"covers: confident match", std::to_string(ap::cover_art::match_game("Ocarina of Time", oot)->score >= ap::cover_art::kConfidentScore), "1"},
        {"covers: candidates", std::to_string(ap::cover_art::candidates(re2).size()), "3"},
        {"covers: portrait grid first", pick_grid(grids), "https://cdn/tall.jpg"},
        {"covers: url encoding", url_encode("Pok\xC3\xA9mon: Red & Blue"), "Pok%C3%A9mon%3A%20Red%20%26%20Blue"},
        {"covers: file stem", file_stem("A Link to the Past").substr(0, 19), "a-link-to-the-past-"},
    };
    bool ok = true;
    for (const Check& check : checks) {
        std::printf("%-32s ", check.what);
        if (check.got == check.expected) {
            std::printf("PASS\n");
        } else {
            std::printf("FAIL  got '%s'\n", check.got.c_str());
            ok = false;
        }
    }
    return ok;
}

int covers_live(int argc, char** argv) {
    using nlohmann::json;
    std::ifstream in(argv[2]);
    const json all = json::parse(in);
    // "<game>" holds the search by its name; "<game>\tfallback", if there is one, the second
    // search the mod makes with fallback_term() when the first finds nothing.
    for (const auto& [game, response] : all.items()) {
        if (game.find('\t') != std::string::npos) {
            continue;
        }
        std::optional<ap::cover_art::Match> picked;
        if (ap::cover_art::has_box(game)) {
            picked = ap::cover_art::match_game(game, response);
            if (!picked && all.contains(game + "\tfallback")) {
                picked = ap::cover_art::match_game(game, all[game + "\tfallback"]);
            }
        }
        std::string name = ap::cover_art::has_box(game) ? "(none: stays a Sol)" : "(no box: Archipelago-only)";
        if (picked) {
            name = picked->name + (picked->score < ap::cover_art::kConfidentScore ? "  [best guess]" : "");
        }
        std::printf("%-40s -> %s\n", game.c_str(), name.c_str());
    }
    for (int i = 3; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto image = ap::cover_art::process(bytes.data(), bytes.size());
        if (!image) {
            std::printf("%s: did not decode\n", argv[i]);
            continue;
        }
        std::printf("%s: %ux%u, %u mips, %zu bytes, aspect %.2f, spine %u,%u,%u, icon %u square, %u mips, %zu bytes\n",
            argv[i], image->width, image->height, image->mips, image->texels.size(), image->aspect,
            image->spine[0], image->spine[1], image->spine[2], image->iconSize, image->iconMips,
            image->icon.size());
        // Raw RGBA of the icon's top level, to look at (rig/cover_probe.py turns it into a PNG).
        std::ofstream(std::string(argv[i]) + ".icon.rgba", std::ios::binary)
            .write(reinterpret_cast<const char*>(image->icon.data()),
                static_cast<std::streamsize>(image->iconSize) * image->iconSize * 4);
    }
    return 0;
}

#include "deflate_vectors.inc"

// permessage-deflate, against vectors from the same websockets encoder Archipelago's server
// uses. Message 2 is pure back-references into message 1, so it only decodes if the stream
// keeps its window between messages the way the server's does.
bool check_deflate() {
    bool ok = true;
    auto report = [&](const char* what, bool pass, const std::string& detail = {}) {
        // The detail is the last error, which only means something when this case failed.
        std::printf("%-32s %s%s%s\n", what, pass ? "PASS" : "FAIL",
            pass || detail.empty() ? "" : "  ", pass ? "" : detail.c_str());
        ok = ok && pass;
    };

    struct Negotiation {
        const char* what;
        const char* header;
        bool valid;
        bool accepted;
        bool noContext;
    };
    const Negotiation negotiations[] = {
        {"declined is fine", "", true, false, false},
        {"archipelago's answer accepted", "permessage-deflate; server_max_window_bits=11", true, true, false},
        {"no-context flag read", "permessage-deflate; server_no_context_takeover", true, true, true},
        {"unoffered extension refused", "x-webkit-deflate-frame", false, false, false},
        {"unknown parameter refused", "permessage-deflate; bogus=1", false, false, false},
        {"duplicate extension refused", "permessage-deflate, permessage-deflate", false, false, false},
    };
    for (const auto& n : negotiations) {
        ap::DeflateParams params;
        std::string error;
        const bool valid = ap::parse_deflate_response(n.header, params, error);
        report(n.what, valid == n.valid && (!valid || (params.accepted == n.accepted &&
                                                         params.serverNoContextTakeover == n.noContext)));
    }

    const std::string packet = kDeflatePacket;
    auto bytes = [](const unsigned char* data, size_t size) {
        return std::string(reinterpret_cast<const char*>(data), size);
    };
    std::string error;

    ap::Inflater stream;
    std::string m1 = bytes(kDeflateMsg1, sizeof(kDeflateMsg1));
    report("first message inflates", stream.inflate_message(m1, 1 << 20, false, error) && m1 == packet, error);
    std::string m2 = bytes(kDeflateMsg2, sizeof(kDeflateMsg2));
    report("second uses the first's window", stream.inflate_message(m2, 1 << 20, false, error) && m2 == packet, error);

    ap::Inflater fresh;
    std::string alone = bytes(kDeflateMsg2, sizeof(kDeflateMsg2));
    report("(and needs it: fails alone)", !(fresh.inflate_message(alone, 1 << 20, false, error) && alone == packet));

    ap::Inflater tight;
    std::string bomb = bytes(kDeflateBomb, sizeof(kDeflateBomb));
    report("2 KB inflating to 2 MB refused", !tight.inflate_message(bomb, 1 << 20, false, error));
    ap::Inflater roomy;
    std::string fine = bytes(kDeflateBomb, sizeof(kDeflateBomb));
    report("same data under a bigger cap", roomy.inflate_message(fine, 4 << 20, false, error) &&
                                                fine.size() == 2u * 1024 * 1024, error);
    return ok;
}

Case parse(const std::string& text) {
    Case item;
    std::string address = text;
    const auto eq = address.rfind('=');
    if (eq != std::string::npos) {
        item.expectSuccess = address.substr(eq + 1) != "fail";
        address = address.substr(0, eq);
    }
    const auto colon = address.rfind(':');
    if (colon != std::string::npos) {
        item.host = address.substr(0, colon);
        item.port = address.substr(colon + 1);
    } else {
        item.host = address;
        item.port = "443";
    }
    return item;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    if (argc >= 3 && std::strcmp(argv[1], "--covers") == 0) {
        return covers_live(argc, argv);
    }
    // --fallback-terms <game>...: what the mod searches second for each game ("" for none).
    if (argc >= 2 && std::strcmp(argv[1], "--fallback-terms") == 0) {
        for (int i = 2; i < argc; ++i) {
            std::printf("%s\n", ap::cover_art::fallback_term(argv[i]).c_str());
        }
        return 0;
    }

    // --offline: only the in-process checks, none of the TLS cases against real servers (CI).
    const bool offline = argc >= 2 && std::strcmp(argv[1], "--offline") == 0;
    std::vector<Case> cases;
    for (int i = offline ? 2 : 1; i < argc; ++i) {
        cases.push_back(parse(argv[i]));
    }
    if (cases.empty() && !offline) {
        cases = {
            {"archipelago.gg", "443", true},
            {"github.com", "443", true},
            {"expired.badssl.com", "443", false},
            {"wrong.host.badssl.com", "443", false},
            {"self-signed.badssl.com", "443", false},
            {"untrusted-root.badssl.com", "443", false},
        };
    }

    int failures = check_text_safety() ? 0 : 1;
    std::printf("\n");
    failures += check_emoji() ? 0 : 1;
    std::printf("\n");
    failures += check_covers() ? 0 : 1;
    std::printf("\n");
    failures += check_deflate() ? 0 : 1;
    std::printf("\n");
    for (const Case& item : cases) {
        if (!run(item)) {
            ++failures;
        }
    }
    std::printf("\n%s\n",
        failures == 0 ? "everything behaved as expected" : "SOMETHING FAILED");

#ifdef _WIN32
    WSACleanup();
#endif
    return failures == 0 ? 0 : 1;
}
