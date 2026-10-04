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

		// Spectator relay pipes: slot k uses ports _spectatorRelayBase + 2k
		// (P1 sends there) and + 2k + 1 (the spectator sends there). 0 means
		// no relay, and spectators reach P1 directly.
		uint16_t _spectatorRelayBase;
		int _spectatorSlots;

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
		void BroadcastMessage(const nlohmann::json& msg);
		void Respond(HSteamNetConnection client, const nlohmann::json& msg);

		// Direct lobby data manipulation utilities
		SessionProtocol::JoinResult RegisterToWait(
			const HSteamNetConnection& conn,
			const uint16_t& port,
			const std::string& sidecarHash,
			const std::string& name,
			const SteamNetworkingIPAddr& peerAddr,
			SessionProtocol::ConnectionID& cid,
			bool spectator,
			const std::string& secret
		);

		// Handed out by the matchmaker with the lobby code; a join without it
		// is refused when required. Empty + required = nobody can join.
		std::string _joinSecret;
		bool _requireJoinSecret = false;
		int _desyncReports = 0;
		// Every accepted connection and its IPv4, for the per-lobby and
		// per-address caps.
		std::map<HSteamNetConnection, uint32_t> _accepted;
		void HandleResults(int loserSide);
		void HandleForfeit(HSteamNetConnection conn);

		// What each seat sent with its ready, kept until the match starts.
		SessionProtocol::LobbyReady _readyRequest[2];
		int MeasuredRoundTripMs() const;
		int SharedInputDelay() const;

		// Once both players have offered an endpoint, hand each of them the
		// other's, with a shared token so the punch cannot be spoofed. Does
		// nothing until both have opted in.
		void MaybeExchangeDirectEndpoints();
		bool _directExchanged = false;

		// Who actually chose the character sitting in each seat. The picks
		// themselves have to be indexed by side because that is what the clients
		// apply, but a seat is not an identity: players rotate on a winner-stays
		// result, and they leave and rejoin in whatever order they like. Holding
		// the owner alongside lets a pick be discarded the moment it stops
		// describing whoever is actually in that seat, which is the difference
		// between "cleared on the membership changes we thought of" and "cannot
		// be wrong".
		SessionProtocol::ConnectionID _charaOwner[2];
		void DropCharaFromVacatedSeats();

		// When the current match started, for the duration in the stats line.
		// 0 means no match is running.
		uint64_t _matchStartMs = 0;

	public:
		// Aggregate, non-identifying usage stats: how many matches are played,
		// how long they last, how they end. Deliberately records NO player
		// names and NO IP addresses -- those are personal data under GDPR, and
		// they answer none of the questions this is for. One JSON object per
		// line, appended to the file named by the SF4E_STATS_FILE environment
		// variable; if that is unset, nothing is written at all.
		static void LogStat(const std::string& event, const nlohmann::json& fields);

		// The addresses currently in this lobby, host byte order. The relay
		// only learns endpoints from these, so a stranger spraying the relay
		// ports cannot take a player's slot or inject packets into the match.
		std::vector<uint32_t> MemberIPv4s() const;
		// The two player seats in order (0 when empty or unknown).
		std::vector<uint32_t> PlayerIPv4s() const;

		void SetJoinSecret(const std::string& secret);
		void SetPublic(bool isPublic);
		void RequireJoinSecret(bool required);
		// Close every connection; used when a lobby is released or reused.
		void DisconnectAll();

		// Drop the remembered character picks (and readiness) after any change to
		// who is in the lobby. The picks are indexed by side, so a join or a
		// leave can leave them describing the wrong player.
		// Called whenever someone joins or leaves. Picks and readiness are NOT
		// reset here -- DropCharaFromVacatedSeats() handles those by owner.
		void OnMembershipChanged();

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

		// Relay pipes for spectators, see _spectatorRelayBase.
		void SetSpectatorRelayPorts(uint16_t basePort, int slots);

		// Players come first in `clients`; spectators after them.
		int PlayerCount() const;
		int SpectatorCount() const;

		// The build every joiner must match. Empty accepts any build; a lobby
		// service sets it from the creator so both players run the same one.
		void SetSidecarHash(const std::string& hash);

		// Forget match progress so the lobby can be handed to new players.
		void ResetLobby();

		typedef struct SessionMember {
			SessionProtocol::MemberData data;
			HSteamNetConnection conn;
			int spectatorSlot = -1;
			// The address this member's session actually came from, kept out of
			// `data` so it is never broadcast to the other members. The relay
			// uses it to refuse traffic from anyone who is not in this lobby.
			// Host byte order; 0 if unknown.
			uint32_t peerIPv4 = 0;
			// Set when this player opted into direct play, and held privately
			// here rather than in `data` so it is never broadcast: it only ever
			// goes to the one other player, and only once they opt in too.
			std::string directIp;
			uint16_t directPort = 0;
			std::string directLocalIp;
			uint16_t directLocalPort = 0;
		} SessionMember;

		std::map<HSteamNetConnection, SessionProtocol::ConnectionID> cidMap;
		std::vector<SessionMember> clients;

		// Lobby data: Public for visibility into tests only.
		bool _dataDirty;
		SessionProtocol::LobbyData _lobbyData;
		SessionProtocol::MatchData _matchData;
	};
}