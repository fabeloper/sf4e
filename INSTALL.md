# SF4Enhanced: rollback netcode for Ultra Street Fighter IV

This is a mod. It does not change any game file. It starts the game and adds
rollback netcode while it runs, so you can remove it by deleting this folder.

## What you need

* Ultra Street Fighter IV on Steam, installed and run at least once.
* Steam running and signed in.
* Windows 10 or later. Linux and Steam Deck: see the end of this file.
* A controller, or the keyboard.

## Installing

1. **Before extracting**, right-click the downloaded `.zip`, choose
   **Properties**, tick **Unblock** near the bottom, and click OK. This is the
   one step that stops the blue "Windows protected your PC" box from appearing
   later. It is safe: it only clears the "downloaded from the internet" mark.
2. Extract this whole folder anywhere you like. The Desktop is fine. Do **not**
   copy the files into the Street Fighter folder.
3. Double-click `SF4Enhanced.exe`.

That is all. It finds your Steam copy of the game by itself, starts it, and
already knows where the lobby server is.

If the blue **"Windows protected your PC"** box appears anyway, the app is not
dangerous, it is only unsigned. Click **More info**, then **Run anyway**. Your
antivirus may also warn, because the mod works by attaching to the game; that
is expected. Unblocking the zip first (step 1) usually avoids all of this.

## Staying up to date

When the lobby screen shows **UPDATE AVAILABLE**, your build is older than the
server's and you will not be able to join until you update. Press **Y** to open
the download page, grab the newest release, and replace this folder with it.
Everyone in a match must be on the same version.

## Playing someone

No IP addresses, no port forwarding. One of you creates a lobby and gets a
six-character code; the other types it in.

1. From the game's main menu choose **Multiplayer Battle**. The lobby opens.
2. Press **Start** on the controller you want to play with.
3. **Player 1:** choose **Create lobby**. A code like `K7PQ2M` appears in gold.
   Send it to your opponent.
   **Player 2:** choose **Join with code** and enter it.
4. Pick a character and press **Start** to ready up. The match begins when both
   players are ready.
5. When it ends, choose **Rematch**, **Change character**, or **Leave**.

Controls in the lobby: d-pad or stick to move, **A** to confirm, **B** to go
back, left and right to change an option, **Start** to ready up. On the
keyboard: arrows, Enter, Escape, and you can type a code directly.

**Input delay** is on the home screen. Start at 2. Raise it if the match
stutters.

## Watching a match

A lobby also has two spectator seats. On the **Join with code** screen, type
the code and pick **WATCH** instead of **JOIN**. You see the two players and
who else is watching; when they both press READY the match starts on your
screen too, seen from player 1's side and a moment behind them. After the
match, choose **KEEP WATCHING** for the next one or leave.

Join before the players ready up: the match is streamed from its first frame,
so anyone who joins while it is running watches the next one instead.

## If something goes wrong

* **"SERVER UNREACHABLE"** on the home screen: the lobby server is down, or a
  firewall is blocking UDP. Tell whoever gave you the mod.
* **"the other player runs a different sf4e build"**: you have different
  versions. Both of you should use the same zip.
* Logs are written to `%APPDATA%\sf4e\logs\`. If the game closed by itself,
  there is also a crash report in `%APPDATA%\sf4e\crash\`. Send both with a
  description of what you were doing, the characters, and who created the lobby.

Steam's own online modes are disabled while the mod is running.

## Linux and Steam Deck

Install [protontricks](https://github.com/Matoking/protontricks), then run:

```
protontricks-launch --appid 45760 SF4Enhanced.exe
```

## Credits

Built on [sf4e](https://codeberg.org/adanducci/sf4e) by Anthony Danducci, which
does the hard part: the reverse-engineered engine, the save states and the GGPO
integration.

Street Fighter and Ultra Street Fighter IV are copyright CAPCOM.
