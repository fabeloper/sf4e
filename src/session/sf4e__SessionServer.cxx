#include <algorithm>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <utility>
#include <vector>
// Not needed by anything here; kept Windows-only so the Linux server builds.
#ifdef _WIN32
#include <windows.h>
#endif

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
// Unused here, and GGPO is a Windows-side dependency the server does not have.
#ifdef _WIN32
#include <ggponet.h>
#endif

// FixedPoint is the only thing this file actually uses out of Dimps, and that
// header is portable.
#include "../Dimps/Dimps__Math.hxx"

// The rest are game-side and unused here: they reach <windows.h>, <d3d9.h> and
// GGPO, none of which exist on the Linux server.
#ifdef _WIN32
#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../sf4e/sf4e__Game__Battle__System.hxx"
#include "../sf4e/sf4e__GameEvents.hxx"
#endif

#include "sf4e__SessionProtocol.hxx"
#include "sf4e__SessionServer.hxx"

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
using Dimps::Math::FixedPoint;
using sf4e::SessionServer;


namespace {
	// Monotonic milliseconds, so a clock adjustment cannot produce a negative
	// or absurd match duration in the stats.
	uint64_t NowMs() {
		return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
}

const int sf4e::SESSION_SERVER_MAX_MESSAGES_PER_POLL = 200;
SessionServer* SessionServer::s_pCallbackInstance;
std::map<HSteamListenSocket, SessionServer*> SessionServer::s_byListenSocket;

SessionServer::SessionServer(std::string identity, std::string sidecarHash, bool editionSelect, int roundCount, FixedPoint roundTime) :
	_identity(identity),
	_sidecarHash(sidecarHash),
	_interface(SteamNetworkingSockets()),
	_dataDirty(false),
	_lobbyData(SessionProtocol::LobbyData::NULL_LOBBY),
	_listenSock(k_HSteamListenSocket_Invalid),
	_relayPort(0),
	_spectatorRelayBase(0),
	_spectatorSlots(0)
{
	_lobbyData.id = { _identity, "1" };
	_lobbyData.editionSelect = editionSelect;
	_lobbyData.roundCount = roundCount;
	_lobbyData.roundTime = roundTime;
	clients.reserve(MAX_SF4E_PROTOCOL_USERS + 1);
	_pollGroup = _interface->CreatePollGroup();
}

SessionServer::~SessionServer()
{
	if (_listenSock != k_HSteamListenSocket_Invalid) {
		Close();
		_listenSock = k_HSteamListenSocket_Invalid;
	}
}

void SessionServer::AddConnection(HSteamNetConnection newConn) {
	// XXX (adanducci): It is absolutely critical to note that
	// `SetConfigValue`'s interface to set callbacks is _not_ the
	// same as the one used by `CreateListenSocketIP`/`SteamNetworkingConfigValue_t`.
	// 
	// Per the documentation for `SetConfigValue` and the header comment @
	// https://github.com/ValveSoftware/GameNetworkingSockets/blob/62b395172f157ca4f01eea3387d1131400f8d604/include/steam/isteamnetworkingutils.h#L296-L307 :
	//
	// NOTE: When setting pointers (e.g. callback functions), do not pass the function pointer
	// directly. Your argument should be a pointer to a function pointer.
	//
	// `CreateListenSocketIP`/`SteamNetworkingConfigValue_t` just takes the
	// function pointer directly. The failure mode if you pass the function
	// pointer directly is _extremely_ confusing- it just appears to be
	// a segfault in the GNS callback loop.
	// Explicit cast: converting a function pointer to void* is a GCC error
	// without it. GNS wants a pointer TO the function pointer (see above).
	void* callback = (void*)SteamNetConnectionStatusChangedCallback;
	SteamNetworkingUtils()->SetConfigValue(
		k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		k_ESteamNetworkingConfig_Connection,
		newConn,
		k_ESteamNetworkingConfig_Ptr,
		&callback
	);
	_interface->SetConnectionPollGroup(newConn, _pollGroup);
}

int SessionServer::Listen(uint16 nPort) {
	SteamNetworkingIPAddr serverLocalAddr;
	serverLocalAddr.Clear();
	serverLocalAddr.m_port = nPort;
	SteamNetworkingConfigValue_t opt;
	opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)SteamNetConnectionStatusChangedCallback);
	_listenSock = _interface->CreateListenSocketIP(serverLocalAddr, 1, &opt);
	if (_listenSock == k_HSteamListenSocket_Invalid) {
		spdlog::error("Failed to listen on port {}", nPort);
		return -1;
	}
	s_byListenSocket[_listenSock] = this;
	spdlog::info("Server listening on port {}", nPort);
	return 0;
}

void SessionServer::SetRelayPort(uint16_t port) {
	_relayPort = port;
}

