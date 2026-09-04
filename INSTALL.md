# Playing Ultra Street Fighter IV with rollback

This is a mod. It does not change any game file. It starts the game and adds
rollback netcode while it runs, so you can remove it by deleting this folder.

## What you need

* Ultra Street Fighter IV on Steam, installed and run at least once.
* Steam running and signed in.
* Windows 10 or later. Linux and Steam Deck: see the end of this file.
* The address of a lobby server, which whoever gave you this folder has
  already put in `server.txt`.

## Installing

1. Extract this whole folder anywhere you like. The Desktop is fine. Do **not**
   copy the files into the Street Fighter folder.
2. Double-click `Launcher.exe`.

That is the entire installation. The launcher finds your Steam copy of the game
by itself and starts it.

## Playing someone

No IP addresses, no port forwarding. One of you creates a lobby and gets a
six-character code; the other types it in.

1. From the game's main menu choose **Multiplayer Battle**. The Network window
   opens. (You can also open it any time by moving the mouse to the top of the
   screen and choosing Network.)
2. If it asks, press Start or light kick on the controller you want to use.
3. Type your name.
4. **Player 1:** press **Create lobby**. A code like `K7PQ2M` appears. Send it to
   your opponent.
   **Player 2:** press **Join with code**, type the code, press **Join**.
5. Both of you: pick a character and press **Send chara**. When both are ready
   the match starts.

**Input delay** adds a few frames of delay in exchange for less visual rollback.
Start at 1 or 2. Raise it if the match stutters.

## If something goes wrong

* **"No lobby server configured"**: `server.txt` next to `Launcher.exe` is
  missing or empty. Ask whoever gave you the mod for the address.
* **"No answer from ..."**: the server is down or a firewall is blocking UDP.
* **"the other player runs a different sf4e build"**: you have different
  versions. Both of you should use the same zip.
* Logs are written to `%APPDATA%\sf4e\logs\`. Paste `sf4e.log` into a bug
  report, with the characters played and what each of you was using to play.

Steam's own online modes are disabled while the mod is running.

## Linux and Steam Deck

Install [protontricks](https://github.com/Matoking/protontricks), then run:

```
protontricks-launch --appid 45760 Launcher.exe
```

## Credits

Built on [sf4e](https://codeberg.org/adanducci/sf4e) by Anthony Danducci, which
does the hard part: the reverse-engineered engine, the save states and the GGPO
integration.

Street Fighter and Ultra Street Fighter IV are copyright CAPCOM.
