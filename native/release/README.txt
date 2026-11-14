Army of Two: The Devil's Cartel – Native PC port
Public test build {VERSION}
================================================

Fan project. Not affiliated with Electronic Arts or Visceral Games. All trademarks belong
to their owners.

NO GAME DATA INCLUDED
---------------------
This package contains no game files (no graphics, audio, videos, levels). You need your own
copy of the game as an ISO image of the Xbox 360 disc:

  Army of Two: The Devil's Cartel, Xbox 360, Europe (English/German/Italian)

Other versions are not supported yet; the launcher checks this during setup.

SYSTEM REQUIREMENTS
-------------------
- Windows 10 or 11, 64-bit
- DirectX 12 graphics card
- CPU with SSE4.1 (all CPUs since about 2008)
- about 8 GB free space for the extracted game files

FIRST START
-----------
1. Extract the folder somewhere with enough space (not into "Program Files").
2. Run AO4Launcher.exe.
3. "Browse..." -> select your ISO -> "Install".
   The game files are extracted once into game\. The ISO is not needed afterwards.

PLAYING
-------
Choose the mode on the "Play" tab:
- Single player
- Split-screen co-op: player 1 mouse/keyboard, player 2 first controller. Choose
  co-op/split screen in the game, player 2 presses Start.
- Online/LAN co-op host or join: same network or a virtual LAN only (Tailscale, ZeroTier,
  Hamachi). The host gives the shown address to the partner. In the game: MISSIONS > Xbox LIVE.
  When hosting for the first time Windows asks about the firewall: choose "Allow access". If
  that is not enough: LAN-Koop-Firewall.cmd (asks for admin rights, undo with -Remove).

Profiles: create player profiles on the "Profiles" tab and import an existing save
(PROFILE_OPTIONS file, e.g. from Xenia). Player slot 1 owns the save games; player slot 2 is
the second split-screen player. Which keyboard/controller belongs to which player is set on
the "Play" tab for split screen.

Controls: key bindings, port keys (F4/F9/F6), mouse and mounted-gun sensitivity on the
"Controls" tab. Partner: Bravo mode and custom AI partner gear on the "Partner" tab.
Optional on "Settings": all masks, outfits and tattoos buyable right away (they still cost
in-game money).

KEYS IN GAME
------------
  F4    port menu (mouse sensitivity, Y axis, audio, game fixes)
  F9    Bravo mode (play as Bravo, in the main menu or a level)
  F6    custom gear for the AI partner on/off
  F2    direct mouse / stick mode
  F3    performance overlay
  F12   mark a bug spot in the log (for bug reports)
Key bindings: config\input-bindings.json

SETTINGS
--------
Graphics, audio, controls and game fixes in the launcher on the "Settings" tab.
60 FPS is experimental and off by default. Known issue: occasionally an enemy stays idle in
cover.

KNOWN ISSUES
------------
- Very rarely the game crashes during startup. Just start it again.
- Online/LAN co-op between two PCs has barely been tested – feedback is very welcome.

REPORTING BUGS
--------------
1. Press F12 in the game where the problem happens.
2. Launcher > Help > "Pack logs for a bug report (ZIP)".
3. Send the ZIP with a short description (mode, what happened).
The ZIP contains only the logs and config folders (no save games, no game data).

FOLDERS
-------
  bin\       port program files
  config\    settings
  game\      your extracted game files (do not share!)
  profiles\  profiles and save games
  logs\      logs
  server\    co-op server for the host (embedded Python)

Uninstall: delete the folder. Nothing is written outside of it.