void SessionServer::SetSpectatorRelayPorts(uint16_t basePort, int slots) {
	_spectatorRelayBase = basePort;
	_spectatorSlots = slots;
}

int SessionServer::PlayerCount() const {
	int n = 0;
	for (auto iter = clients.begin(); iter != clients.end(); iter++) {
		if (!iter->data.spectator) n++;
	}
	return n;
}

void SessionServer::OnMembershipChanged() {
	// Deliberately does NOT touch the picks or the ready flags.
	//
	// It used to clear both, to stop a stale seat-indexed pick describing the
	// wrong player. DropCharaFromVacatedSeats() now does that precisely, by
	// owner, so the blanket wipe was pure collateral damage: a player who
	// picked a character and pressed READY while waiting had their readiness
	// erased the moment their opponent walked in, so the lobby could never
	// reach all-ready and the match simply never started. Being ready before
	// the other player arrives is the normal way to use a lobby.
	_directExchanged = false;
	_dataDirty = true;
}

void SessionServer::DropCharaFromVacatedSeats() {
	// A pick only survives while the player who made it is still sitting in
	// that seat. Rotation on a winner-stays result moves the loser and the
	// swap below keeps their pick with them; anything else -- a player leaving,
	// two players rejoining in the opposite order, a seat filled by someone new
	// -- leaves a pick describing a player who is no longer there, and the two
	// clients then disagree about who is playing what. That disagreement is a
	// desync at frame 60 with mismatched vitmax, which is exactly what it was.
	for (int side = 0; side < 2; side++) {
		bool occupied = (int)clients.size() > side && !clients.at(side).data.spectator;
		bool sameOwner = occupied && _charaOwner[side] == clients.at(side).data.connId;
		if (!sameOwner && _matchData.chara[side].charaID != 0) {
			memset(&_matchData.chara[side], 0, sizeof(_matchData.chara[side]));
			_matchData.readyMessageNum[side] = -1;
			_charaOwner[side] = SessionProtocol::ConnectionID();
			_dataDirty = true;
		}
	}
}

void SessionServer::MaybeExchangeDirectEndpoints() {
	// Both players, both opted in, or nothing happens. This is the gate that
	// keeps the promise: a player who left direct play off never has their
	// address handed to anyone, whatever the other side chose.
	if (_directExchanged || PlayerCount() < 2) {
		return;
	}
	if (clients.at(0).directPort == 0 || clients.at(1).directPort == 0) {
		return;
	}

	// A shared nonce both sides put in their punch packets, so a stray or
	// forged datagram cannot be mistaken for the peer answering.
	static const char HEX[] = "0123456789abcdef";
	std::random_device rd;
	char token[33] = { 0 };
	for (int i = 0; i < 32; i++) {
		token[i] = HEX[rd() % 16];
	}

	for (int side = 0; side < 2; side++) {
		const SessionMember& other = clients.at(1 - side);
		SessionProtocol::DirectPeer peer;
		peer.ip = other.directIp;
		peer.port = other.directPort;
		peer.localIp = other.directLocalIp;
		peer.localPort = other.directLocalPort;
		peer.token = token;
		// Respond(), not BroadcastMessage(): the spectators in this lobby
		// must not see either player's address.
		Respond(clients.at(side).conn, peer);
	}
	_directExchanged = true;
	// Tagged with the relay port, because that is the key the relay lines use
	// too. Without it a pairing could not be matched against whether that
	// relay later carried any traffic -- which is the only way to tell a
	// pairing that actually went direct from one that quietly fell back.
	spdlog::info("both players opted into direct play; endpoints exchanged (relay :{})", _relayPort);
}

std::vector<uint32_t> SessionServer::MemberIPv4s() const {
	std::vector<uint32_t> out;
	for (auto iter = clients.begin(); iter != clients.end(); iter++) {
		if (iter->peerIPv4 != 0) {
			out.push_back(iter->peerIPv4);
		}
	}
	return out;
}

std::vector<uint32_t> SessionServer::PlayerIPv4s() const {
	std::vector<uint32_t> out(2, 0);
	for (int i = 0; i < PlayerCount() && i < 2; i++) {
		out[i] = clients.at(i).peerIPv4;
	}
	return out;
}

void SessionServer::SetJoinSecret(const std::string& secret) {
	_joinSecret = secret;
}

void SessionServer::SetPublic(bool isPublic) {
	_lobbyData.isPublic = isPublic;
	_dataDirty = true;
}

void SessionServer::RequireJoinSecret(bool required) {
	_requireJoinSecret = required;
}

