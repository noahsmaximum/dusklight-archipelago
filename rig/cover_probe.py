"""Checks cover matching against the live SteamGridDB API.

Searches SteamGridDB for Archipelago game names the way the mod does (same aliases, read from
src/ap/cover_art.cpp), saves the replies to search.json, downloads a few covers, then runs
tls_test --covers over them so the mod's own C++ picks each game.

    python rig/cover_probe.py [out_dir] [game ...]

Needs a SteamGridDB API key: SGDB_KEY in the environment, or rig/sgdb_key.txt (git-ignored;
never commit a key).
"""
import json
import pathlib
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request

import os

import paths

RIG = pathlib.Path(__file__).resolve().parent
REPO = paths.REPO
KEY = os.environ.get("SGDB_KEY") or (RIG / "sgdb_key.txt").read_text().strip()

GAMES = [
    "A Link to the Past", "A Short Hike", "Adventure", "Aquaria", "Banjo-Tooie", "Blasphemous",
    "Bomb Rush Cyberfunk", "Castlevania 64", "Castlevania - Circle of the Moon", "Celeste 64",
    "Celeste (Open World)", "ChecksFinder", "Civilization VI", "Clique", "Dark Souls III",
    "Dark Souls Remastered", "DLCQuest", "Donkey Kong Country 3", "DOOM 1993", "DOOM II",
    "Factorio", "Faxanadu", "Final Fantasy", "Final Fantasy Mystic Quest", "Hades", "Heretic",
    "Hollow Knight", "Hylics 2", "Inscryption", "Jak and Daxter: The Precursor Legacy",
    "Kingdom Hearts", "Kingdom Hearts 2", "Kirby's Dream Land 3",
    "Landstalker - The Treasures of King Nole", "Lingo", "Links Awakening DX",
    "Lufia II Ancient Cave", "Mario & Luigi Superstar Saga", "MegaMan Battle Network 3",
    "Mega Man 2", "Meritous", "Metroid Prime", "Minecraft", "Muse Dash", "Noita",
    "Ocarina of Time", "Old School Runescape", "Ori and the Blind Forest", "Overcooked! 2",
    "Paper Mario", "Pokemon Crystal", "Pokemon Emerald", "Pokemon Red and Blue", "Raft",
    "Risk of Rain 2", "Rogue Legacy", "Secret of Evermore", "Shivers", "Slay the Spire", "SMZ3",
    "Sonic Adventure 2 Battle", "Starcraft 2", "Stardew Valley", "Subnautica",
    "Super Mario 64", "Super Mario Land 2", "Super Mario Sunshine", "Super Mario World",
    "Super Metroid", "Terraria", "The Legend of Zelda", "The Legend of Zelda - Oracle of Seasons",
    "The Messenger", "The Wind Waker", "The Witness", "Timespinner", "TUNIC",
    "Twilight Princess (Dusklight)", "Undertale", "VVVVVV", "Wargroove", "Yoshi's Island",
    "Yu-Gi-Oh! 2006", "Zillion", "Zork Grand Inquisitor", "Majora's Mask Recompiled",
    "Resident Evil 2 Remake", "Resident Evil 3 Remake",
]
COVERS_FOR = ["Ocarina of Time", "Hollow Knight", "Factorio", "A Link to the Past"]


def aliases():
    src = (REPO / "src" / "ap" / "cover_art.cpp").read_text(encoding="utf-8")
    return dict(re.findall(r'\{"([^"]+)", \{"([^"]+)"', src))


def get(url, auth=True):
    req = urllib.request.Request(url, headers={"User-Agent": "cover_probe"})
    if auth:
        req.add_header("Authorization", "Bearer " + KEY)
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def main():
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else paths.WORK / "cover_probe"
    games = sys.argv[2:] or GAMES
    out.mkdir(parents=True, exist_ok=True)
    alias = aliases()
    replies = {}
    for game in games:
        term = alias.get(game, game)
        url = "https://www.steamgriddb.com/api/v2/search/autocomplete/" + urllib.parse.quote(term, safe="")
        replies[game] = json.loads(get(url))
        time.sleep(0.25)
    # The mod searches again with a shorter name when the full one finds nothing.
    terms = subprocess.run([str(paths.TLS_TEST), "--fallback-terms", *games], check=True,
                           capture_output=True, text=True, encoding="utf-8").stdout.splitlines()
    for game, term in zip(games, terms):
        if term.strip():
            url = ("https://www.steamgriddb.com/api/v2/search/autocomplete/" +
                   urllib.parse.quote(term.strip(), safe=""))
            replies[game + "\tfallback"] = json.loads(get(url))
            time.sleep(0.25)
    (out / "search.json").write_text(json.dumps(replies, indent=1), encoding="utf-8")

    images = []
    for game in COVERS_FOR:
        data = replies.get(game, {}).get("data", [])
        if not data:
            continue
        # The first exact-ish hit is enough to exercise decoding.
        gid = next((g["id"] for g in data if game.lower() in g["name"].lower()), data[0]["id"])
        grids = json.loads(get(f"https://www.steamgriddb.com/api/v2/grids/game/{gid}?dimensions="
                               "600x900,342x482,660x930&types=static&nsfw=false&humor=false&epilepsy=false"))
        if not grids.get("data"):
            continue
        thumb = grids["data"][0].get("thumb") or grids["data"][0]["url"]
        path = out / (re.sub(r"[^a-z0-9]+", "-", game.lower()) + pathlib.Path(thumb).suffix)
        path.write_bytes(get(thumb, auth=False))
        images.append(str(path))

    exe = paths.TLS_TEST
    subprocess.run([str(exe), "--covers", str(out / "search.json"), *images], check=True)


if __name__ == "__main__":
    main()
