// sf4e lobby server: hands out lobby codes, runs one session server per
// lobby, and relays GGPO traffic between the two players so that neither of
// them needs a reachable address or a forwarded port.
//
// Everything is UDP. A client asks the matchmaker port for a code (create) or
// looks one up (join), then connects to that lobby's session port with the
// normal sf4e session client. When the match starts, both games send their
// GGPO packets to the lobby's relay port and this program forwards each one
// to the other player.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
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
	const int MAX_PLAYERS_PER_LOBBY = 2;         // spectators are not relayed yet
	const ULONGLONG EMPTY_LOBBY_TIMEOUT_MS = 90 * 1000;
	const ULONGLONG RELAY_ENDPOINT_TIMEOUT_MS = 20 * 1000;

	// No 0/O or 1/I/L, so a code read aloud or typed from a screenshot is
	// never ambiguous.
	const char CODE_ALPHABET[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
	const int CODE_LENGTH = 6;

	volatile bool g_running = true;

	BOOL WINAPI OnConsoleCtrl(DWORD) {
		g_running = false;
		return TRUE;
	}

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
				int fromLen = sizeof(from);
				int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
				if (n <= 0) {
					break;
				}

				int idx = -1;
				for (int i = 0; i < MAX_PLAYERS_PER_LOBBY; i++) {
					if (eps[i].used && SameEndpoint(eps[i].addr, from)) {
						idx = i;
						break;
					}
				}
				if (idx < 0) {
					for (int i = 0; i < MAX_PLAYERS_PER_LOBBY; i++) {
						if (!eps[i].used) {
							eps[i].used = true;
							eps[i].addr = from;
							idx = i;
							spdlog::info("relay :{} learned player {} at {}", port, i + 1, Describe(from));
							break;
						}
					}
				}
				if (idx < 0) {
					// A third sender. Spectators are not relayed yet.
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

	struct Lobby {
		int index = 0;
		bool active = false;
		std::string code;
		std::string hash;
		uint16_t sessionPort = 0;
		std::unique_ptr<SessionServer> server;
		Relay relay;
		ULONGLONG lastNonEmpty = 0;

		int PlayerCount() const {
			return server ? (int)server->clients.size() : 0;
		}
	};

	std::vector<Lobby> g_lobbies;
	std::mt19937 g_rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());

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

	Lobby* FindByCode(const std::string& code) {
		for (auto& l : g_lobbies) {
			if (l.active && l.code == code) {
				return &l;
			}
		}
		return nullptr;
	}

	void ResetLobby(Lobby& l, ULONGLONG now) {
		if (l.active) {
			spdlog::info("lobby {} ({}) released", l.index, l.code);
		}
		l.active = false;
		l.code.clear();
		l.hash.clear();
		l.lastNonEmpty = now;
		l.relay.Clear();
		l.server->SetSidecarHash("");
		l.server->ResetLobby();
	}

	json HandleMatchmaker(const json& req, ULONGLONG now) {
		std::string op = req.value("op", "");
		if (op == "ping") {
			int active = 0;
			for (auto& l : g_lobbies) {
				if (l.active) {
					active++;
				}
			}
			return { {"ok", true}, {"lobbies", active}, {"capacity", NUM_LOBBIES} };
		}
		if (op == "create") {
			for (auto& l : g_lobbies) {
				if (l.active) {
					continue;
				}
				l.active = true;
				l.code = NewCode();
				l.hash = req.value("hash", "");
				l.lastNonEmpty = now;
				l.relay.Clear();
				l.server->SetSidecarHash(l.hash);
				l.server->ResetLobby();
				spdlog::info("lobby {} created: code {} session :{} relay :{} by {}",
					l.index, l.code, l.sessionPort, l.relay.port, req.value("name", "?"));
				return { {"ok", true}, {"code", l.code}, {"session_port", l.sessionPort} };
			}
			return { {"ok", false}, {"error", "server_full"} };
		}
		if (op == "join") {
			std::string code = req.value("code", "");
			for (auto& c : code) {
				c = (char)toupper((unsigned char)c);
			}
			Lobby* l = FindByCode(code);
			if (!l) {
				return { {"ok", false}, {"error", "not_found"} };
			}
			if (l->PlayerCount() >= MAX_PLAYERS_PER_LOBBY) {
				return { {"ok", false}, {"error", "lobby_full"} };
			}
			std::string hash = req.value("hash", "");
			if (!l->hash.empty() && !hash.empty() && hash != l->hash) {
				return { {"ok", false}, {"error", "version_mismatch"} };
			}
			spdlog::info("lobby {} ({}) join lookup by {}", l->index, l->code, req.value("name", "?"));
			return { {"ok", true}, {"code", l->code}, {"session_port", l->sessionPort} };
		}
		return { {"ok", false}, {"error", "bad_request"} };
	}
}

int main(int argc, char** argv) {
	spdlog::set_default_logger(spdlog::stdout_color_mt("lobby"));
	spdlog::set_pattern("[%H:%M:%S] %^%l%$ %v");
	SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

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
		l.server.reset(new SessionServer("sf4e-lobby-" + std::to_string(i), "", true, 3, roundTime));
		l.server->SetRelayPort(FIRST_RELAY_PORT + i);
		if (l.server->Listen(l.sessionPort) != 0) {
			spdlog::critical("could not listen on session port {}", l.sessionPort);
			return 1;
		}
		if (!l.relay.Open(FIRST_RELAY_PORT + i)) {
			spdlog::critical("could not bind relay port {}", FIRST_RELAY_PORT + i);
			return 1;
		}
	}

	spdlog::info("sf4e lobby server up: matchmaker udp/{}, sessions udp/{}-{}, relays udp/{}-{}, {} lobbies",
		MATCHMAKER_PORT,
		FIRST_SESSION_PORT, FIRST_SESSION_PORT + NUM_LOBBIES - 1,
		FIRST_RELAY_PORT, FIRST_RELAY_PORT + NUM_LOBBIES - 1,
		NUM_LOBBIES);

	while (g_running) {
		ULONGLONG now = GetTickCount64();

		// Matchmaker requests: one JSON datagram in, one out.
		for (;;) {
			char buf[1500];
			sockaddr_in from = { 0 };
			int fromLen = sizeof(from);
			int n = recvfrom(matchmaker, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
			if (n <= 0) {
				break;
			}
			buf[n] = 0;
			json reply;
			try {
				reply = HandleMatchmaker(json::parse(buf), now);
			}
			catch (const std::exception&) {
				reply = { {"ok", false}, {"error", "bad_request"} };
			}
			std::string out = reply.dump();
			sendto(matchmaker, out.c_str(), (int)out.size(), 0, (sockaddr*)&from, sizeof(from));
		}

		SteamNetworkingSockets()->RunCallbacks();

		for (auto& l : g_lobbies) {
			l.server->PrepareForCallbacks();
			l.server->Step();
			l.relay.Pump(now);

			if (l.active) {
				if (l.PlayerCount() > 0) {
					l.lastNonEmpty = now;
				}
				else if (now - l.lastNonEmpty > EMPTY_LOBBY_TIMEOUT_MS) {
					ResetLobby(l, now);
				}
			}
		}

		Sleep(2);
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
