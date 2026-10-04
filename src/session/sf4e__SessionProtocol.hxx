#pragma once

#include <string>
#include <utility>
#include <vector>
#include <cstdint>


#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <nlohmann/json.hpp>

#include "../Dimps/Dimps__Math.hxx"

// The lobby server builds for Linux, where the game-reversing headers cannot
// follow: Dimps__GameEvents.hxx reaches Dimps__Platform.hxx, which includes
// <d3d9.h>. The protocol only ever needed two PODs out of all that, so on
// Windows we alias the real game types (keeping every existing client
// assignment valid and the layout identical by construction), and elsewhere we
// declare layout-compatible standalone copies.
#ifdef _WIN32
#include "../Dimps/Dimps__GameEvents.hxx"
#endif

#define MAX_SF4E_PROTOCOL_USERS 4
#define MAX_SF4E_SPECTATORS 2

namespace sf4e {
	namespace SessionProtocol {
		typedef Dimps::Math::FixedPoint FixedPoint;

#ifdef _WIN32
		// The game's own type, so client code assigning a live
		// ConfirmedCharaConditions into MatchData keeps compiling unchanged.
		typedef Dimps::GameEvents::VsMode::ConfirmedCharaConditions CharaConditions;
#else
		// Server-side copy. Field names and order must match the Windows type
		// exactly: they are what the JSON serialiser writes, so a mismatch
		// would silently break the wire format between server and clients.
		struct CharaConditions {
			uint8_t charaID;
			uint8_t costume;
			uint8_t color;
			uint8_t _unused;
			uint8_t personalAction;
			uint8_t winQuote;
			uint8_t ultraCombo;
			uint8_t handicap;
			uint8_t unc_edition;
		};
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
			CharaConditions,
			charaID, costume, color, _unused, personalAction,
			winQuote, ultraCombo, handicap, unc_edition);
#endif

		// Bounds for values the game uses as table indexes; they come from the
		// other player, so the server rejects and the client sanitizes.
		const int CHARA_COUNT = 0x2c;
		const int STAGE_COUNT = 30;
		const int COSTUME_COUNT = 8;

		// A seat that sends INPUT_DELAY_AUTO lets the server pick from the
		// measured round trip between the two players.
		const int INPUT_DELAY_AUTO = 0;
		const int INPUT_DELAY_DEFAULT = 2;
		const int INPUT_DELAY_MAX = 8;

		inline int InputDelayForRoundTrip(int roundTripMs) {
			if (roundTripMs < 0) return INPUT_DELAY_DEFAULT;
			if (roundTripMs <= 50) return 1;
			if (roundTripMs <= 100) return 2;
			if (roundTripMs <= 150) return 3;
			if (roundTripMs <= 200) return 4;
			return 5;
		}
		const int COLOR_COUNT = 10;
		const int ULTRA_COUNT = 3;
		bool CharaConditionsValid(const CharaConditions& c);
		bool StageIDValid(int64_t stageID);
		bool SanitizeCharaConditions(CharaConditions& c);

		// Connection IDs are ephemeral and reusable- they can be used to
		// distinguish clients from each other, but should not be used as
		// any kind of stable identifier. "user" in this context is _not_
		// a typical user account- it only correspond to the `username`
		// portion of a URL. Care should be taken that any references to
		// connection IDs (including those inside client code) are released
		// or deleted when the connection is terminated, to prevent
		// referring to duplicate IDs.
		struct ConnectionID {
			std::string host;
			std::string user;

			bool operator==(const ConnectionID&);
		};

		// Similar to connection IDs, lobby IDs are ephemeral and reusable.
		// All lobby IDs containing an empty string as the host or key are
		// equivalent and unreachable, and the canonical null lobby, which
		// contains no members, is represented as {"", ""}.
		struct LobbyID {
			std::string host;
			std::string key;

			bool operator==(const LobbyID& rhs);
			static const LobbyID NULL_LOBBY_ID;
		};

