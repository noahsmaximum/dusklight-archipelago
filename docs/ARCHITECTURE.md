# Architecture

How Dusklight Archipelago works, where its code lives, and the things that are easy to break.
Read this before changing logic, the network client or anything that hooks the game.
[CONTRIBUTING.md](../CONTRIBUTING.md) covers building, testing and releasing.

## The idea in one paragraph

The repo is a fork of [Twilit Realm's randomizer](https://github.com/TwilitRealm/dusklight-randomizer)
with two additions: an **apworld** that teaches Archipelago this game's logic, and an
**Archipelago game mode** inside the mod. A host generates a multiworld with the apworld like any
other game, so Archipelago's fill decides every placement using this game's logic. In the game,
the player picks the Archipelago mode, starts a new file and connects. The mod receives the
slot data (settings, every placement, location ids), writes them as the randomizer's own
`settings.yaml` / `plando.yaml`, and runs the randomizer's generator to rebuild the seed locally.
From then on it plays like a normal randomizer seed, with the mod sending checks and handing over
items from other worlds.

## How a session flows

1. **Generation (Archipelago).** `apworld/tp_dusklight` reads the randomizer's YAML data (vendored
   into the apworld by `tools/build_apworld.py`), builds regions and rules from it, and fills.
   `fill_slot_data` sends the settings, every placement in this world (our own items by name,
   other worlds' items as the "Archipelago Item" with their owner, name and classification),
   location ids, unshuffled dungeons and the logic data fingerprint.
2. **New save (mod).** `mode/saves.cpp` opens the connection window; `mode/items.cpp`'s
   `on_connected` hands the slot data to `mode/slot.cpp`, which checks the fingerprint, builds the
   lookup tables, writes the YAML files and runs the generator on a worker thread with
   `randomizer::g_archipelagoMode` set (skips local logic verification, fill and entrance
   validation: other worlds' items are opaque to it).
3. **Play.** Checks are detected from save flags (`scan_locations`) and from the item service's
   give observer (`observe_give`), queued and sent (`flush_checks`). Received items are given one
   at a time through `ItemService::give_item` (`deliver_items`; progressives resolve at
   dispatch). The goal fires from Ganondorf's final blow.

## Code map

Most of the repo is the upstream randomizer (`generator/`, `src/` outside `src/ap/`, `res/`
except what's listed here). Ours:

| Path | What |
| --- | --- |
| `apworld/tp_dusklight/` | The apworld: `__init__.py` (world, regions, fill, slot data), `logic.py` (port of the randomizer's requirement language), `options.py`, `pools.py`, `data.py` (reads the vendored YAML). |
| `src/ap/ap_mode.cpp` | The game mode's public face: registration, activate/deactivate, per-frame entry points, and the game hooks it installs. |
| `src/ap/mode/` | The mode itself, split by area. `internal.hpp` holds the shared state (`ap::internal`) and the functions files call across each other. |
| `src/ap/mode/slot.cpp` | Slot data into lookup tables; the seed rebuilt from it. |
| `src/ap/mode/items.cpp` | Client callbacks, the check resolver and give observer, sending checks, delivering items, Collect Dungeon on Completion, the goal. |
| `src/ap/mode/hooks.cpp`, `death_link.cpp`, `boxes.cpp` | Gameplay hooks (AP item scale, Transform Anywhere), death link, game box glue. |
| `src/ap/mode/saves.cpp` | The new-save connection window and save lifecycle. |
| `src/ap/mode/tracker.cpp`, `messages.cpp`, `hints.cpp`, `overlay.cpp`, `status.cpp` | The Archipelago window's tabs, and the overlay's content. |
| `src/ap/ap_client.*` | The Archipelago protocol client (packets, data package, hints). |
| `src/ap/ws_tcp.*`, `tls.*`, `ws_deflate.*` | Our own WebSocket client over raw TCP, TLS (mbedTLS) and permessage-deflate (miniz). |
| `src/ap/ap_tracker.*` | Tracker logic: rebuilds the world and searches it on a worker thread. |
| `src/ap/ap_box.*`, `ap_covers.*`, `cover_art.*` | Game boxes: drawing, cover download/cache, SteamGridDB matching and image processing. |
| `src/ap/ap_overlay.*`, `emoji.*`, `text_safe.hpp`, `data_version.*` | Overlay drawing, emoji, text sanitising, the logic data fingerprint. |
| `generator/logic/search.*`, `fill.*`, `world.hpp` | Small additions to upstream for the tracker (`_collectFilter`, an exit-time cache overload, `GetItemTable`). |
| `tools/` | `tls_test.cpp` (host tests), `ap_gen_test.cpp` (rebuilds a seed from slot data, tracker queries), `build_apworld.py`, `make_template.py`, `check_data_version.py`, `fetch_twemoji.py`. |
| `rig/` | Test and measurement scripts (see `rig/README.md`). |
| `presets/` | The seven preset YAMLs and the generated `Template.yaml`. |

## Rules that must hold

- **The apworld and the in-game generator must agree exactly.** Archipelago fills using the
  apworld's logic; the game rebuilds the seed with the randomizer's. If they disagree, items end
  up behind requirements Archipelago never knew about. `src/ap/data_version.cpp` CRCs the
  embedded logic data exactly as `data.py::data_version` does (same files, same order, `\r`
  stripped), the apworld puts that in slot data, and the mod refuses a seed whose fingerprint
  differs. `tools/check_data_version.py` (run in CI) compares the two file lists, because if they
  drift the mod refuses *every* seed. Any change to the logic port must pass
  `rig/tracker_parity.py` across the presets, and seed rebuilds must pass `rig/sweep_dungeons.py`.
- **Version the apworld when logic data changes** (`world_version` in
  `apworld/tp_dusklight/archipelago.json`, and the presets' `requires`).
- **Never iterate `.items()` of a temporary**:
  `for (auto& [k, v] : j.value("x", json::object()).items())` reads freed memory on compilers
  without C++23's range-for lifetime rules (GCC before 15). MSVC happens to work, so Windows hides
  it; on Linux it broke connecting entirely (issue #2). Name the value first.
  `grep ")\.items()" src` should find nothing.
- **The server is untrusted input** (see below).
- **Hook by exact symbol when the target is virtual or file-local**, and install optional hooks
  separately so a future Dusklight that renames one costs a feature, not the mode.

## Subsystems

### Network

- Pump the network from `mod_update`, not the game-mode tick: Dusklight pauses game ticks while a
  UI window is open, so the connect window would hang otherwise.
- The mod doesn't use Dusklight's WebSocket service (2.0.1 delivered only the first message, and
  it refuses plain `ws://` beyond loopback). `ws_tcp` is an RFC 6455 client over `NetService`
  raw TCP; `tls` wraps it in mbedTLS for `wss://`, verifying against the embedded
  `src/ap/ca_bundle.pem` with the hostname set.
- A room is either TLS or plain on a port, never both, so the client tries the likely scheme
  first (`wss` remote, `ws` loopback) and falls back.
- `permessage-deflate`: offered in the upgrade, incoming messages inflated with a persistent
  raw-deflate stream (Archipelago keeps context between messages), capped at 64 MB; outgoing
  messages aren't compressed.
- Numbers from the server are read with `json_num` where a default is harmless. DataPackage
  entries without an integer id are skipped (they're display names only). A Connected packet
  that can't be read ends the attempt with the reason shown; slot data `location_ids` without an
  id refuse the slot, since skipping one would silently never send that check.
- Cover downloads use Dusklight's `HttpService` (optional import).

### Other worlds' items

- Item `0xDC` ("Archipelago Item") stands for anything belonging to another world: the Sol model
  (`Obj_ballS`), scaled by post-hooks on `daDitem_c::set_mtx` / `daItem_c::setBaseMtx`
  (`apItemModelScale`, default 0.3).
- It has no message of its own, so the get-item demo is pointed at message 120 (the Foolish
  Item's) and its text is overridden per pickup with "You found X's Y". The override consumes
  itself, so text can never lag a pickup.
- **Which location an actor is.** The check resolver's `giver_actor` is the item actor itself
  (field item, shop item, heart container, key all resolve in their create), so `resolve_check`
  maps actor to location. The held-up item (`fpcNm_Demo_Item_e`) carries the pickup's committed
  result, which never reaches resolvers, so it takes the location resolved by the pickup just
  before it appears.
- **Game boxes.** A pre-hook on `daItemBase_c::DrawBase` (by exact symbol: it's virtual) replaces
  the Sol's model entry with a `J3DPacket` box (`ap_box.cpp`) sized from the Sol's bounds and
  placed with the model's matrix. It's unlit, fogged, and resets GX and J3D caches before and
  after. Frame interpolation uses `dusk::interp` functions resolved by mangled name through the
  symbol manifest (not exported); without them the box moves at 30 Hz. Covers come from
  SteamGridDB with the player's own key (`ap_covers.cpp`), cached once per game, decoded off-thread
  with stb_image into `GX_TF_RGBA8_PC` textures with mips.
- **The text box icon.** `dMsgScrnItem_c` builds its icon from a 3 KB buffer and sizes it from
  that image, so a post-hook on its `exec` swaps the picture's texture for the cover's own
  `ResTIMG` and draws it square. The box works out its item from the message id, so ours reads as
  the Foolish Item (`0x13`): the box that sees our text first claims it, and any other pickup
  clears the claim (boxes are reallocated at the same address).
- **Jingles.** `hookPreSetGetSubBgm` (`src/hooks.cpp`) asks `ap::ap_item_importance()` for the
  item's Archipelago classification: progression gets the full fanfare, useful the small one,
  filler the quick one, traps the save menu's "Continue? No" sound.

### Goal

The ending plays inside one stage, so scene changes can't detect it. A post-hook on the
file-local `daB_GND_Execute` fires on `mActionMode == 22` with `mDemoCamMode >= 60`. The Status
tab also has a manual "Send goal complete".

### Death link

Post-hooks on `daAlink_c::procCoDeadInit` / `procCoFogDeadInit` see every real death; a bottled
fairy takes another branch, so it isn't one. A received death sets life to 0 once no event is
running and lets the game's own check kill Link (fairies still save him); a short window marks
that death as the received one so it isn't bounced back. A per-save override lives in the save's
`ap_state` blob.

### Transform Anywhere

Mods can't reach host settings, so when the slot's `Logic Transform Anywhere` is on, the mod
reproduces Dusklight's cheat for that save: the file-local `daMidna_searchNpc` returns nullptr,
and `dMsgFlow_c::query042`'s Castle Town result 4 becomes 0 (3 in twilight). To check a static is
hookable: `symgen manifest --pdb dusklight.pdb -o out.bin --no-compress`, then grep the name.

### Collect Dungeon on Completion

The apworld sends `collect_dungeons` when the option is on: per shuffled dungeon, its locations
with our own item's number (-1 for another world's), and the triggers (the dungeon reward and
the boss heart container). When a trigger is checked, `tick_dungeon_collect` sends every
unchecked location in that dungeon, gives our items silently with one summary toast, and records
them in the save so `resolve_check` turns those chests into green rupees. Its gives are counted
in `g_redeemPending` so the give observer doesn't mistake them for server items.

### The apworld's logic

- `logic.py` parses requirement strings exactly as the C++ parser does. Setting comparisons fold
  to constants, and form/time predicates fold for exits or become reachability checks on an
  area's form-time copies for locations.
- Regions are a product graph: each area has Human/Wolf x Day/Night copies, plus Twilight Wolf
  and Twilight Human copies while its twilight is uncleared. Exits connect matching copies, and
  the C++ search's `ExpandFormTimes` becomes edges between copies.
- Entering an uncleared twilight spreads wolf passes as Twilight Wolf and human ones as
  Twilight Human, except at the three `TWILIGHT_GATES`, where Midna pulls Link across as a wolf.
  A parent's twilight bits are only tried from outside a twilight, a cleared one, or the same
  twilight section. With the Shadow Crystal, a transformable uncleared-twilight area links its
  two twilight copies.
- A macro body only sees macros defined above it (as the C++ loader registers them).
- Locations defined in several areas live in `Menu` with `can_reach(hub)` in their rule.
- **Shuffled Dungeons**: unshuffled dungeons are picked from the world's seeded random, and their
  locations are locked to vanilla items and sent to the game as explicit placements (the
  generator doesn't know about the option). Their spare keys leave the pool, and in logic their
  keys count as held. The randomizer's key logic is conservative enough that the sweep would
  otherwise deadlock; the vanilla dungeon's own order guarantees keys arrive in time.
  `generate_basic` raises if a locked item is unreachable, rather than pruning it.
- **Start inventory**: Archipelago creates start items before `create_regions` has compiled the
  rules, so `_classification` calls anything progression until then and `create_regions`
  reclassifies the precollected items.
- Defaults are "max checks"; presets set every option explicitly so a default change can't move
  them. Option descriptions come from `settings_list.yaml`.
- Archipelago's `custom_worlds` only loads `.apworld` files, and a stale copy under `worlds/`
  silently wins: `tools/build_apworld.py --install` warns about that.

### The tracker

`ap_tracker.cpp` rebuilds the seed's world from the save's own `settings.yaml` / `plando.yaml`
on a worker thread and runs the generator's accessible-locations search with a `_collectFilter`:
only non-check locations give up their items (events, vanilla and unshuffled-dungeon contents).
The inventory is received items plus our own items at checked locations. To match the apworld it
re-caches the exit form-time cache with every item (Archipelago mode empties the pool, and the
cache built from that blocked almost everything), and holds unshuffled dungeons' keys as the
apworld does. `rig/tracker_parity.py` compares the two sphere by sphere.

"Not needed" checks don't touch logic: on connect the client Gets and SetNotifies every other
player's `_read_client_status_<team>_<slot>`, and a check whose item (slot data `placements`)
belongs to a player at CLIENT_GOAL (30) is shown struck out. Item link groups are skipped; they
have no status of their own.

### UI and the host

- A control's `help_rml` fills the tab's right pane, replacing whatever else was there, so tabs
  that put content in that pane (Hints, Messages) use none.
- A group button must sit directly in the tab's left pane. Control labels are plain text, and
  toasts don't render `<img>`.
- **Emoji**: Twemoji Mozilla (COLR) is loaded as an RmlUi fallback font through `Rml::LoadFontFace`,
  resolved by mangled name; the message log also swaps emoji for Twemoji PNGs from `res/emoji`,
  which handle ZWJ sequences and flags (`tools/fetch_twemoji.py` regenerates them).
- **Overlay**: a post-hook on `dMeter2Draw_c::draw` draws with `J2DFillBox` and the game's message
  font (which draws from the baseline). Hide only on `dComIfGp_isPauseFlag()`:
  `dScnPly_c::isPause()` is hit-stop and makes it flicker. Mods have no pointer input, so it's
  placed by settings.

## Treating the server as untrusted input

The mod makes only outbound connections, but the server drives seed generation, items and text,
so its data is checked at every sink:

- **Text** (`text_safe.hpp`): every server string goes through `message_safe()` before the
  game's message renderer or a toast. Control bytes are stripped (including the message format's
  in-band tag escape), length is capped, and UTF-8 is never cut mid-sequence.
- **Buffers** (`ws_tcp.cpp`, `tls.cpp`): the upgrade response, a frame, a fragmented message and
  the ciphertext queue all have caps.
- **Item ids** are range-checked before `give_item` and resolve through a switch or a map, never
  an array index.
- **The seed name** is reduced to alphanumerics, `-` and `_` before it becomes a path.
- **Certificates** are verified with the hostname set; `tls_test` covers acceptance and refusal.
- WebSocket masking keys come from the TLS CSPRNG.

Accepted: the room password is stored in the save in plain text, and plain `ws://` is
unauthenticated by nature. The CA bundle is frozen at build time; refresh it at each release.

## Not supported

- Entrance randomization and the randomizer's in-game hints are forced off in the apworld: both
  would need the generator to know about other worlds.
- The apworld isn't in Archipelago's main repository.
