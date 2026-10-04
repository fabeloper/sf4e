// sf4e lobby server: hands out lobby codes, runs one session server per
// lobby, and relays GGPO traffic between the two players so that neither of
// them needs a reachable address or a forwarded port.
//
// Everything is UDP. A client asks the matchmaker port for a code (create) or
// looks one up (join), then connects to that lobby's session port with the
// normal sf4e session client. When the match starts, both games send their
// GGPO packets to the lobby's relay port and this program forwards each one
// to the other player.
//
// Spectators get a relay "pipe" each: two ports, one that P1 sends to and one
// that the spectator sends to. Whatever arrives on one side goes out to the
// last address seen on the other, so neither side has to be recognised by
// its address, which a symmetric NAT would defeat.

// Winsock on Windows, POSIX sockets elsewhere. See net_compat.hxx.
#include "net_compat.hxx"

#include <chrono>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "../session/sf4e__SessionServer.hxx"

using nlohmann::json;
using sf4e::SessionServer;

namespace {
	const int NUM_LOBBIES = 20;
	const uint16_t MATCHMAKER_PORT = 23400;
	const uint16_t FIRST_SESSION_PORT = 23401;   // 23401 .. 23420
	const uint16_t FIRST_RELAY_PORT = 24001;     // 24001 .. 24020
	const int MAX_PLAYERS_PER_LOBBY = 2;
	const int MAX_SPECTATORS_PER_LOBBY = MAX_SF4E_SPECTATORS;
	// Two ports per spectator pipe, two pipes per lobby: 25001 .. 25080.
	const uint16_t FIRST_SPECTATOR_PORT = 25001;
	const int SPECTATOR_PORTS_PER_LOBBY = MAX_SPECTATORS_PER_LOBBY * 2;
	const ULONGLONG EMPTY_LOBBY_TIMEOUT_MS = 90 * 1000;
	const ULONGLONG RELAY_ENDPOINT_TIMEOUT_MS = 20 * 1000;
	const ULONGLONG MAX_LOBBY_AGE_MS = 12ULL * 3600 * 1000;
	const int MAX_LOBBIES_PER_IP = 2;
	// Matchmaker datagrams per source address: a burst of this many, then
	// this many per second. Anything over is dropped without a reply.
	const double MATCHMAKER_BURST = 20;
	const double MATCHMAKER_PER_SECOND = 5;