		enum MemberFlags {
			MF_BATTLE_LOADED = 1,
		};

		struct MemberData {
			ConnectionID connId;

			// A user-provided display name.
			std::string name;

			// The IP the client has connected from. While not guaranteed to
			// be reachable, other P2P libraries (ex. GGPO) can try to leverage
			// this to connect directly.
			std::string ip;
			uint16_t port;
			uint64_t flags;

			// Spectators watch the match through P1. `port` is where P1 sends
			// them the confirmed inputs; `hostPort` is where the spectator sends
			// its own traffic (a relay pipe has one port per direction; 0 means
			// P1's own address). `watching` is set by the server for the
			// spectators that were present when the current match was called,
			// so P1 only waits for those.
			bool spectator = false;
			bool watching = false;
			uint16_t hostPort = 0;
		};

		struct LobbyData {
			LobbyID id;
			bool editionSelect;
			int roundCount;
			FixedPoint roundTime;
			bool isPublic = false;
			std::vector<MemberData> members;

			static const LobbyData NULL_LOBBY;
		};

		struct MatchData {
			MatchData();
			void Clear();
			bool IsAllReady();

			int64_t readyMessageNum[2];
			CharaConditions chara[2];
			int64_t stageID;
			uint32_t rngSeed;   // was DWORD; identical width, and portable
			// Per seat as chosen; both stamped with the higher value once ready.
			int32_t inputDelay[2];
		};

		enum MessageType {
			MT_SESSION_HELLO,
			MT_SESSION_HELLO_RESP,

			MT_SESSION_DATAUPDATE,
			MT_SESSION_JOINREQ,
			MT_SESSION_JOINREJ,

			MT_LOBBY_READY,
			MT_LOBBY_ALLREADY,
			MT_LOBBY_REPORTRESULTS,

			MT_PREBATTLE_SETENV,
			MT_PREBATTLE_SETCHARA,
			MT_PREBATTLE_SETSTAGE,

			MT_BATTLE_LOADED,
			MT_BATTLE_SYNCED,
			MT_BATTLE_SNAPSHOT,

			// Opt-in direct play. The offer carries the sender's own public
			// endpoint; the server hands each player the other's ONLY once both
			// have opted in, and never puts it in the broadcast lobby data
			// (which spectators also receive).
			MT_DIRECT_OFFER,
			MT_DIRECT_PEER,

			// A match forked. Sent so a desync anywhere in the world lands in
			// the server log with its evidence attached, instead of depending on
			// a player volunteering their log file -- which is exactly what we
			// could not get from the one report that mattered most.
			MT_DESYNC_REPORT,

			// A player left a running match through the menu; it counts as
			// their loss. The server answers both players with MT_MATCH_FORFEITED.
			MT_LOBBY_FORFEIT,
			MT_MATCH_FORFEITED,

			MT_FORWARD,
		};

		NLOHMANN_JSON_SERIALIZE_ENUM(MessageType, {
			{MT_SESSION_HELLO, "hello"},
			{MT_SESSION_HELLO_RESP, "hello_resp"},
			{MT_SESSION_DATAUPDATE, "data_update"},
			{MT_SESSION_JOINREJ, "join_rej"},
			{MT_SESSION_JOINREQ, "join_req"},

			{MT_LOBBY_READY, "lobby_ready"},
			{MT_LOBBY_ALLREADY, "lobby_allready"},
			{MT_LOBBY_REPORTRESULTS, "lobby_reportresults"},

			{MT_PREBATTLE_SETENV, "prebattle_setenv"},
			{MT_PREBATTLE_SETCHARA, "prebattle_setchara"},
			{MT_PREBATTLE_SETSTAGE, "prebattle_setstage"},

			{MT_BATTLE_LOADED, "battle_loaded"},
			{MT_BATTLE_SYNCED, "battle_synced"},
			{MT_BATTLE_SNAPSHOT, "battle_snapshot"},

			{MT_DIRECT_OFFER, "direct_offer"},
			{MT_DIRECT_PEER, "direct_peer"},
			{MT_DESYNC_REPORT, "desync_report"},

			{MT_LOBBY_FORFEIT, "lobby_forfeit"},
			{MT_MATCH_FORFEITED, "match_forfeited"},

			{MT_FORWARD, "forward"},
		})