void SessionServer::DisconnectAll() {
	for (auto iter = _accepted.begin(); iter != _accepted.end(); iter++) {
		_interface->SetConnectionPollGroup(iter->first, k_HSteamNetPollGroup_Invalid);
		_interface->CloseConnection(iter->first, 0, "lobby closed", false);
	}
	_accepted.clear();
	cidMap.clear();
	if (!clients.empty()) {
		clients.clear();
		OnMembershipChanged();
	}
	_dataDirty = true;
}

int SessionServer::SpectatorCount() const {
	return (int)clients.size() - PlayerCount();
}

void SessionServer::SetSidecarHash(const std::string& hash) {
	_sidecarHash = hash;
}

void SessionServer::ResetLobby() {
	_matchData.Clear();
	_desyncReports = 0;
	for (auto iter = clients.begin(); iter != clients.end(); iter++) {
		iter->data.watching = false;
	}
	ResetBattleSync();
	_dataDirty = true;
}

int SessionServer::Step()
{
	ISteamNetworkingMessage* pIncomingMsgs[SESSION_SERVER_MAX_MESSAGES_PER_POLL] = { 0 };
	int numMsgs = _interface->ReceiveMessagesOnPollGroup(_pollGroup, pIncomingMsgs, SESSION_SERVER_MAX_MESSAGES_PER_POLL);
	bool bSendLobbyAllReady = false;
	bool bSendBattleSynced = false;

	if (numMsgs < 0) {
		spdlog::error("Session server error checking for messages: {}", numMsgs);
		return -1;
	}

	for (int i = 0; i < numMsgs; i++) {
		ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
		if (!pIncomingMsg) {
			spdlog::error("Client: incoming message enumerated, but not data retrieved");
			return -1;
		}

		HSteamNetConnection conn = pIncomingMsg->m_conn;
		const char* start = (const char*)pIncomingMsg->m_pData;
		json msg;

		try {
			msg = json::parse(start, start + pIncomingMsg->m_cbSize);
		}
		catch (json::exception e) {
			spdlog::info("Server: got a non-json message");
			continue;
		}

		SessionProtocol::MessageType type;
		try {
			msg.at("type").get_to(type);
		}
		catch (json::exception e) {
			spdlog::info("Server: got a message without a type, or a type that was not a string");
			continue;
		}

		if (cidMap.find(conn) == cidMap.end()) {
			if (type == SessionProtocol::MT_SESSION_HELLO) {
				SessionProtocol::SessionHelloResp cidMsg;
				SessionProtocol::ConnectionID newCid;
				cidMsg.cid.host = _identity;
				cidMsg.cid.user = std::to_string(conn);
				cidMap[conn] = cidMsg.cid;
				Respond(conn, json(cidMsg));
			}
			else {
				spdlog::warn("Server: got unrecognized message type: {}", (int)type);
			}
		}
		else {
			SessionProtocol::ConnectionID cid = cidMap[conn];
			if (type == SessionProtocol::MT_FORWARD) {
				SessionProtocol::ForwardMessage fwdMsg;
				try {
					msg.get_to(fwdMsg);
				}
				catch (json::exception e) {
					spdlog::debug("Server: could not deserialize forwarding message");
					continue;
				}

				// If this is a connection ID managed by this server, we can apply
				// additional security- messages with this source address should
				// only be coming from the connection that the address is assigned
				// to.
				if (fwdMsg.src.host == _identity) {
					if (fwdMsg.src.user != std::to_string(conn)) {
						spdlog::debug("Server: dropping fraudulent packet; {} masqueraded as {}", conn, fwdMsg.src.user);
						continue;
					}
				}

				if (fwdMsg.dest.host != _identity) {
					spdlog::info("Server: cannot forward to nonlocal identity {}@{}, clustering not yet implemented", fwdMsg.dest.user, fwdMsg.dest.host);
				}

				// Check that the destination is in fact the lobby
				auto clientIter = clients.begin();
				for (; clientIter != clients.end(); clientIter++) {
					if (clientIter->data.connId == fwdMsg.dest) {
						break;
					}
				}
				if (clientIter != clients.end()) {
					Respond(conn, msg);
				}
				else {
					spdlog::debug("Server: Could not forward to {}@{}: not in known lobby", fwdMsg.dest.user, fwdMsg.dest.host);
				}
			}
			else if (type == SessionProtocol::MT_SESSION_JOINREQ) {
				SessionProtocol::SessionJoinRequest request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize join request");
					SessionProtocol::SessionJoinReject reject;
					reject.result = SessionProtocol::JR_REQUEST_INVALID;
					json rejectMsg = reject;
					Respond(conn, rejectMsg);
					continue;
				}

				SteamNetworkingIPAddr peerAddr = *(pIncomingMsg->m_identityPeer.GetIPAddr());
				SessionProtocol::JoinResult joinResult = RegisterToWait(conn, request.port, request.sidecarHash, request.username, peerAddr, cid, request.spectator, request.secret);
				if (joinResult != SessionProtocol::JOIN_OK) {
					spdlog::info("Server: rejecting registration for reason {}", (int)joinResult);
					SessionProtocol::SessionJoinReject reject;
					reject.result = joinResult;
					json rejectMsg = reject;
					Respond(conn, rejectMsg);
					continue;
				}

				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETCHARA) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side == -1) {
					spdlog::info("Server: sender {} tried to set chara, but is not playing", conn);
					continue;
				}

				SessionProtocol::PreBattleSetChara request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize SetConditionsRequest");
					continue;
				}
				if (!SessionProtocol::CharaConditionsValid(request.chara)) {
					spdlog::warn("Server: sender {} sent out-of-range character conditions (chara {} costume {} color {} ultra {} handicap {}); ignored",
						conn, request.chara.charaID, request.chara.costume, request.chara.color, request.chara.ultraCombo, request.chara.handicap);
					continue;
				}
				_matchData.chara[side] = request.chara;
				_charaOwner[side] = clients.at(side).data.connId;
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_DESYNC_REPORT) {
				SessionProtocol::DesyncReport report;
				try {
					msg.get_to(report);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize DesyncReport");
					continue;
				}
				bool fromPlayer = false;
				for (int i = 0; i < PlayerCount(); i++) {
					if (clients.at(i).conn == conn) fromPlayer = true;
				}
				if (!fromPlayer || _desyncReports >= 20) {
					continue;
				}
				_desyncReports++;
				report.diff = report.diff.substr(0, 512);
				msg = report;
				// Recorded like any other statistic: no name, no address. The
				// point is to see the shape of desyncs across every player
				// rather than only the handful who send us a log.
				LogStat("desync", {
					{"frame", report.frame},
					{"gameplay", report.gameplay},
					{"flow_differs", report.flowDiffers},
					{"input_delay", report.inputDelay},
					{"direct", report.direct},
					{"ping_ms", report.pingMs},
					{"diff", report.diff},
				});
				spdlog::warn("desync reported at frame {} (delay {}, ping {} ms, {}): {}",
					report.frame, report.inputDelay, report.pingMs,
					report.direct ? "peer to peer" : "server relay",
					report.diff.substr(0, 200));

				// Tell the OTHER player too. Only the PC that noticed the fork
				// was ending its match; the other one kept running against a peer
				// that had already gone, which is the black screen people get
				// stuck on. A desync is a property of the match, not of the
				// machine that happened to detect it first, so both sides leave.
				for (auto& other : clients) {
					if (other.conn != conn && other.conn != k_HSteamNetConnection_Invalid) {
						Respond(other.conn, msg);
					}
				}
			}
			else if (type == SessionProtocol::MT_DIRECT_OFFER) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				// Spectators never take part in this: the direct path is only
				// ever between the two players.
				if (side == -1) {
					continue;
				}

				SessionProtocol::DirectOffer offer;
				try {
					msg.get_to(offer);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize DirectOffer");
					continue;
				}
				// ALWAYS re-pair on a fresh offer, changed endpoint or not.
				//
				// Only re-pairing when the address changed looked reasonable
				// and was wrong: after a match both clients discard their peer
				// info and re-arm, so they need a new pairing even though a
				// port-preserving NAT hands back the identical endpoint. The
				// server saw "nothing changed", stayed quiet, and every
				// rematch silently fell back to the relay. An offer only
				// arrives when a client has re-armed, so this is not chatty.
				_directExchanged = false;
				clients.at(side).directIp = offer.ip;
				clients.at(side).directPort = offer.port;
				clients.at(side).directLocalIp = offer.localIp;
				clients.at(side).directLocalPort = offer.localPort;
				MaybeExchangeDirectEndpoints();
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETENV) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side != 0) {
					spdlog::info("Server: sender {} tried to set env, but is not P1", conn);
					continue;
				}
				SessionProtocol::PreBattleSetEnv request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize SetConditionsRequest");
					continue;
				}

				_matchData.rngSeed = request.rngSeed;
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETSTAGE) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side != 0) {
					spdlog::info("Server: sender {} tried to set stage, but is not P1", conn);
					continue;
				}
				SessionProtocol::PreBattleSetStage request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize SetConditionsRequest");
					continue;
				}

				if (!SessionProtocol::StageIDValid(request.stageID)) {
					spdlog::warn("Server: sender {} sent out-of-range stage {}; ignored", conn, request.stageID);
					continue;
				}
				_matchData.stageID = request.stageID;
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_BATTLE_LOADED) {
				bSendBattleSynced = true;
				// Only the players gate the start. A spectator that is slow, or
				// never loads, is handled by GGPO on P1's side with a deadline.
				for (int i = 0; i < clients.size(); i++) {
					if (clients.at(i).conn == conn) {
						clients.at(i).data.flags |= SessionProtocol::MF_BATTLE_LOADED;
					}
					if (!clients.at(i).data.spectator) {
						bSendBattleSynced = bSendBattleSynced && (clients.at(i).data.flags & SessionProtocol::MF_BATTLE_LOADED);
					}
				}
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_LOBBY_READY) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side == -1) {
					spdlog::info("Server: sender {} tried to ready, but is not playing", conn);
					continue;
				}

				SessionProtocol::LobbyReady request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize ReportResultsRequest");
					continue;
				}
				if (!request.ready) {
					// Back to not ready, unless the match is already starting.
					if (!_matchData.IsAllReady()) {
						_matchData.readyMessageNum[side] = -1;
						_matchData.inputDelay[side] = -1;
						_readyRequest[side] = SessionProtocol::LobbyReady();
						_dataDirty = true;
					}
					continue;
				}
				_matchData.readyMessageNum[side] = pIncomingMsg->GetMessageNumber();
				_matchData.inputDelay[side] = request.inputDelay;
				_readyRequest[side] = request;
				if (_matchData.IsAllReady()) {
					int shared = SharedInputDelay();
					spdlog::info("Match delay {} frames (seats chose {} and {}, round trip {} ms)", shared,
						_readyRequest[0].inputDelay, _readyRequest[1].inputDelay, MeasuredRoundTripMs());
					_matchData.inputDelay[0] = _matchData.inputDelay[1] = shared;
				}
				bSendLobbyAllReady = bSendLobbyAllReady || _matchData.IsAllReady();
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_LOBBY_REPORTRESULTS) {
				SessionProtocol::LobbyReportResults request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize ReportResultsRequest");
					continue;
				}

				HandleResults(request.loserSide);
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_BATTLE_SNAPSHOT) {
				// Forward the snapshot to every other client. Spectators only
				// listen: their state must never end a players' match.
				bool fromSpectator = false;
				for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
					if (clientIter->conn == conn && clientIter->data.spectator) {
						fromSpectator = true;
					}
				}
				for (auto clientIter = clients.begin(); clientIter != clients.end() && !fromSpectator; clientIter++) {
					if (clientIter->conn != conn) {
						_interface->SendMessageToConnection(
							clientIter->conn, (const char*)pIncomingMsg->m_pData, pIncomingMsg->m_cbSize,
							k_nSteamNetworkingSend_Reliable, nullptr
						);
					}
				}
			}
			else {
				spdlog::warn("Server: got unrecognized message type: {}", (int)type);
			}
		}
	}

	for (int i = 0; i < numMsgs; i++) {
		ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
		if (pIncomingMsg) {
			pIncomingMsg->Release();
		}
	}

	// Any pick whose owner no longer occupies that seat is dropped before it
	// can reach a client.
	DropCharaFromVacatedSeats();

	// Cheap and guarded: pairs the two players as soon as both have offered,
	// whatever order the offers and the join happened to arrive in.
	MaybeExchangeDirectEndpoints();

	if (_dataDirty) {
		SessionProtocol::SessionDataUpdate updateMsg;
		updateMsg.lobbyData = _lobbyData;
		updateMsg.matchData = _matchData;
		updateMsg.lobbyData.members.clear();
		for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
			updateMsg.lobbyData.members.push_back(clientIter->data);
		}
		BroadcastMessage(json(updateMsg));
		_dataDirty = false;
	}

	if (bSendLobbyAllReady) {
		// The spectators in the room right now are the ones P1 will wait for.
		// Anyone joining later watches the next match.
		SessionProtocol::SessionDataUpdate updateMsg;
		updateMsg.lobbyData = _lobbyData;
		updateMsg.matchData = _matchData;
		updateMsg.lobbyData.members.clear();
		for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
			if (clientIter->data.spectator) {
				clientIter->data.watching = true;
			}
			updateMsg.lobbyData.members.push_back(clientIter->data);
		}
		BroadcastMessage(json(updateMsg));
		BroadcastMessage(json(SessionProtocol::LobbyAllReady()));

		// Both sides are ready, so a match is starting now.
		//
		// If the previous match never reported a result before this one began,
		// close it out here with its real length. Letting the new match simply
		// overwrite the clock is what produced the 0-second entries in the
		// stats: the old match's result would arrive a moment later and be
		// measured against the new match's start.
		if (_matchStartMs != 0) {
			LogStat("match_end", {
				{"seconds", (int)((NowMs() - _matchStartMs) / 1000)},
				{"spectators", SpectatorCount()},
				{"reported", false},
			});
		}
		_matchStartMs = NowMs();
		LogStat("match_start", {
			{"players", PlayerCount()},
			{"spectators", SpectatorCount()},
		});
	}

	if (bSendBattleSynced) {
		BroadcastMessage(json(SessionProtocol::BattleSynced()));
	}

	return 0;
}

