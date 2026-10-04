#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "sf4e__HolePunch.hxx"
#include "sf4e__Upnp.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	extern const int SESSION_CLIENT_MAX_MESSAGES_PER_POLL;

	class SessionClient
	{

	public:
		static bool bVerboseLogging;

		// Set when a match was ended because the two games' authoritative state
		// forked (a real desync). The lobby reads it to show the player why the
		// match dropped, then clears it.
		static bool bDesyncAbort;

		// The session connection to the lobby server dropped (timeout, or the
		// server went away). Set here, read and cleared by the lobby, which
		// tells the player. This used to pop a modal MessageBox, which BLOCKED
		// the game thread: the process stayed alive but froze and stopped
		// logging until somebody clicked OK -- indistinguishable from a hang,
		// and fatal to an unattended run.
		static bool bConnectionLost;

		// A player left the running match through the menu. Seats are the
		// ones in effect when the notice arrived, before the server rotates.
		struct ForfeitNotice {
			bool pending = false;
			int loserSide = -1;
			int mySide = -1;
		};
		static ForfeitNotice forfeit;

		enum ErrorType {
			SCE_UNKNOWN,
			SCE_JOIN_REJECTED_HASH_INVALID,
			SCE_JOIN_REJECTED_LOBBY_FULL,
			SCE_JOIN_REJECTED_NAME_TAKEN,
			SCE_JOIN_REJECTED_REQUEST_INVALID,
		};

		struct Callbacks {
			void* data;
			void (*OnError)(ErrorType errorType, SessionClient* const client, const Callbacks& callbacks);
			void (*OnReady)(SessionClient* const client, const Callbacks& callbacks);
			void (*OnBattleSynced)(SessionClient* const client, const Callbacks& callbacks);
		};

		SessionClient(
			const Callbacks& callbacks,
			std::string sidecarHash,
			uint16_t ggpoPort,
			std::string& name,
			bool spectator = false
		);
		~SessionClient();

		int Connect(HSteamNetConnection newConn);
		int Connect(const SteamNetworkingIPAddr& serverAddr);
		void Disconnect();
		int Step();
		void PrepareForCallbacks();

		// Lobby data
		std::string _name;
		// From the matchmaker; sent with the join request.
		std::string joinSecret;
		bool _spectator = false;
		SessionProtocol::LobbyData _lobbyData;
		SessionProtocol::MatchData _matchData;
		int64_t _outstandingReadyRequestNumber = -1;
		bool _snapshotsEnabled;

		EResult Lobby_Ready(int inputDelay, int serverPingMs = -1);
		EResult Lobby_Forfeit();
		EResult Lobby_Unready();
		EResult Lobby_ReportResults(int loserSide);

		EResult PreBattle_SetEnv(uint32_t rngSeed);
		EResult PreBattle_SetChara(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& chara);
		EResult PreBattle_SetStage(int32_t stageID);

		EResult Battle_Loaded();

		// Reports a match fork to the server: the frame, which fields disagreed,
		// and the conditions it happened under. Carries no name and no address.
		// Once per match -- a desync repeats every frame once it starts.
		void ReportDesync(int frame, bool gameplay, bool flowDiffers, const std::string& diff);

		// The live client, for code that has to reach it from a free function
		// (the desync handler runs deep inside the battle system).
		static SessionClient* Instance() { return s_pCallbackInstance; }


		// --- Direct play ------------------------------------------------------
		//
		// On by default (lobby CONNECTION option). A direct connection means
		// the other player's machine learns this machine's address, which the
		// relay otherwise never reveals.
		//
		// EnableDirect() binds the GGPO port, asks the server what address it
		// appears to come from, and offers that to the other player. The
		// socket is then HELD until the match starts, which is what keeps the
		// NAT mapping alive so the address stays true.
		// `altServer` is any OTHER region's address, used once to work out
		// whether this router can do peer to peer at all. Empty skips the check.
		bool EnableDirect(const SteamNetworkingIPAddr& matchmakerAddr,
			const SteamNetworkingIPAddr* altServer = nullptr);
		void DisableDirect();
		bool IsDirectEnabled() const { return _directEnabled; }
		int DirectRoundTripMs() const { return _punchProven ? _directRttMs : -1; }
		int MySide() const;

		// Called just before GGPO starts. Punches if the other player also
		// opted in, and reports whether a direct path opened. On false (the
		// usual case behind a strict NAT) the caller simply uses the relay.
		bool TryDirectPath();

		// How the CURRENT match is actually connected. Asked often enough out
		// loud that it belongs on screen rather than in a log file.
		bool IsMatchDirect() const { return _matchIsDirect; }

		// Call every frame while the lobby is up: keeps a proven path from
		// expiring before the match starts. Does nothing if there is no path.
		void PumpDirect();

		// Valid only after TryDirectPath() returned true.
		const std::string& DirectPeerIp() const { return _directPeerIp; }
		uint16_t DirectPeerPort() const { return _directPeerPort; }
		EResult Forward(const SessionProtocol::ConnectionID& dest, const nlohmann::json& msg);

		// Public for testing
		EResult Send(nlohmann::json& msg, int64_t* outMessageNum);
		
		// Connection related data - public for testing
		std::string _sidecarHash;
		uint16_t _ggpoPort;
		SteamNetworkingIPAddr _serverAddr;
		std::map<int, SessionProtocol::StateSnapshot> pendingRemoteSnapshots;
		SessionProtocol::ConnectionID _cid;
	private:

		// Opt-in direct play; see EnableDirect().
		HolePunch _punch;
		// Best-effort router port opening, tried once per session before the
		// punch. Costs one discovery timeout when there is no UPnP router.
		Upnp _upnp;
		bool _upnpMapped = false;
		bool _directEnabled = false;
		std::string _directPeerIp;
		uint16_t _directPeerPort = 0;
		std::string _directPeerLocalIp;
		uint16_t _directPeerLocalPort = 0;
		std::string _directToken;
		// One desync report per match; see ReportDesync().
		bool _desyncReported = false;
		// Spaced discovery attempts per lobby visit; see EnableDirect().
		bool _matchIsDirect = false;
		// A path proven in the lobby, kept warm until the match uses it.
		bool _punchProven = false;
		std::string _provenIp;
		uint16_t _provenPort = 0;
		int _directRttMs = -1;
		unsigned long _lastKeepaliveTick = 0;
		int _directAttempts = 0;
		unsigned long _directLastAttemptTick = 0;

		// Connection related data
		Callbacks _callbacks;
		bool _connected = false;
		HSteamNetConnection _conn;
		ISteamNetworkingSockets* _interface;


		void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

		static SessionClient* s_pCallbackInstance;
		static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);
	};
}
