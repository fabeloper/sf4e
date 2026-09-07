# Running the sf4e lobby server

Players never share IP addresses or forward ports. Both games connect *out* to
this server: it hands out six-character lobby codes, runs the pre-match lobby,
and relays the rollback traffic between the two players. One small Windows VPS
runs it.

## What you need

* A Windows Server VPS. Any provider works; 1 vCPU and 1 GB of RAM is plenty.
  Pick a region roughly between your players, since every packet takes one hop
  through it.
* Its public IP address.
* In the provider's firewall (often called a security group), allow inbound
  **UDP 23400-23420** and **UDP 24001-24020**.

## Installing

1. Copy the `sf4e-server` folder to the VPS, for example onto the Desktop.
2. Right-click `run-server.cmd` and choose *Run as administrator*.

That opens Windows Firewall for the ports above and starts the server in a
console window. Leave the window open. If the server ever exits, the script
restarts it after five seconds.

You should see a line like:

```
sf4e lobby server up: matchmaker udp/23400, sessions udp/23401-23420, relays udp/24001-24020, 20 lobbies
```

To keep it running after you disconnect from Remote Desktop, sign out instead
of closing the window, or run it as a scheduled task that starts at boot.

## Pointing players at it

In the player package, edit `server.txt` and put the VPS's IP on a line by
itself:

```
203.0.113.5
```

Zip the folder and send it. Players extract, double-click `Launcher.exe`, and
see *Create lobby* and *Join with code*.

## Running it at home

A spare Windows PC or laptop on your home network works just as well as a
VPS, with one difference: your router has to send the server's ports to it.

1. Plug the laptop into mains power and, ideally, into the router with a
   cable. Sign in to Windows.
2. Copy the `sf4e-server` folder onto it, anywhere, and run
   `setup-laptop.cmd` in it. It asks for administrator rights, then opens
   the firewall, stops the machine sleeping, registers the server to start
   at every sign-in, and starts it. It prints the machine's IP address at
   the end; note it.
3. Give the laptop a fixed address so the router can't move it: in your
   router's app or web page, find *Address Reservation* (or *DHCP
   reservation*) and reserve the address it printed.
4. Send the ports to it. Either run `upnp-map.cmd` on the laptop, which asks
   the router to do it automatically, or forward **UDP 23400-23420** and
   **UDP 24001-24020** to the laptop's address by hand in the router. If you
   have two routers in a row (a provider box and your own), the provider's
   box must forward the ports, or DMZ, to your router, and your router to
   the laptop.
5. Check it from another machine: `test-server.cmd LAPTOP-IP`, then
   `test-server.cmd YOUR-PUBLIC-IP`. Both should print `"ok":true`.
6. For a server that survives reboots with nobody around, turn on automatic
   sign-in: `Win+R`, `netplwiz`, untick "Users must enter a user name and
   password".

Players keep using your public IP in `server.txt`. It changes when your
provider decides; a free dynamic-DNS name, set up in the router, gives them
an address that never changes.

## Capacity and limits

* 20 lobbies at once. Change `NUM_LOBBIES` in `src/server/lobby_server.cxx` to
  raise it; the port ranges grow with it.
* Two players per lobby. Spectators are not relayed yet.
* Empty lobbies are released after 90 seconds.
* Both players must run the same sf4e build, checked automatically.

## Checking it from your PC

From any machine, this prints how many lobbies are in use:

```powershell
$u = New-Object System.Net.Sockets.UdpClient
$b = [Text.Encoding]::ASCII.GetBytes('{"op":"ping"}')
$u.Send($b, $b.Length, 'YOUR.VPS.IP', 23400) | Out-Null
$ep = New-Object System.Net.IPEndPoint([Net.IPAddress]::Any, 0)
[Text.Encoding]::ASCII.GetString($u.Receive([ref]$ep))
```