void SessionServer::Respond(HSteamNetConnection client, const nlohmann::json& msg) {
	std::string buf = msg.dump();
	_interface->SendMessageToConnection(
		client, buf.c_str(), (uint32)buf.length(),
		k_nSteamNetworkingSend_Reliable, nullptr
	);
}

void SessionServer::BroadcastMessage(const nlohmann::json& msg) {
	// XXX (adanducci) replace SendMessageToConnection with SendMessages for
	// peformance gains, but ensuring low-copy with it is annoyingly difficult.
	std::string buf = msg.dump();
	for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
		_interface->SendMessageToConnection(
			clientIter->conn, buf.c_str(), (uint32)buf.length(),
			k_nSteamNetworkingSend_Reliable, nullptr
		);
	}
}

int SessionServer::Close()
{
	spdlog::info("Closing connections...");
	// Close all connections.  We use "linger mode" to ask SteamNetworkingSockets
	// to flush this out and close gracefully.
	for (auto iter = clients.begin(); iter != clients.end(); iter++) {
		_interface->SetConnectionPollGroup(iter->conn, k_HSteamNetPollGroup_Invalid);
		_interface->CloseConnection(iter->conn, 0, "Server Shutdown", true);
	}
	clients.clear();
	_interface->DestroyPollGroup(_pollGroup);
	_pollGroup = k_HSteamNetPollGroup_Invalid;
	if (_listenSock != k_HSteamListenSocket_Invalid) {
		s_byListenSocket.erase(_listenSock);
		_interface->CloseListenSocket(_listenSock);
		_listenSock = k_HSteamListenSocket_Invalid;
	}
	return 0;
}