		enum JoinResult {
			JOIN_OK = 0,
			JR_REQUEST_INVALID = 1,
			JR_LOBBY_FULL = 2,
			JR_NAME_TAKEN = 3,
			JR_HASH_INVALID = 4,
			JR_SECRET_INVALID = 5,
		};

		NLOHMANN_JSON_SERIALIZE_ENUM(JoinResult, {
			{JOIN_OK, "ok"},
			{JR_REQUEST_INVALID, "request_invalid"},
			{JR_LOBBY_FULL, "lobby_full"},
			{JR_NAME_TAKEN, "name_taken"},
			{JR_HASH_INVALID, "hash_invalid"},
			{JR_SECRET_INVALID, "secret_invalid"}
		})

		struct SessionHelloMsg {
			MessageType type = MT_SESSION_HELLO;
			ConnectionID cid;
		};

		struct SessionHelloResp {
			MessageType type = MT_SESSION_HELLO_RESP;
			ConnectionID cid;
		};

		struct SessionDataUpdate {
			MessageType type = MT_SESSION_DATAUPDATE;
			LobbyData lobbyData;
			MatchData matchData;
		};

		struct SessionJoinReject {
			MessageType type = MT_SESSION_JOINREJ;
			JoinResult result;
		};

		struct SessionJoinRequest {
			MessageType type = MT_SESSION_JOINREQ;
			std::string sidecarHash;
			std::string username;
			uint16_t port;
			bool spectator = false;
			std::string secret;
		};

		struct LobbyReady {
			MessageType type = MT_LOBBY_READY;
			// The delay this player chose. The server plays both seats at the
			// higher of the two, so the fighter who chose less does not get
			// responsive controls while the opponent absorbs the rollbacks.
			int32_t inputDelay = -1;
			// False takes the seat back to not ready; ignored once both are ready.
			bool ready = true;
			// Round trips this player measured, or -1: to the lobby server, and
			// to the other player over a proven direct path.
			int32_t serverPingMs = -1;
			int32_t directRoundTripMs = -1;
		};

		struct LobbyAllReady {
			MessageType type = MT_LOBBY_ALLREADY;
		};

		struct LobbyReportResults {
			MessageType type = MT_LOBBY_REPORTRESULTS;
			int32_t loserSide;
		};

		struct LobbyForfeit {
			MessageType type = MT_LOBBY_FORFEIT;
		};

		struct MatchForfeited {
			MessageType type = MT_MATCH_FORFEITED;
			int32_t loserSide = -1;
		};

		struct PreBattleSetEnv {
			MessageType type = MT_PREBATTLE_SETENV;
			uint32_t rngSeed;
		};

		struct PreBattleSetChara {
			MessageType type = MT_PREBATTLE_SETCHARA;
			CharaConditions chara;
		};
		// Sent by a player who has turned direct play on, carrying the public
		// endpoint their own GGPO socket appears to come from.
		struct DirectOffer {
			MessageType type = MT_DIRECT_OFFER;
			std::string ip;
			uint16_t port = 0;
			// The address on the sender's own network. Two players behind the
			// same router cannot generally reach each other at their shared
			// public address -- that needs hairpin NAT, which many routers do
			// not do -- but they can always reach each other directly on the
			// LAN. Every real P2P stack gathers both for this reason.
			std::string localIp;
			uint16_t localPort = 0;
		};

