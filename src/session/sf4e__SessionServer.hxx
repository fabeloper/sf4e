#pragma once

#include <map>
#include <string>
#include <vector>

#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <nlohmann/json.hpp>

#include "../Dimps/Dimps__Math.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	extern const int SESSION_SERVER_MAX_MESSAGES_PER_POLL;

	class SessionServer
	{
	private:
		// A binary blob usable by any host for routing messages to this
		// server. This is most likely an IP address and port, a hostname
		// and port, or in extreme cases an overlay network's concept of
		// addressing (ex. an index in a service discovery protocol).
		// Identities of clients connected to the server are prefixed with
		// this identity. This allows all servers in a cluster to forward
		// messages to any user connected to any server in the cluster-
		// just send it to the prefixed identity, and that host will take
		// care of the rest.
		std::string _identity;

		// Connection related data
		std::string _sidecarHash;
		HSteamListenSocket _listenSock;
		HSteamNetPollGroup _pollGroup;
		ISteamNetworkingSockets* _interface;

		// When nonzero, every member is reported to the others as reachable
		// at the session host on this UDP port instead of at their real
		// address. A relay on that port forwards GGPO traffic between them,
		// so neither player needs a reachable address of their own.
		uint16_t _relayPort;

		// Connection callbacks and message utilities.
		//
		// GameNetworkingSockets delivers connection status changes through
		// one function pointer, so several servers in one process need a way
		// to find the instance a change belongs to. Each listen socket is
		// registered here on creation; the callback routes by the listen
		// socket the connection arrived on, falling back to the single
		// instance for anything else.
		static SessionServer* s_pCallbackInstance;
		static std::map<HSteamListenSocket, SessionServer*> s_byListenSocket;
		static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);
		void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);
		void BroadcastMessage(nlohmann::json& msg);
		void Respond(HSteamNetConnection client, nlohmann::json& msg);

		// Direct lobby data manipulation utilities
		SessionProtocol::JoinResult RegisterToWait(
			const HSteamNetConnection& conn,
			const uint16_t& port,
			const std::string& sidecarHash,
			const std::string& name,
			const SteamNetworkingIPAddr& peerAddr,
			SessionProtocol::ConnectionID& cid
		);
		void HandleResults(int loserSide);

	public:
		SessionServer(
			std::string identity,
			std::string sidecarHash,
			bool editionSelect,
			int roundCount,
			Dimps::Math::FixedPoint roundTime
		);
		~SessionServer();

		void AddConnection(HSteamNetConnection newConn);
		int Listen(uint16 nPort);
		int Step();
		int Close();
		void PrepareForCallbacks();
		void ResetBattleSync();

		// Route the members' peer traffic through a relay on this port (0
		// disables). See _relayPort.
		void SetRelayPort(uint16_t port);

		// The build every joiner must match. Empty accepts any build; a lobby
		// service sets it from the creator so both players run the same one.
		void SetSidecarHash(const std::string& hash);

		// Forget match progress so the lobby can be handed to new players.
		void ResetLobby();

		typedef struct SessionMember {
			SessionProtocol::MemberData data;
			HSteamNetConnection conn;
		} SessionMember;

		std::map<HSteamNetConnection, SessionProtocol::ConnectionID> cidMap;
		std::vector<SessionMember> clients;

		// Lobby data: Public for visibility into tests only.
		bool _dataDirty;
		SessionProtocol::LobbyData _lobbyData;
		SessionProtocol::MatchData _matchData;
	};
}