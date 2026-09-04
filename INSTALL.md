# Playing Ultra Street Fighter IV with rollback

This is a mod. It does not change any game file. It starts the game and adds
netcode to it while it runs, so you can remove it by deleting this folder.

## What you need

* Ultra Street Fighter IV on Steam, installed and run at least once.
* Steam running and signed in.
* Windows 10 or later. On Linux and Steam Deck, see the bottom of this file.

## Installing

1. Extract this whole folder anywhere you like. The Desktop is fine. Do **not**
   copy the files into the Street Fighter folder.
2. Double-click `Launcher.exe`.

That is the entire installation. The launcher finds your Steam copy of the game
by itself and starts it.

If it cannot find the game, set the `STEAM_APP_PATH` environment variable to
your `Super Street Fighter IV - Arcade Edition` folder. You can open that folder
from Steam: right-click the game, Manage, Browse local files.

## Playing someone else

Steam's own online modes are disabled while the mod is running. Use the mod's
menu instead: from the main menu choose **Multiplayer Battle**, or move your
mouse to the top of the screen and pick **Network**.

One player hosts and the other joins.

**Host:** open the Network window, choose *Host a game*, type a name, and press
*Start hosting*. Tell the other player your public IP address, which you can
find by searching "what is my IP".

**Joiner:** choose *Join a game*, type a name, and enter the host's address as
`ip:port`, for example `203.0.113.5:23456`. Press *Join*.

Both players then pick a character and press *Send chara*. The match starts once
both are ready.

**Delay** is how many frames of input delay you add. Start at `1` or `2`. Higher
delay means less rollback and smoother animation, but less responsive controls.

### Port forwarding

Right now the two games talk to each other directly, so the connection needs to
get through your routers. Until the lobby server exists, the host has to forward
two ports in their router settings:

| Port    | Protocol | Who forwards it |
| ------- | -------- | --------------- |
| `23456` | UDP      | Host only       |
| `23457` | UDP      | Both players    |

Both ports are configurable in the Network window if those numbers are already
taken on your network.

If you have never forwarded a port, search for "port forwarding" plus your
router's model. The two of you being on the same local network, or both on a
VPN or Hamachi-style virtual network, avoids this entirely and is the easiest
way to test.

## If something goes wrong

Logs are written to `%APPDATA%\sf4e\logs\`. Paste `sf4e.log` into a bug report.
Include which characters were being played, whether you were host or joiner, and
what you each were using to play, such as a pad, a stick, or the keyboard.

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
