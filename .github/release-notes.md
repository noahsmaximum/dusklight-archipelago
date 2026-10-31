## Install

1. **Mod** — put `archipelago.dusk` in your Dusklight mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
2. **Apworld** — put `tp_dusklight.apworld` in your Archipelago `custom_worlds` folder.
3. **YAML** — grab a preset below, or generate a template from the Archipelago Launcher. Set `name:` to your slot name.

Then in Dusklight: use the arrows on the Play button to pick **Archipelago**, start a **new file**, and enter your server address, slot name and password. The seed builds from the server and you play.

Needs Dusklight 2.0.1 or newer. This mod carries its own copy of the randomizer, so Dusklight's built-in Randomizer doesn't need updating or removing; the two don't interfere.

## What's new in 1.8.0 (beta 1)

This is a **prerelease** for testing. Please report anything odd on the issues it came from.

- **Tracker strikes out checks nobody needs** (#3). When a player finishes their game, every check of yours holding one of their items is crossed out as "not needed" and left off the in-logic list. It updates live while you play. Turn it off on the Status tab.
- **Held items keep their own game box** (#4). Another Archipelago item spawning nearby while Link held one up could swap its cover (and its text and jingle) for the other item's. The held item now knows its own check.
- The apworld is unchanged (1.9.0), and 1.7.x multiworlds keep working: just swap in the new `archipelago.dusk`.

## Updating

Update the mod and the apworld **together**. The generating host needs the new apworld for the new presets and options; if the logic data ever differs between a player's mod and the seed, the mod refuses it and says to update. Finish any multiworld already in progress on the versions you started it with.
