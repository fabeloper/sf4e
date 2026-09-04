#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "sf4e__Matchmaker.hxx"

using nlohmann::json;
using sf4e::Matchmaker;

namespace {
	const uint16_t DEFAULT_MATCHMAKER_PORT = 23400;
	const unsigned long long RETRY_MS = 700;
	const int MAX_ATTEMPTS = 5;
}

Matchmaker::~Matchmaker() {
	if (_sock != INVALID_SOCKET) {
		closesocket(_sock);
	}
}

bool Matchmaker::IsConfigured() const {
	return _configured;
}

bool Matchmaker::Configure(const std::string& hostPort) {
	_configured = false;
	error.clear();

	std::string host = hostPort;
	uint16_t port = DEFAULT_MATCHMAKER_PORT;
	size_t colon = hostPort.rfind(':');
	if (colon != std::string::npos) {
		host = hostPort.substr(0, colon);
		port = (uint16_t)atoi(hostPort.c_str() + colon + 1);
		if (port == 0) {
			port = DEFAULT_MATCHMAKER_PORT;
		}
	}
	if (host.empty()) {
		error = "no server configured";
		return false;
	}

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	addrinfo hints = { 0 };
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo* result = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
		error = "could not resolve " + host;
		return false;
	}
	_server = *(sockaddr_in*)result->ai_addr;
	_server.sin_port = htons(port);
	freeaddrinfo(result);

	serverAddr.Clear();
	serverAddr.SetIPv4(ntohl(_server.sin_addr.s_addr), port);
	serverHost = host;

	if (_sock == INVALID_SOCKET) {
		_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (_sock == INVALID_SOCKET) {
			error = "could not open a socket";
			return false;
		}
		u_long nonBlocking = 1;
		ioctlsocket(_sock, FIONBIO, &nonBlocking);
	}

	_configured = true;
	return true;
}

std::string Matchmaker::SessionAddress() const {
	char ip[INET_ADDRSTRLEN] = { 0 };
	inet_ntop(AF_INET, &_server.sin_addr, ip, sizeof(ip));
	return std::string(ip) + ":" + std::to_string(sessionPort);
}

void Matchmaker::Send(const std::string& payload) {
	_pending = payload;
	_attempts = 0;
	_sentAt = 0;
	state = State::Waiting;
	error.clear();
	code.clear();
	sessionPort = 0;
	Poll();
}

void Matchmaker::Create(const std::string& sidecarHash, const std::string& name) {
	if (!_configured) {
		state = State::Failed;
		error = "no server configured";
		return;
	}
	json req = { {"op", "create"}, {"hash", sidecarHash}, {"name", name} };
	Send(req.dump());
}

void Matchmaker::Join(const std::string& lobbyCode, const std::string& sidecarHash, const std::string& name) {
	if (!_configured) {
		state = State::Failed;
		error = "no server configured";
		return;
	}
	json req = { {"op", "join"}, {"code", lobbyCode}, {"hash", sidecarHash}, {"name", name} };
	Send(req.dump());
}

void Matchmaker::Cancel() {
	state = State::Idle;
	_pending.clear();
}

void Matchmaker::Poll() {
	if (state != State::Waiting) {
		return;
	}

	unsigned long long now = GetTickCount64();
	if (_sentAt == 0 || now - _sentAt > RETRY_MS) {
		if (_attempts >= MAX_ATTEMPTS) {
			state = State::Failed;
			error = "no answer from " + serverHost + " (is the lobby server running?)";
			return;
		}
		sendto(_sock, _pending.c_str(), (int)_pending.size(), 0, (sockaddr*)&_server, sizeof(_server));
		_sentAt = now;
		_attempts++;
	}

	char buf[1500];
	sockaddr_in from = { 0 };
	int fromLen = sizeof(from);
	int n = recvfrom(_sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
	if (n <= 0) {
		return;
	}
	buf[n] = 0;

	try {
		json reply = json::parse(buf);
		if (reply.value("ok", false)) {
			code = reply.value("code", "");
			sessionPort = (uint16_t)reply.value("session_port", 0);
			state = State::Done;
			spdlog::info("Matchmaker: lobby {} on session port {}", code, sessionPort);
		}
		else {
			std::string reason = reply.value("error", "unknown");
			if (reason == "not_found") {
				error = "no lobby with that code";
			}
			else if (reason == "lobby_full") {
				error = "that lobby already has two players";
			}
			else if (reason == "version_mismatch") {
				error = "the other player runs a different sf4e build";
			}
			else if (reason == "server_full") {
				error = "the server has no free lobbies right now";
			}
			else {
				error = reason;
			}
			state = State::Failed;
		}
	}
	catch (const std::exception&) {
		error = "bad reply from server";
		state = State::Failed;
	}
}