void SessionServer::PrepareForCallbacks()
{
	s_pCallbackInstance = this;
}

void SessionServer::ResetBattleSync()
{
	for (int i = 0; i < clients.size(); i++) {
		clients.at(i).data.flags &= (~SessionProtocol::MF_BATTLE_LOADED);
	}
}

void SessionServer::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	switch (pInfo->m_info.m_eState)
	{
	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
	{
		// Ignore if they were not previously connected.  (If they disconnected
		// before we accepted the connection.)
		if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected) {
			// Select appropriate log messages
			const char* pszDebugLogAction;
			if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
			{
				pszDebugLogAction = "problem detected locally";
			}
			else
			{
				// Note that here we could check the reason code to see if
				// it was a "usual" connection or an "unusual" one.
				pszDebugLogAction = "closed by peer";
			}

			spdlog::debug("Connection {} {}, reason {}: {}",
				pInfo->m_info.m_szConnectionDescription,
				pszDebugLogAction,
				pInfo->m_info.m_eEndReason,
				pInfo->m_info.m_szEndDebug
			);

			for (auto iter = clients.begin(); iter != clients.end(); iter++) {
				if (iter->conn == pInfo->m_hConn) {
					clients.erase(iter);
					_dataDirty = true;
					// Someone left, so the remaining players may be reseated and
					// whoever rejoins can land on either side. The kept
					// characters are indexed by SIDE, so they now describe a
					// seating that no longer exists: keeping them made the two
					// clients disagree about who was playing what, which forked
					// the match at frame 60 with mismatched vitmax. Both players
					// re-send their character when they ready up, so dropping it
					// here costs nothing and is always correct.
					OnMembershipChanged();
					break;
				}
			}

		}

		// Close on every path, including a handshake that never completed:
		// the handle is not destroyed until we do.
		cidMap.erase(pInfo->m_hConn);
		_accepted.erase(pInfo->m_hConn);
		_interface->SetConnectionPollGroup(pInfo->m_hConn, k_HSteamNetPollGroup_Invalid);
		_interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
		break;
	}

	case k_ESteamNetworkingConnectionState_Connecting:
	{
		uint32_t ip = pInfo->m_info.m_addrRemote.IsIPv4() ? pInfo->m_info.m_addrRemote.GetIPv4() : 0;
		int fromSameIp = 0;
		for (auto iter = _accepted.begin(); iter != _accepted.end(); iter++) {
			if (iter->second == ip) fromSameIp++;
		}
		if (_accepted.size() >= 2 * MAX_SF4E_PROTOCOL_USERS || fromSameIp >= MAX_SF4E_PROTOCOL_USERS) {
			_interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
			spdlog::debug("Server: connection refused, lobby has {} pending/active connections", _accepted.size());
			break;
		}

		// Try to accept the connection.
		if (_interface->AcceptConnection(pInfo->m_hConn) != k_EResultOK)
		{
			// This could fail.  If the remote host tried to connect, but then
			// disconnected, the connection may already be half closed.  Just
			// destroy whatever we have on our side.
			_interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
			spdlog::error("Can't accept connection.  (It was already closed?)");
			break;
		}

		_interface->SetConnectionPollGroup(pInfo->m_hConn, _pollGroup);
		_accepted[pInfo->m_hConn] = ip;
	}

	default:
		break;
	}
}

