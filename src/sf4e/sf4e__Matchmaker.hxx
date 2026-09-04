#pragma once

#include <string>
#include <cstdint>

#include <winsock2.h>
#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>

namespace sf4e {
	// Talks to the lobby server's matchmaker port to create a lobby or look
	// one up by code. Everything is non-blocking: call Poll() every frame and
	// read `state`. One request at a time.
	struct Matchmaker {
		enum class State {
			Idle,
			Waiting,
			Done,
			Failed,
		};

		State state = State::Idle;
		std::string error;

		// Filled in on Done.
		std::string code;
		uint16_t sessionPort = 0;

		// The server as resolved by Configure(); the session client connects
		// to this address on `sessionPort`.
		SteamNetworkingIPAddr serverAddr;
		std::string serverHost;

		// Resolves "host:port" (or "host", using the default matchmaker port).
		// Returns false and sets `error` if it cannot be resolved.
		bool Configure(const std::string& hostPort);
		bool IsConfigured() const;

		void Create(const std::string& sidecarHash, const std::string& name);
		void Join(const std::string& lobbyCode, const std::string& sidecarHash, const std::string& name);
		void Cancel();
		void Poll();

		// "a.b.c.d:port" for the resolved session endpoint, for StartSession.
		std::string SessionAddress() const;

		~Matchmaker();

	private:
		void Send(const std::string& payload);

		SOCKET _sock = INVALID_SOCKET;
		sockaddr_in _server = { 0 };
		bool _configured = false;
		std::string _pending;
		unsigned long long _sentAt = 0;
		int _attempts = 0;
	};
}