	// No 0/O or 1/I/L, so a code read aloud or typed from a screenshot is
	// never ambiguous.
	const char CODE_ALPHABET[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
	const int CODE_LENGTH = 6;

	volatile bool g_running = true;

#ifdef _WIN32
	BOOL WINAPI OnConsoleCtrl(DWORD) {
		g_running = false;
		return TRUE;
	}
#else
	// Same job as the console handler: drop out of the main loop so the
	// shutdown path runs. SIGTERM matters as much as SIGINT here, because that
	// is what systemd sends when the service is stopped or the box reboots.
	void OnSignal(int) {
		g_running = false;
	}
#endif

	SOCKET OpenUdp(uint16_t port) {
		SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == INVALID_SOCKET) {
			return INVALID_SOCKET;
		}
		sockaddr_in addr = { 0 };
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(port);
		if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
			closesocket(s);
			return INVALID_SOCKET;
		}
		u_long nonBlocking = 1;
		ioctlsocket(s, FIONBIO, &nonBlocking);
		return s;
	}

	bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b) {
		return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
	}

	std::string Describe(const sockaddr_in& a) {
		char ip[INET_ADDRSTRLEN] = { 0 };
		inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
		return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
	}

	// Forwards packets between the first two endpoints that talk to it. Both
	// games are told the other player lives at this port, so GGPO on each side
	// sees the relay as its peer and never learns the other's real address.
	// A short, fixed probe a client sends from its GGPO socket to ask "what
	// address do you see me at?". Nine bytes, compared before anything else, so
	// a real match packet can never be mistaken for one.
	static const char STUN_PROBE[] = "SF4ESTUN1";
	static const size_t STUN_PROBE_LEN = sizeof(STUN_PROBE) - 1;

	struct Relay {
		SOCKET sock = INVALID_SOCKET;
		uint16_t port = 0;
		struct Endpoint {
			bool used = false;
			sockaddr_in addr = { 0 };
			ULONGLONG lastSeen = 0;
		};
		Endpoint eps[MAX_PLAYERS_PER_LOBBY];
		uint64_t packets = 0;

		// The two players' addresses in seat order (host byte order, 0 =
		// empty). Slot i belongs to seat i; a seat change drops its slot.
		std::vector<uint32_t> players = std::vector<uint32_t>(MAX_PLAYERS_PER_LOBBY, 0);
		uint64_t rejected = 0;

		bool IsAllowed(const sockaddr_in& from) const {
			uint32_t ip = ntohl(from.sin_addr.s_addr);
			for (size_t i = 0; i < players.size(); i++) {
				if (ip != 0 && players[i] == ip) return true;
			}
			return false;
		}

		void SetPlayers(const std::vector<uint32_t>& seats) {
			for (int i = 0; i < MAX_PLAYERS_PER_LOBBY; i++) {
				uint32_t ip = (size_t)i < seats.size() ? seats[i] : 0;
				if (players[i] != ip) {
					players[i] = ip;
					eps[i].used = false;
				}
			}
		}

		bool Open(uint16_t p) {
			port = p;
			sock = OpenUdp(p);
			return sock != INVALID_SOCKET;
		}

		void Clear() {
			for (int i = 0; i < MAX_PLAYERS_PER_LOBBY; i++) {
				eps[i].used = false;
			}
		}

		void Pump(ULONGLONG now) {
			char buf[2048];
			for (;;) {
				sockaddr_in from = { 0 };
				socklen_t fromLen = sizeof(from);   // POSIX wants socklen_t; Winsock defines it too
				int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
				if (n <= 0) {
					break;
				}

				// An address query, answered here rather than on the matchmaker
				// port.
				//
				// A player reported that peer to peer never started: his query
				// to the matchmaker never reached the server at all, while his
				// ordinary matchmaking AND his match traffic to this relay port
				// both worked from the same machine. Something between him and
				// us drops that one combination. The relay port is a path his
				// network demonstrably allows -- it has to, or he could not
				// play -- so ask the question where the answer can get back.
				//
				// Checked before a slot is taken so a query never registers as
				// a player, and it is never forwarded to the opponent.
				if (n == (int)STUN_PROBE_LEN && memcmp(buf, STUN_PROBE, STUN_PROBE_LEN) == 0) {
					if (!IsAllowed(from)) {
						// Always logged, never rate limited. These are rare -- a
						// handful per player per lobby -- and sharing the scanner
						// counter meant a refusal could be silently skipped, which
						// is indistinguishable from the packet never arriving. That
						// ambiguity is exactly what we are trying to resolve.
						spdlog::debug("relay :{} address query from {} REFUSED (not a member of this lobby)", port, Describe(from));
						continue;
					}
					char ip[INET_ADDRSTRLEN] = { 0 };
					inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
					std::string out = json{
						{"ok", true}, {"ip", ip}, {"port", ntohs(from.sin_port)}
					}.dump();
					sendto(sock, out.c_str(), (int)out.size(), 0, (sockaddr*)&from, sizeof(from));
					spdlog::debug("relay :{} answered an address query from {}", port, Describe(from));
					continue;
				}

				int idx = -1;
				for (int i = 0; i < MAX_PLAYERS_PER_LOBBY; i++) {
					if (eps[i].used && SameEndpoint(eps[i].addr, from)) {
						idx = i;
						break;
					}
				}
				if (idx < 0) {
					// Only someone who actually joined this lobby may take a
					// slot. Rate-limited logging: a scan would otherwise fill
					// the log faster than anything useful in it.
					if (!IsAllowed(from)) {
						if ((rejected++ % 500) == 0) {
							spdlog::warn("relay :{} rejected {} (not a member of this lobby)", port, Describe(from));
						}
						continue;
					}
					// The seat whose address this is; two players behind one
					// address take the seats in order.
					uint32_t ip = ntohl(from.sin_addr.s_addr);
					for (int i = 0; i < MAX_PLAYERS_PER_LOBBY; i++) {
						if (!eps[i].used && players[i] == ip) {
							eps[i].used = true;
							eps[i].addr = from;
							idx = i;
							spdlog::debug("relay :{} learned player {} at {}", port, i + 1, Describe(from));
							break;
						}
					}
				}
				if (idx < 0) {
					// A third sender on the players' port. Spectators have
					// their own pipes; anything else is noise.
					continue;
				}
				eps[idx].lastSeen = now;

				int other = 1 - idx;
				if (eps[other].used) {
					sendto(sock, buf, n, 0, (sockaddr*)&eps[other].addr, sizeof(eps[other].addr));
					packets++;
				}
			}

			for (int i = 0; i < MAX_PLAYERS_PER_LOBBY; i++) {
				if (eps[i].used && now - eps[i].lastSeen > RELAY_ENDPOINT_TIMEOUT_MS) {
					spdlog::info("relay :{} player {} timed out", port, i + 1);
					eps[i].used = false;
				}
			}
		}
	};

	// One spectator's pipe. P1 talks to `hostSide`, the spectator to
	// `specSide`; each side's packets go to the last address seen on the
	// other side.
	struct Pipe {
		SOCKET hostSock = INVALID_SOCKET;
		SOCKET specSock = INVALID_SOCKET;
		uint16_t hostPort = 0;
		uint16_t specPort = 0;
		Relay::Endpoint host, spec;
		uint64_t packets = 0;
		// Same membership check as the relay; see Relay::allowed.
		std::vector<uint32_t> allowed;
		uint64_t rejected = 0;

		void SetAllowed(const std::vector<uint32_t>& members) {
			if (members != allowed) {
				allowed = members;
				Clear();
			}
		}

		bool IsAllowed(const sockaddr_in& from) const {
			uint32_t ip = ntohl(from.sin_addr.s_addr);
			for (size_t i = 0; i < allowed.size(); i++) {
				if (allowed[i] == ip) return true;
			}
			return false;
		}

		bool Open(uint16_t hostSide, uint16_t specSide) {
			hostPort = hostSide;
			specPort = specSide;
			hostSock = OpenUdp(hostSide);
			specSock = OpenUdp(specSide);
			return hostSock != INVALID_SOCKET && specSock != INVALID_SOCKET;
		}

		void Clear() {
			host.used = false;
			spec.used = false;
		}

		void PumpSide(SOCKET in, Relay::Endpoint& sender, Relay::Endpoint& receiver, const char* who, uint16_t port, ULONGLONG now) {
			char buf[2048];
			for (;;) {
				sockaddr_in from = { 0 };
				socklen_t fromLen = sizeof(from);   // POSIX wants socklen_t; Winsock defines it too
				int n = recvfrom(in, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
				if (n <= 0) {
					break;
				}
				if (!sender.used || !SameEndpoint(sender.addr, from)) {
					// This side re-learns on ANY new address, so without a check
					// a single packet from a stranger would redirect a
					// spectator feed mid-stream. Only lobby members qualify.
					if (!IsAllowed(from)) {
						if ((rejected++ % 500) == 0) {
							spdlog::warn("pipe :{} rejected {} (not a member of this lobby)", port, Describe(from));
						}
						continue;
					}
					sender.used = true;
					sender.addr = from;
					spdlog::debug("pipe :{} learned {} at {}", port, who, Describe(from));
				}
				sender.lastSeen = now;
				if (receiver.used) {
					// Reply out of the receiver's own side, so its NAT sees
					// the port it already talked to.
					SOCKET out = (&receiver == &host) ? hostSock : specSock;
					sendto(out, buf, n, 0, (sockaddr*)&receiver.addr, sizeof(receiver.addr));
					packets++;
				}
			}
		}

		void Pump(ULONGLONG now) {
			PumpSide(hostSock, host, spec, "P1", hostPort, now);
			PumpSide(specSock, spec, host, "spectator", specPort, now);
			if (host.used && now - host.lastSeen > RELAY_ENDPOINT_TIMEOUT_MS) {
				spdlog::info("pipe :{} P1 timed out", hostPort);
				host.used = false;
			}
			if (spec.used && now - spec.lastSeen > RELAY_ENDPOINT_TIMEOUT_MS) {
				spdlog::info("pipe :{} spectator timed out", specPort);
				spec.used = false;
			}
		}
	};

	struct Lobby {
		int index = 0;
		bool active = false;
		std::string code;
		std::string hash;
		// Listed to anyone who asks when public. The title is all a stranger
		// sees; the code lets them join, as if a friend had given it to them.
		// Never the creator address.
		bool isPublic = false;
		std::string title;
		uint16_t sessionPort = 0;
		std::string secret;
		uint32_t creatorIp = 0;
		ULONGLONG createdAt = 0;
		std::unique_ptr<SessionServer> server;
		Relay relay;
		Pipe pipes[MAX_SPECTATORS_PER_LOBBY];
		ULONGLONG lastNonEmpty = 0;

		int PlayerCount() const {
			return server ? server->PlayerCount() : 0;
		}
		int SpectatorCount() const {
			return server ? server->SpectatorCount() : 0;
		}
		int MemberCount() const {
			return server ? (int)server->clients.size() : 0;
		}
	};

	std::vector<Lobby> g_lobbies;
	std::random_device g_rng;

	std::string NewSecret() {
		static const char HEX[] = "0123456789abcdef";
		std::string s;
		for (int i = 0; i < 32; i++) {
			s += HEX[g_rng() % 16];
		}
		return s;
	}

	std::string NewCode() {
		for (;;) {
			std::string code;
			for (int i = 0; i < CODE_LENGTH; i++) {
				code += CODE_ALPHABET[g_rng() % (sizeof(CODE_ALPHABET) - 1)];
			}
			bool taken = false;
			for (auto& l : g_lobbies) {
				if (l.active && l.code == code) {
					taken = true;
				}
			}
			if (!taken) {
				return code;
			}
		}
	}

	struct Bucket {
		double tokens = MATCHMAKER_BURST;
		ULONGLONG last = 0;
	};
	std::map<uint32_t, Bucket> g_buckets;

	bool AllowMatchmaker(const sockaddr_in& from, ULONGLONG now) {
		if (g_buckets.size() > 50000) {
			g_buckets.clear();
		}
		Bucket& b = g_buckets[ntohl(from.sin_addr.s_addr)];
		if (b.last != 0) {
			b.tokens += (now - b.last) * MATCHMAKER_PER_SECOND / 1000.0;
			if (b.tokens > MATCHMAKER_BURST) b.tokens = MATCHMAKER_BURST;
		}
		b.last = now;
		if (b.tokens < 1) {
			return false;
		}
		b.tokens -= 1;
		return true;
	}

	Lobby* FindByCode(const std::string& code) {
		for (auto& l : g_lobbies) {
			if (l.active && l.code == code) {
				return &l;
			}
		}
		return nullptr;
	}

	void ClearRelays(Lobby& l) {
		l.relay.Clear();
		for (int k = 0; k < MAX_SPECTATORS_PER_LOBBY; k++) {
			l.pipes[k].Clear();
		}
	}

	void ResetLobby(Lobby& l, ULONGLONG now) {
		if (l.active) {
			SessionServer::LogStat("lobby_released", { {"lobby", l.index} });
			spdlog::info("lobby {} ({}) released", l.index, l.code);
		}
		l.active = false;
		l.code.clear();
		l.hash.clear();
		l.secret.clear();
		l.creatorIp = 0;
		l.isPublic = false;
		l.title.clear();
		l.lastNonEmpty = now;
		ClearRelays(l);
		l.server->SetSidecarHash("");
		l.server->SetJoinSecret("");
		l.server->SetPublic(false);
		l.server->DisconnectAll();
		l.server->ResetLobby();
	}

	// Who is around right now: players connected to a lobby, and addresses that
	// pinged or listed in the last minute. Kept in memory only, as a count.
	std::map<uint32_t, ULONGLONG> g_recentBrowsers;
	int CountBrowsing(uint32_t ip, ULONGLONG now) {
		g_recentBrowsers[ip] = now;
		for (auto it = g_recentBrowsers.begin(); it != g_recentBrowsers.end();) {
			if (now - it->second > 60000) it = g_recentBrowsers.erase(it);
			else ++it;
		}
		return (int)g_recentBrowsers.size();
	}
	int CountPlayers() {
		int n = 0;
		for (auto& l : g_lobbies) {
			if (l.active) n += l.MemberCount();
		}
		return n;
	}

	json HandleMatchmaker(const json& req, ULONGLONG now, const sockaddr_in& from) {
		// STUN: tell the caller what address this datagram came from. A client
		// asks this from the very socket GGPO will use, so the answer is the
		// mapping its NAT will present to the other player. Nothing is stored.
		if (req.value("type", std::string()) == "stun") {
			char ip[INET_ADDRSTRLEN] = { 0 };
			inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
			// Logged because a player reported "the server did not answer"
			// while their ordinary matchmaking to this same port worked. Only
			// the server can say whether the request ever arrived; without this
			// line the two possibilities are indistinguishable.
			spdlog::debug("stun request from {}:{}", ip, ntohs(from.sin_port));
			return { {"ok", true}, {"ip", ip}, {"port", ntohs(from.sin_port)} };
		}
		// Printable ASCII only, bounded: shown to strangers and logged.
		auto cleanText = [](std::string s, size_t cap) {
			std::string out;
			for (unsigned char c : s) {
				if (c >= 0x20 && c <= 0x7e && out.size() < cap) out.push_back((char)c);
			}
			return out;
		};
		std::string op = req.value("op", "");
		if (op == "ping") {
			int active = 0;
			for (auto& l : g_lobbies) {
				if (l.active) {
					active++;
				}
			}
			return { {"ok", true}, {"lobbies", active}, {"capacity", NUM_LOBBIES}, {"version", SF4E_VERSION},
				{"players", CountPlayers()}, {"browsing", CountBrowsing(ntohl(from.sin_addr.s_addr), now)} };
		}
		if (op == "create") {
			std::string hash = req.value("hash", "");
			if (hash.empty()) {
				return { {"ok", false}, {"error", "bad_request"} };
			}
			bool isPublic = req.value("public", false);
			std::string title = cleanText(req.value("title", ""), 24);
			if (title.empty()) {
				std::string who = cleanText(req.value("name", ""), 16);
				title = who.empty() ? "open lobby" : "lobby by " + who;
			}
			uint32_t ip = ntohl(from.sin_addr.s_addr);
			// A lobby this address created and then left is still waiting out
			// its empty timeout. Release it now rather than count it against
			// the cap: two PCs behind one router hit it on their second try.
			int mine = 0;
			for (auto& l : g_lobbies) {
				if (!l.active || l.creatorIp != ip) continue;
				if (l.MemberCount() == 0) ResetLobby(l, now);
				else mine++;
			}
			if (mine >= MAX_LOBBIES_PER_IP) {
				return { {"ok", false}, {"error", "server_full"} };
			}
			for (auto& l : g_lobbies) {
				if (l.active) {
					continue;
				}
				l.active = true;
				l.code = NewCode();
				l.secret = NewSecret();
				l.hash = hash;
				l.creatorIp = ip;
				l.createdAt = now;
				l.isPublic = isPublic;
				l.title = title;
				l.lastNonEmpty = now;
				ClearRelays(l);
				l.server->DisconnectAll();
				l.server->SetSidecarHash(l.hash);
				l.server->SetJoinSecret(l.secret);
				l.server->ResetLobby();
				l.server->SetPublic(isPublic);
				SessionServer::LogStat("lobby_created", { {"lobby", l.index} });
				spdlog::info("lobby {} created ({}): code {} session :{} relay :{} by {}",
					l.index, isPublic ? "public" : "private", l.code, l.sessionPort, l.relay.port, req.value("name", "?"));
				return { {"ok", true}, {"code", l.code}, {"session_port", l.sessionPort}, {"secret", l.secret} };
			}
			return { {"ok", false}, {"error", "server_full"} };
		}
		if (op == "list") {
			// Open public lobbies. Codes are the join handle a friend would have
			// typed; nothing about the creator leaves the server. Full lobbies
			// drop off on their own.
			json list = json::array();
			for (auto& l : g_lobbies) {
				if (!l.active || !l.isPublic || l.MemberCount() == 0) continue;
				list.push_back({
					{"code", l.code}, {"title", l.title},
					{"players", l.PlayerCount()}, {"spectators", l.SpectatorCount()},
					{"full", l.PlayerCount() >= MAX_PLAYERS_PER_LOBBY}, {"spectators_max", MAX_SPECTATORS_PER_LOBBY},
					{"age", (int)((now - l.createdAt) / 1000)},
				});
				if (list.size() >= 20) break;
			}
			return { {"ok", true}, {"list", list},
				{"players", CountPlayers()}, {"browsing", CountBrowsing(ntohl(from.sin_addr.s_addr), now)} };
		}
		if (op == "join") {
			std::string code = req.value("code", "");
			for (auto& c : code) {
				c = (char)toupper((unsigned char)c);
			}
			bool spectate = req.value("spectate", false);
			Lobby* l = FindByCode(code);
			if (!l) {
				return { {"ok", false}, {"error", "not_found"} };
			}
			if (!spectate && l->PlayerCount() >= MAX_PLAYERS_PER_LOBBY) {
				return { {"ok", false}, {"error", "lobby_full"} };
			}
			if (spectate && l->SpectatorCount() >= MAX_SPECTATORS_PER_LOBBY) {
				return { {"ok", false}, {"error", "spectators_full"} };
			}
			std::string hash = req.value("hash", "");
			if (hash.empty() || hash != l->hash) {
				return { {"ok", false}, {"error", "version_mismatch"} };
			}
			spdlog::info("lobby {} ({}) {} lookup by {}", l->index, l->code, spectate ? "spectate" : "join", req.value("name", "?"));
			return { {"ok", true}, {"code", l->code}, {"session_port", l->sessionPort}, {"secret", l->secret} };
		}
		return { {"ok", false}, {"error", "bad_request"} };
	}
	// One JSON datagram in, one out.
	void ServeMatchmaker(SOCKET matchmaker, ULONGLONG now) {
		for (;;) {
			char buf[1500];
			sockaddr_in from = { 0 };
			socklen_t fromLen = sizeof(from);   // POSIX wants socklen_t; Winsock defines it too
			int n = recvfrom(matchmaker, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
			if (n <= 0) {
				break;
			}
			buf[n] = 0;
			if (!AllowMatchmaker(from, now)) {
				continue;
			}
			json reply;
			try {
				reply = HandleMatchmaker(json::parse(buf), now, from);
			}
			catch (const std::exception&) {
				reply = { {"ok", false}, {"error", "bad_request"} };
			}
			std::string out = reply.dump();
			sendto(matchmaker, out.c_str(), (int)out.size(), 0, (sockaddr*)&from, sizeof(from));
		}
	}

	// The sockets whose packets are answered or forwarded the moment they
	// arrive, instead of on the next loop tick: match traffic and pings.
	struct WakeSockets {
		struct Owner {
			Lobby* lobby;   // null: the matchmaker
			int pipe;       // negative: the players' relay
		};
		std::vector<PollEntry> entries;
		std::vector<Owner> owners;

		void Add(SOCKET sock, Lobby* lobby, int pipe) {
			PollEntry entry = { 0 };
			entry.fd = sock;
			entry.events = POLL_READABLE;
			entries.push_back(entry);
			owners.push_back({ lobby, pipe });
		}

		void AddMatchmaker(SOCKET matchmaker) {
			Add(matchmaker, nullptr, 0);
		}

		void AddLobby(Lobby& lobby) {
			Add(lobby.relay.sock, &lobby, -1);
			for (int k = 0; k < MAX_SPECTATORS_PER_LOBBY; k++) {
				Add(lobby.pipes[k].hostSock, &lobby, k);
				Add(lobby.pipes[k].specSock, &lobby, k);
			}
		}

		void ServeUntil(int timeoutMs) {
			int ready = PollSockets(entries.data(), (unsigned long)entries.size(), timeoutMs);
			if (ready < 0) {
				Sleep(timeoutMs);
			}
			if (ready <= 0) {
				return;
			}
			ULONGLONG now = GetTickCount64();
			for (size_t i = 0; i < entries.size(); i++) {
				if (!(entries[i].revents & POLL_READABLE)) {
					continue;
				}
				const Owner& owner = owners[i];
				if (owner.lobby == nullptr) {
					ServeMatchmaker(entries[i].fd, now);
				}
				else if (owner.pipe < 0) {
					owner.lobby->relay.Pump(now);
				}
				else {
					owner.lobby->pipes[owner.pipe].Pump(now);
				}
			}
		}
	};

	int MillisecondsUntil(std::chrono::steady_clock::time_point deadline) {
		auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		return remaining <= 0 ? 0 : (int)((remaining + 999) / 1000);
	}
}

int main(int argc, char** argv) {
	spdlog::set_default_logger(spdlog::stdout_color_mt("lobby"));
	spdlog::set_pattern("[%H:%M:%S] %^%l%$ %v");
#ifdef _WIN32
	SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#else
	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);
	// A client that vanishes can leave us writing to a dead socket, and the
	// default action for SIGPIPE is to kill the process. Ignore it and handle
	// the error at the call site like any other send failure.
	signal(SIGPIPE, SIG_IGN);
#endif

	SteamDatagramErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
		spdlog::critical("GameNetworkingSockets_Init failed: {}", errMsg);
		return 1;
	}

	SOCKET matchmaker = OpenUdp(MATCHMAKER_PORT);
	if (matchmaker == INVALID_SOCKET) {
		spdlog::critical("could not bind matchmaker port {} (already in use?)", MATCHMAKER_PORT);
		return 1;
	}

	Dimps::Math::FixedPoint roundTime = { 0, 99 };
	g_lobbies.resize(NUM_LOBBIES);
	for (int i = 0; i < NUM_LOBBIES; i++) {
		Lobby& l = g_lobbies[i];
		l.index = i;
		l.sessionPort = FIRST_SESSION_PORT + i;
		uint16_t specBase = FIRST_SPECTATOR_PORT + i * SPECTATOR_PORTS_PER_LOBBY;
		l.server.reset(new SessionServer("sf4e-lobby-" + std::to_string(i), "", true, 3, roundTime));
		l.server->SetRelayPort(FIRST_RELAY_PORT + i);
		l.server->SetSpectatorRelayPorts(specBase, MAX_SPECTATORS_PER_LOBBY);
		l.server->RequireJoinSecret(true);
		if (l.server->Listen(l.sessionPort) != 0) {
			spdlog::critical("could not listen on session port {}", l.sessionPort);
			return 1;
		}
		if (!l.relay.Open(FIRST_RELAY_PORT + i)) {
			spdlog::critical("could not bind relay port {}", FIRST_RELAY_PORT + i);
			return 1;
		}
		for (int k = 0; k < MAX_SPECTATORS_PER_LOBBY; k++) {
			if (!l.pipes[k].Open(specBase + 2 * k, specBase + 2 * k + 1)) {
				spdlog::critical("could not bind spectator ports {}-{}", specBase + 2 * k, specBase + 2 * k + 1);
				return 1;
			}
		}
	}

	spdlog::info("sf4e lobby server up: matchmaker udp/{}, sessions udp/{}-{}, relays udp/{}-{}, spectator pipes udp/{}-{}, {} lobbies",
		MATCHMAKER_PORT,
		FIRST_SESSION_PORT, FIRST_SESSION_PORT + NUM_LOBBIES - 1,
		FIRST_RELAY_PORT, FIRST_RELAY_PORT + NUM_LOBBIES - 1,
		FIRST_SPECTATOR_PORT, FIRST_SPECTATOR_PORT + NUM_LOBBIES * SPECTATOR_PORTS_PER_LOBBY - 1,
		NUM_LOBBIES);

	WakeSockets wake;
	wake.AddMatchmaker(matchmaker);
	for (auto& l : g_lobbies) {
		wake.AddLobby(l);
	}
	const auto housekeepingInterval = std::chrono::milliseconds(2);
	auto nextHousekeeping = std::chrono::steady_clock::now();

	while (g_running) {
		wake.ServeUntil(MillisecondsUntil(nextHousekeeping));
		if (MillisecondsUntil(nextHousekeeping) > 0) {
			continue;
		}
		nextHousekeeping = std::chrono::steady_clock::now() + housekeepingInterval;

		ULONGLONG now = GetTickCount64();

		ServeMatchmaker(matchmaker, now);

		SteamNetworkingSockets()->RunCallbacks();

		for (auto& l : g_lobbies) {
			l.server->PrepareForCallbacks();
			l.server->Step();

			// Refresh who is allowed to use this lobby's relay before pumping
			// it. Taken from the session members, which is the only place we
			// actually know a player is who they say they are: they had to look
			// the lobby code up and complete a session handshake to get here.
			std::vector<uint32_t> members = l.server->MemberIPv4s();
			l.relay.SetPlayers(l.server->PlayerIPv4s());
			for (int k = 0; k < MAX_SPECTATORS_PER_LOBBY; k++) {
				l.pipes[k].SetAllowed(members);
			}

			l.relay.Pump(now);
			for (int k = 0; k < MAX_SPECTATORS_PER_LOBBY; k++) {
				l.pipes[k].Pump(now);
			}

			if (l.active) {
				if (l.MemberCount() > 0) {
					l.lastNonEmpty = now;
				}
				else if (now - l.lastNonEmpty > EMPTY_LOBBY_TIMEOUT_MS) {
					ResetLobby(l, now);
				}
				if (l.active && now - l.createdAt > MAX_LOBBY_AGE_MS) {
					ResetLobby(l, now);
				}
			}
		}
	}

	spdlog::info("shutting down");
	for (auto& l : g_lobbies) {
		l.server->Close();
	}
	closesocket(matchmaker);
	GameNetworkingSockets_Kill();
	WSACleanup();
	return 0;
}