void SessionServer::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	auto found = s_byListenSocket.find(pInfo->m_info.m_hListenSocket);
	if (found != s_byListenSocket.end()) {
		found->second->OnSteamNetConnectionStatusChanged(pInfo);
		return;
	}
	if (s_pCallbackInstance) {
		s_pCallbackInstance->OnSteamNetConnectionStatusChanged(pInfo);
	}
}

SessionProtocol::JoinResult SessionServer::RegisterToWait(
	const HSteamNetConnection& conn,
	const uint16_t& port,
	const std::string& sidecarHash,
	const std::string& rawName,
	const SteamNetworkingIPAddr& peerAddr,
	SessionProtocol::ConnectionID& cid,
	bool spectator,
	const std::string& secret
) {
	if (_requireJoinSecret && (_joinSecret.empty() || secret != _joinSecret)) {
		return SessionProtocol::JR_SECRET_INVALID;
	}
	if (!_sidecarHash.empty() && sidecarHash != _sidecarHash) {
		return SessionProtocol::JR_HASH_INVALID;
	}
	std::string name;
	for (size_t i = 0; i < rawName.size() && name.size() < 32; i++) {
		unsigned char ch = (unsigned char)rawName[i];
		if (ch >= 0x20 && ch != 0x7f) {
			name.push_back((char)ch);
		}
	}
	if (name.empty()) {
		name = "Player";
	}

	if (clients.size() >= MAX_SF4E_PROTOCOL_USERS) {
		return SessionProtocol::JR_LOBBY_FULL;
	}
	if (!spectator && PlayerCount() >= 2) {
		return SessionProtocol::JR_LOBBY_FULL;
	}
	if (spectator && SpectatorCount() >= MAX_SF4E_SPECTATORS) {
		return SessionProtocol::JR_LOBBY_FULL;
	}
	if (spectator && _spectatorRelayBase != 0 && SpectatorCount() >= _spectatorSlots) {
		return SessionProtocol::JR_LOBBY_FULL;
	}

	for (auto iter = clients.begin(); iter != clients.end(); iter++) {
		if (iter->data.name == name) {
			return SessionProtocol::JR_NAME_TAKEN;
		}
	}

	// An empty address means "the same host as this session server"; the
	// clients substitute the address they connected to. With a relay that is
	// exactly right: everyone reaches everyone else at the relay's port.
	char peerAddrStr[SteamNetworkingIPAddr::k_cchMaxString];
	uint16_t reportedPort = port;
	if (_relayPort != 0) {
		peerAddrStr[0] = 0;
		reportedPort = _relayPort;
	}
	else if (peerAddr.IsLocalHost()) {
		peerAddrStr[0] = 0;
	}
	else {
		peerAddr.ToString(peerAddrStr, SteamNetworkingIPAddr::k_cchMaxString, false);
	}
	SessionMember newMember;
	newMember.conn = conn;
	// Remembered privately (never broadcast) so the relay can reject anyone who
	// is not actually in this lobby. IsIPv4() covers the IPv4-mapped form GNS
	// hands back for a v4 peer.
	newMember.peerIPv4 = peerAddr.IsIPv4() ? peerAddr.GetIPv4() : 0;
	newMember.data.connId = cid;
	newMember.data.name = name;
	newMember.data.ip = peerAddrStr;
	newMember.data.port = reportedPort;
	newMember.data.flags = 0;
	newMember.data.spectator = spectator;
	if (spectator && _spectatorRelayBase != 0) {
		// Lowest free pipe. Slots stick to a member for as long as it stays,
		// so everyone reads the same ports whenever they look.
		int slot = 0;
		for (;; slot++) {
			bool taken = false;
			for (auto iter = clients.begin(); iter != clients.end(); iter++) {
				if (iter->spectatorSlot == slot) taken = true;
			}
			if (!taken) break;
		}
		newMember.spectatorSlot = slot;
		newMember.data.port = _spectatorRelayBase + 2 * slot;
		newMember.data.hostPort = _spectatorRelayBase + 2 * slot + 1;
	}
	if (spectator) {
		clients.push_back(std::move(newMember));
	}
	else {
		// Players stay in front of the spectators.
		clients.insert(clients.begin() + PlayerCount(), std::move(newMember));
		// A new player means the seating just changed, and the kept characters
		// are indexed by side. Two players who leave and rejoin in the opposite
		// order swap sides with no winner-stays rotation to account for it, so
		// the picks would describe the wrong player -- which is exactly how a
		// match forked at frame 60 with P2.vitmax 1050 on one machine and 1000
		// on the other. Both players re-send their pick when they ready up.
		OnMembershipChanged();
	}
	return SessionProtocol::JOIN_OK;
}