		// The other player's endpoint, sent to each of the two players alone
		// and only when both have offered. An empty ip means "the other side did
		// not opt in" -- stay on the relay.
		struct DesyncReport {
			MessageType type = MT_DESYNC_REPORT;
			int32_t frame = 0;
			// Did the authoritative state fork, or was it only cosmetic drift?
			bool gameplay = false;
			// Were the two machines at different points in the match? Separates
			// a timing problem from a simulation one; they need different fixes.
			bool flowDiffers = false;
			// The conditions the fork happened under.
			int32_t inputDelay = -1;
			bool direct = false;
			int32_t pingMs = -1;
			// Which fields disagreed, truncated. No names, no addresses.
			std::string diff;
		};

		struct DirectPeer {
			MessageType type = MT_DIRECT_PEER;
			std::string ip;
			uint16_t port = 0;
			std::string localIp;
			uint16_t localPort = 0;
			std::string token;
		};


		struct PreBattleSetStage {
			MessageType type = MT_PREBATTLE_SETSTAGE;
			int32_t stageID;
		};

		struct BattleLoaded {
			MessageType type = MT_BATTLE_LOADED;
		};

		struct BattleSynced {
			MessageType type = MT_BATTLE_SYNCED;
		};

		// Soak-test GameManager probe geometry. One chunk per checksum, sampled
		// from the GameManager base out past GM_SAVED_BYTES (what the save
		// state actually copies) so we can tell whether the round state that
		// forks lives in memory we never save.
		// Probe ONLY the region we actually save. Measured: reading past 0x49c
		// lands in unrelated heap allocations that differ between two machines
		// by nature, so it reported a divergence every snapshot and buried the
		// signal. Everything fully inside the object compares clean during
		// normal play, so a hit here now means something real.
		static const size_t GM_CHUNK_BYTES = 64;
		static const size_t GM_SAVED_BYTES = 0x49c;   // what SaveState copies
		static const size_t GM_PROBE_BYTES = 1152;    // 18 chunks, last one ending before 0x49c
		static const size_t GM_MAX_CHUNKS = GM_PROBE_BYTES / GM_CHUNK_BYTES;

		struct StateSnapshot {
			struct CharaStateSnapshot {
				int status;
				float rootPos[4];
				int side;
				FixedPoint vit;
				FixedPoint vitmax;
				FixedPoint revenge;
				FixedPoint revengemax;
				FixedPoint recoverable;
				FixedPoint recoverablemax;
				FixedPoint super;
				FixedPoint supermax;
				FixedPoint sctimeamt;
				FixedPoint sctimemax;
				FixedPoint uctime;
				FixedPoint uctimemax;
				FixedPoint damage;
				FixedPoint combodamage;
				// The move in progress: which action, its frame, the posture,
				// and the unit time scale. A fork here shows frames before it
				// reaches health or position.
				int action;
				FixedPoint actionFrame;
				int posture;
				FixedPoint timeScale;
			};

			int frameIdx;
			// Battle-flow state (which phase of the round: fighting, KO,
			// round-over, next-round), for diagnosing a round-transition
			// desync: if these differ, the two machines are in different
			// rounds. Diagnostic only -- not part of the abort decision.
			int battleFlow = 0;
			int battleFlowSubstate = 0;
			// Soak-test diagnostic: checksums of successive GM_CHUNK_BYTES
			// blocks of the game's GameManager, starting at its base and
			// running PAST the 0x49c the save state copies. The round/match
			// bookkeeping (how many rounds each side has won) is believed to
			// live here, and 0x49c is a GUESS -- so if a chunk whose offset is
			// >= 0x49c diverges between the two machines, round state is
			// living in memory we never save or restore, which is exactly how
			// one machine can decide "match over" while the other fights on.
			// Diagnostic only -- never part of the abort decision.
			//
			// A FIXED ARRAY, not a vector: this whole struct is compared with
			// memcmp (see SessionClient), so it must stay free of pointers.
			uint32_t gmChunks[GM_MAX_CHUNKS] = { 0 };
			CharaStateSnapshot chara[2];
		};

