Dusklight Archipelago adds full Archipelago multiworld support to Dusklight, the Twilight Princess PC port. It's built on the official Dusklight randomizer.
You don't need a separate client and you don't patch anything. Pick Archipelago from the Play menu, enter your server and slot name, and the mod rebuilds your seed in-game with the randomizer's own generator. A multiworld seed plays exactly like a normal rando seed, with the same logic, stage edits, text and progressive items.
Features

* Connects from inside the game. Checks go out as you collect them, and items from other players arrive automatically. Anything you pick up while disconnected gets sent the next time you connect, and saves reconnect on their own.
* Other players' items say whose they are when you find them ("You found Ava's Gunpowder!") and can show as their game's box with its cover art, on the ground, in Link's hands and in the text box. Covers come from SteamGridDB with your own free API key; without one they stay Sols.
* Item jingles match how important the item is to its owner: the full fanfare for progression, smaller ones for useful items and filler, and a "nope" sound for traps.
* Built-in Archipelago tab (F1):
   * Status: connection, reconnect or change server, death link toggle
   * Tracker showing which checks are in logic right now, worked out from the seed's own logic. Checks holding items for players who've already finished get crossed out so you can skip them
   * Hints: every hint involving you, request by item or location, set Priority/Avoid
   * Room chat and server commands, with emoji and an emoji picker
* Optional HUD overlay you can put anywhere on screen. It shows status, checks, received items, hints, chat and death link.
* Death link. Bottled fairies still save you from deaths other players send, and deaths wait until cutscenes end.
* Goal is sent automatically when you beat Ganondorf.
* Transform Anywhere logic just works: the mod allows it itself when your YAML expects it, no cheat needed.
* Works with ws:// and wss://. The mod has its own TLS with proper certificate checks and supports compressed messages.
* Seven ready-made presets (under github releases):
   * Easy: about 150 checks
   * Normal: about 200 checks
   * Hard: about 300 checks
   * Extreme: about 450 checks
   * Ultimate, Hero of Twilight and Hero of Time: every check in the game (about 570) at Normal, Hard and hardest difficulty
* Shuffled Dungeons option (0–9) so you can control how long a run is.
* Optional Collect Dungeon on Completion: beat a boss and every check left in that dungeon is collected at once, for everyone.
* Refuses a seed made with a different apworld version, so logic never silently differs from Archipelago's.
* One download covers every platform Dusklight runs on, and it runs alongside Dusklight's built-in randomizer without conflicts.

Requirements: Dusklight 2.0.1 or newer. Put `archipelago.dusk` in your Dusklight mods folder and `tp_dusklight.apworld` in Archipelago's `custom_worlds` folder. You'll also need a YAML: use one of the presets or the included template. Game boxes need a free SteamGridDB API key (optional).
This fork's code, apworld and documentation were written/assisted with Claude Code. I, NoahsMaximum have been directing, reviewing and play-testing every version. The randomizer is built off of TwilitRealm's randomizer. Every release goes through manual and automated tests (each preset's seeds rebuilt in the game's own generator and compared exactly, the TLS and compression suites, builds on every platform), but not every change has been played in game before release, so edge-case bugs might get through. Please [open an issue](https://github.com/noahsmaximum/dusklight-archipelago/issues) if you find anything.