void SessionServer::LogStat(const std::string& event, const nlohmann::json& fields) {
	// Opt-in: no environment variable, no file, no collection.
	const char* path = std::getenv("SF4E_STATS_FILE");
	if (path == nullptr || path[0] == 0) {
		return;
	}
	json line = fields;
	line["event"] = event;
	line["ts"] = (int64_t)std::time(nullptr);
	const char* region = std::getenv("SF4E_REGION");
	if (region != nullptr && region[0] != 0) {
		line["region"] = region;
	}
	// Append-and-close rather than holding the file open: this is written a
	// couple of times per match, and it keeps the file safe to rotate or read
	// while the server runs.
	FILE* f = fopen(path, "a");
	if (f == nullptr) {
		return;
	}
	std::string s = line.dump();
	fwrite(s.c_str(), 1, s.size(), f);
	fputc('\n', f);
	fclose(f);
}

// Direct when both players proved a path to each other; otherwise the
// match goes through this server, one player's ping after the other's.
int SessionServer::MeasuredRoundTripMs() const {
	const SessionProtocol::LobbyReady& a = _readyRequest[0];
	const SessionProtocol::LobbyReady& b = _readyRequest[1];
	if (a.directRoundTripMs >= 0 && b.directRoundTripMs >= 0) {
		return a.directRoundTripMs > b.directRoundTripMs ? a.directRoundTripMs : b.directRoundTripMs;
	}
	if (a.serverPingMs >= 0 && b.serverPingMs >= 0) {
		return a.serverPingMs + b.serverPingMs;
	}
	return -1;
}