		struct BattleSnapshot {
			MessageType type = MT_BATTLE_SNAPSHOT;
			StateSnapshot snapshot;
		};

		struct ForwardMessage {
			MessageType type = MT_FORWARD;
			ConnectionID src;
			ConnectionID dest;
			nlohmann::json msg;
		};

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ConnectionID, host, user);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyID, host, key);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MemberData, connId, name, ip, port, spectator, watching, hostPort);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LobbyData, id, editionSelect, roundCount, roundTime, members, isPublic);
		// Explicit rather than the macro: MatchData holds C arrays, which the
		// _WITH_DEFAULT form cannot assign, and inputDelay must default when a
		// server that predates it leaves the key out.
		inline void to_json(nlohmann::json& j, const MatchData& m) {
			j = nlohmann::json{
				{"readyMessageNum", m.readyMessageNum},
				{"chara", m.chara},
				{"stageID", m.stageID},
				{"rngSeed", m.rngSeed},
				{"inputDelay", m.inputDelay},
			};
		}
		inline void from_json(const nlohmann::json& j, MatchData& m) {
			j.at("readyMessageNum").get_to(m.readyMessageNum);
			j.at("chara").get_to(m.chara);
			j.at("stageID").get_to(m.stageID);
			j.at("rngSeed").get_to(m.rngSeed);
			if (j.contains("inputDelay")) {
				j.at("inputDelay").get_to(m.inputDelay);
			}
			else {
				m.inputDelay[0] = m.inputDelay[1] = -1;
			}
		}

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionHelloMsg, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionHelloResp, type, cid);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionDataUpdate, type, lobbyData, matchData);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionJoinReject, type, result);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionJoinRequest, type, sidecarHash, username, port, spectator, secret);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LobbyReady, type, inputDelay, ready, serverPingMs, directRoundTripMs);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyAllReady, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyReportResults, type, loserSide);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LobbyForfeit, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MatchForfeited, type, loserSide);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetChara, type, chara);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DirectOffer, type, ip, port, localIp, localPort);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DesyncReport, type, frame, gameplay, flowDiffers, inputDelay, direct, pingMs, diff);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DirectPeer, type, ip, port, localIp, localPort, token);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetEnv, type, rngSeed);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreBattleSetStage, type, stageID);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ForwardMessage, type, src, dest, msg);

		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StateSnapshot::CharaStateSnapshot, status, rootPos, side, vit, vitmax, revenge, revengemax, recoverable, recoverablemax, super, supermax, sctimeamt, sctimemax, uctime, uctimemax, damage, combodamage, action, actionFrame, posture, timeScale);
		// (StateSnapshot itself is defined below with battleFlow included.)
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StateSnapshot, frameIdx, battleFlow, battleFlowSubstate, gmChunks, chara);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleSnapshot, type, snapshot);

		// Names every snapshot field that differs between two states, with both
		// values. One place, used by the online desync report and the local sync
		// test alike, so a divergence always says exactly what drifted.
		std::string DescribeSnapshotDiff(const StateSnapshot& mine, const StateSnapshot& theirs);

		// True if the two states differ in authoritative gameplay -- health,
		// meter, animation state, side -- as opposed to only the derived float
		// render position (rootPos). A gameplay difference means the fights
		// have genuinely forked and the match cannot continue; a position-only
		// difference is drift that has not (yet) changed the outcome.
		bool SnapshotGameplayDiffers(const StateSnapshot& a, const StateSnapshot& b);

		// True when the two machines disagree about where in the match they
		// are (battle flow), as opposed to what happened in it. Never ends a
		// match by itself; it classifies the cause. See the implementation.
		bool SnapshotFlowDiffers(const StateSnapshot& a, const StateSnapshot& b);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleLoaded, type);
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BattleSynced, type);
	}
}