int SessionServer::SharedInputDelay() const {
	int automatic = SessionProtocol::InputDelayForRoundTrip(MeasuredRoundTripMs());
	int shared = 0;
	for (int side = 0; side < 2; side++) {
		int chosen = _readyRequest[side].inputDelay;
		if (chosen <= SessionProtocol::INPUT_DELAY_AUTO) {
			chosen = automatic;
		}
		if (chosen > SessionProtocol::INPUT_DELAY_MAX) {
			chosen = SessionProtocol::INPUT_DELAY_MAX;
		}
		if (chosen > shared) {
			shared = chosen;
		}
	}
	return shared;
}

void SessionServer::HandleResults(int loserIndex) {
	// A result arrived, so the match finished rather than being abandoned.
	// Duration tells us whether people are playing full sets or bouncing off
	// something after a few seconds, which is the whole point of collecting it.
	if (_matchStartMs != 0) {
		uint64_t elapsed = NowMs() - _matchStartMs;
		// A result arriving within seconds of this match STARTING belongs to
		// the previous match, delayed past the rematch: that one was closed out
		// when this match started, so logging it again would both double-count
		// it and destroy the running match's clock. No real match ends this
		// fast -- the shortest genuine one we have ever recorded was a
		// mid-match disconnect at five seconds.
		if (elapsed >= 3000) {
			LogStat("match_end", {
				{"seconds", (int)(elapsed / 1000)},
				{"spectators", SpectatorCount()},
				{"reported", true},
			});
			_matchStartMs = 0;
		}
	}

	// Winner stays as P1: the loser moves behind the other player, but
	// never behind the spectators.
	int nPlayers = PlayerCount();
	if (loserIndex >= 0 && loserIndex < nPlayers && nPlayers > 1) {
		SessionMember loser = clients.at(loserIndex);
		clients.erase(clients.begin() + loserIndex);
		clients.insert(clients.begin() + (nPlayers - 1), loser);

		// The kept characters below are indexed by SIDE, so a player who
		// changes seat has to take their character with them. Without this the
		// array still describes the previous seating and the two players end up
		// playing each other's character. Only a loser in seat 0 actually moves
		// anyone: a loser already in the last seat is reinserted where it was.
		if (nPlayers == 2 && loserIndex == 0) {
			std::swap(_matchData.chara[0], _matchData.chara[1]);
			std::swap(_charaOwner[0], _charaOwner[1]);
		}
	}
	for (auto iter = clients.begin(); iter != clients.end(); iter++) {
		iter->data.watching = false;
	}
	// Require both players to ready again for the next match, but KEEP the
	// characters, stage and seed. Clearing them here raced with a rematch: if
	// one player re-selected before the other's result message arrived, the
	// clear wiped the pick, producing an empty Ryu-vs-Ryu rematch with no stage
	// (seen after a draw). The rematch re-sends chara/stage/seed anyway, in
	// order before its ready, so keeping the old values is always safe.
	_matchData.readyMessageNum[0] = -1;
	_matchData.readyMessageNum[1] = -1;
}
