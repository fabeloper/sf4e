#include <cstdio>
#include <string>
#include <utility>

#include <windows.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <ggponet.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"

#include "../sf4e/sf4e__Game__Battle__System.hxx"
#include "../sf4e/sf4e__UserApp.hxx"

#include "sf4e__SessionClient.hxx"
#include "sf4e__SessionProtocol.hxx"

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
using rSystem = Dimps::Game::Battle::System;
using fSystem = sf4e::Game::Battle::System;
using sf4e::SessionClient;
using sf4e::SessionProtocol::LobbyReady;

const int sf4e::SESSION_CLIENT_MAX_MESSAGES_PER_POLL = 20;

namespace {
	// A state divergence between the two machines, found by comparing periodic
	// snapshots. This once popped a modal box (a freeze that read as a crash),
	// then was changed to leave the match cleanly -- but that still threw both
	// players back to character select seconds in, over what is usually a
	// cosmetic float. `rootPos` is a derived RENDER position, not the
	// authoritative fixed-point one the engine fights with, and it drifts by
	// rounding without changing who wins; the idempotence check shows exactly
	// this, a recomputed 3D-vector cache near the end of the actor. GGPO itself
	// never aborts on divergence. So log the field diff for diagnosis and PLAY
	// ON. A future build fixes the underlying drift; until then a match is far
	// better finished than voided every few seconds.
	int g_desyncLogged = 0;
	// Leave the match. Called both by the PC that spotted the fork and by the
	// one the server relays it to, so the two always leave together.
	void LeaveBattle() {
		rSystem* system = rSystem::staticMethods.GetSingleton();
		if (system) {
			*rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
		}
	}

	void AbortForDesync() {
		SessionClient::bDesyncAbort = true;
		LeaveBattle();
	}

	void HandleDesync(int frame, const SessionProtocol::StateSnapshot& mine, const SessionProtocol::StateSnapshot& theirs) {
		bool gameplay = SessionProtocol::SnapshotGameplayDiffers(mine, theirs);
		bool flow = SessionProtocol::SnapshotFlowDiffers(mine, theirs);
		std::string diff = SessionProtocol::DescribeSnapshotDiff(mine, theirs);
		// A time scale that is not exactly 1 means that PC is not on the Fixed
		// frame-rate setting, which is the usual cause; say which side.
		auto offClock = [](const SessionProtocol::StateSnapshot& s) {
			return s.chara[0].timeScale.integral != 1 || s.chara[0].timeScale.fractional != 0;
		};
		if (offClock(mine)) diff += " [this PC is not on the Fixed frame-rate setting]";
		if (offClock(theirs)) diff += " [the other PC is not on the Fixed frame-rate setting]";

		// Rate-limited: the first several, then occasional, so a drifting
		// value cannot flood the log across a long match.
		if (gameplay || g_desyncLogged < 8 || (g_desyncLogged % 120) == 0) {
			// States the fact and stops there. This used to add "which is what
			// a mismatched frames-per-second setting looks like" -- but a
			// differing flow is CONSISTENT with that, not diagnostic of it, and
			// the one time it fired the cause was something else entirely.
			spdlog::warn("State divergence at frame {} ({}, {}): {}",
				frame,
				gameplay ? "GAMEPLAY forked - ending match" : "position drift only, continuing",
				flow ? "battle flow ALSO differs - the two PCs are at different points in the match" : "battle flow agrees - both PCs are at the same point in the match",
				diff);
		}
		g_desyncLogged++;

		// Tell the server, once per match. Whoever hit this may never send us
		// their log -- most players have no reason to, and the best report we
		// received came from someone we had no way to contact.
		if (gameplay) {
			SessionClient* c = SessionClient::Instance();
			if (c != nullptr) {
				c->ReportDesync(frame, gameplay, flow, diff);
			}
		}

		// Only an authoritative fork ends the match: health, meter, animation
		// or side actually disagree, so the two games can no longer be the same
		// match. A position-only drift is left alone -- ending on that threw
		// players out over a cosmetic float.
		if (gameplay) {
			AbortForDesync();
		}
	}
}

SessionClient* SessionClient::s_pCallbackInstance;
bool SessionClient::bVerboseLogging = false;
bool SessionClient::bDesyncAbort = false;
bool SessionClient::bConnectionLost = false;

SessionClient::SessionClient(
	const Callbacks& callbacks,
	std::string sidecarHash,
	uint16_t ggpoPort,
	std::string& name,
	bool spectator
):
	_callbacks(callbacks),
	_spectator(spectator),
	_sidecarHash(sidecarHash),
	_name(name),
	_ggpoPort(ggpoPort),
	_interface(SteamNetworkingSockets()),
	_conn(k_HSteamNetConnection_Invalid),
	_connected(false),
	_lobbyData(SessionProtocol::LobbyData::NULL_LOBBY)
{
	_serverAddr.Clear();
}

int SessionClient::Connect(HSteamNetConnection newConn) {
	_snapshotsEnabled = false;
	_serverAddr.SetIPv6LocalHost();
	_conn = newConn;
	_connected = true;
	_interface->SetConnectionUserData(newConn, (int64)this);

	// XXX (adanducci): It is absolutely critical to note that
	// `SetConfigValue`'s interface to set callbacks is _not_ the
	// same as the one used by `ConnectByIPAddress`/`SteamNetworkingConfigValue_t`.
	// 
	// Per the documentation for `SetConfigValue` and the header comment @
	// https://github.com/ValveSoftware/GameNetworkingSockets/blob/62b395172f157ca4f01eea3387d1131400f8d604/include/steam/isteamnetworkingutils.h#L296-L307 :
	//
	// NOTE: When setting pointers (e.g. callback functions), do not pass the function pointer
	// directly. Your argument should be a pointer to a function pointer.
	//
	// `ConnectByIPAddress`/`SteamNetworkingConfigValue_t` just takes the
	// function pointer directly. The failure mode if you pass the function
	// pointer directly is _extremely_ confusing- it just appears to be
	// a segfault in the GNS callback loop.
	void* callback = SteamNetConnectionStatusChangedCallback;
	SteamNetworkingUtils()->SetConfigValue(
		k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		k_ESteamNetworkingConfig_Connection,
		newConn,
		k_ESteamNetworkingConfig_Ptr,
		&callback
	);

	return 0;
}

int SessionClient::Connect(const SteamNetworkingIPAddr& serverAddr) {
	char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
	SteamNetworkingConfigValue_t opts[2];
	_serverAddr = serverAddr;
	_serverAddr.ToString(szAddr, sizeof(szAddr), true);
	spdlog::info("Connecting to session server at {}", szAddr);

	opts[0].SetInt64(
		k_ESteamNetworkingConfig_ConnectionUserData,
		(int64)this
	);
	opts[1].SetPtr(
		k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		(void*)SteamNetConnectionStatusChangedCallback
	);
	_snapshotsEnabled = true;
	_conn = _interface->ConnectByIPAddress(_serverAddr, 2, opts);
	if (_conn == k_HSteamNetConnection_Invalid) {
		spdlog::error("Client failed to create connection");
	}
	return 0;
}

void SessionClient::Disconnect() {
	if (_conn != k_HSteamNetConnection_Invalid) {
		_interface->CloseConnection(_conn, k_ESteamNetConnectionEnd_App_Generic, nullptr, true);
		_conn = k_HSteamNetConnection_Invalid;
		_connected = false;
		_lobbyData = SessionProtocol::LobbyData::NULL_LOBBY;
	}
}

SessionClient::~SessionClient()
{
	// Hand the mapping back. A router's table is finite, and an entry left
	// pointing at a machine that has gone can block the next person on that
	// network from mapping the same port -- a fault that would look like "peer
	// to peer stopped working for no reason" and be very hard to trace.
	if (_upnpMapped) {
		_upnp.DeleteMapping(_punch.localPort);
		_upnpMapped = false;
	}
	Disconnect();
	_interface = nullptr;
}

void SessionClient::PrepareForCallbacks()
{
	s_pCallbackInstance = this;
}

int SessionClient::Step()
{
	if (_interface == nullptr || _conn == k_HSteamNetConnection_Invalid) {
		return -1;
	}

	if (!_connected) {
		// Not yet connected- not an error state, but nothing to do.
		return 0;
	}

	ISteamNetworkingMessage* pIncomingMsgs[SESSION_CLIENT_MAX_MESSAGES_PER_POLL] = { 0 };
	int numMsgs = _interface->ReceiveMessagesOnConnection(_conn, pIncomingMsgs, SESSION_CLIENT_MAX_MESSAGES_PER_POLL);

	if (numMsgs < 0) {
		spdlog::error("Session client error checking for messages: {}", numMsgs);
		return -1;
	}

	for (int i = 0; i < numMsgs; i++) {
		ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
		if (!pIncomingMsg) {
			spdlog::error("Client: incoming message enumerated, but not data retrieved");
			return -1;
		}

		const char* start = (const char*)pIncomingMsg->m_pData;
		SteamNetworkingIPAddr peerAddr = *(pIncomingMsg->m_identityPeer.GetIPAddr());
		json msg;
		// This is attacker-controlled input: anything that can reach this
		// socket can put bytes here. An uncaught json::exception would call
		// std::terminate and kill the game outright -- no crash report, no
		// dump, nothing in the log. Drop the message instead. The server side
		// already guarded its equivalent; this one was missed.
		bool parsed = false;
		try {
			msg = json::parse(start, start + pIncomingMsg->m_cbSize);
			parsed = true;
		}
		catch (const json::exception&) {
			spdlog::warn("Client: dropped a malformed message");
		}
		pIncomingMsg->Release();
		if (!parsed) {
			continue;
		}

		SessionProtocol::MessageType type;
		try {
			msg.at("type").get_to(type);
		}
		catch (json::exception e) {
			spdlog::info("Client: got a message without a type, or a type that was not a string");
			continue;
		}

		if (type == SessionProtocol::MT_SESSION_HELLO_RESP) {
			SessionProtocol::SessionHelloResp cidMsg;
			try {
				msg.get_to(cidMsg);
			}
			catch (json::exception e) {
				spdlog::info("Client: couldn't deserialize CID?");
				continue;
			}
			
			_cid = cidMsg.cid;

			SessionProtocol::SessionJoinRequest request;
			request.sidecarHash = _sidecarHash;
			request.username = _name;
			request.port = _ggpoPort;
			request.spectator = _spectator;
			request.secret = joinSecret;
			json msg = request;
			if (Send(msg, nullptr) != k_EResultOK) {
				spdlog::warn("Client could send initial join request");
			}
		}
		else if (type == SessionProtocol::MT_SESSION_JOINREJ) {
			SessionProtocol::SessionJoinReject reject;
			try {
				msg.get_to(reject);
			}
			catch (json::exception e) {
				spdlog::info("Client: couldn't deserialize join rejection?");
				continue;
			}

			spdlog::info("Join rejected, reason: {}", (int)reject.result);
			ErrorType errType = ErrorType::SCE_UNKNOWN;
			switch (reject.result) {
			case SessionProtocol::JoinResult::JR_HASH_INVALID:
				errType = ErrorType::SCE_JOIN_REJECTED_HASH_INVALID;
				break;
			case SessionProtocol::JoinResult::JR_LOBBY_FULL:
				errType = ErrorType::SCE_JOIN_REJECTED_LOBBY_FULL;
				break;
			case SessionProtocol::JoinResult::JR_NAME_TAKEN:
				errType = ErrorType::SCE_JOIN_REJECTED_NAME_TAKEN;
				break;
			case SessionProtocol::JoinResult::JR_SECRET_INVALID:
			case SessionProtocol::JoinResult::JR_REQUEST_INVALID:
				errType = ErrorType::SCE_JOIN_REJECTED_REQUEST_INVALID;
				break;
			default:
				break;
			}
			_callbacks.OnError(errType, this, _callbacks);
			_interface->CloseConnection(_conn, 0, nullptr, false);
			_conn = k_HSteamNetConnection_Invalid;
			return -1;
		}
		else if (type == SessionProtocol::MT_SESSION_DATAUPDATE) {
			SessionProtocol::SessionDataUpdate update;
			try {
				msg.get_to(update);
			}
			catch (json::exception e) {
				spdlog::info("Client: could not deserialize response");
				continue;
			}
			_lobbyData = update.lobbyData;
			_matchData = update.matchData;
			for (int i = 0; i < 2; i++) {
				if (SessionProtocol::SanitizeCharaConditions(_matchData.chara[i])) {
					spdlog::warn("Client: out-of-range character conditions for side {} replaced with defaults", i);
				}
			}
			if (_matchData.stageID != -1 && !SessionProtocol::StageIDValid(_matchData.stageID)) {
				spdlog::warn("Client: out-of-range stage {} replaced with 0", _matchData.stageID);
				_matchData.stageID = 0;
			}

			if (_outstandingReadyRequestNumber > -1) {
				for (int i = 0; i < _lobbyData.members.size() && i < 2; i++) {
					if (_lobbyData.members[i].name == _name) {
						if (_matchData.readyMessageNum[i] == _outstandingReadyRequestNumber) {
							// This contains the ready data, so there's no longer an outstanding request.
							_outstandingReadyRequestNumber = -1;
						}
						break;
					}
				}
			}
		}
		else if (type == SessionProtocol::MT_LOBBY_ALLREADY) {
			_callbacks.OnReady(this, _callbacks);
		}
		else if (type == SessionProtocol::MT_BATTLE_SYNCED) {
			_callbacks.OnBattleSynced(this, _callbacks);
		}
		else if (type == SessionProtocol::MT_BATTLE_SNAPSHOT) {
			SessionProtocol::BattleSnapshot m;
			try {
				msg.get_to(m);
			}
			catch (json::exception e) {
				spdlog::info("Client: could not deserialize incoming checksum msg");
				continue;
			}

			auto localSnapshotIter = fSystem::snapshotMap.find(m.snapshot.frameIdx);
			if (localSnapshotIter != fSystem::snapshotMap.end()) {
				// This client is ahead and already has a snapshot for this frame.
				// Compare it.
				SessionProtocol::StateSnapshot& localSnapshot = localSnapshotIter->second.first;
				if (bVerboseLogging) {
					spdlog::error("Client: snapshot receipt: valid snapshot @ frame {} on receipt, confirm {}, sent {}", localSnapshot.frameIdx, localSnapshotIter->second.second.confirmed, localSnapshotIter->second.second.sent);
				}
				if (memcmp(&m.snapshot, &localSnapshot, sizeof(SessionProtocol::StateSnapshot)) != 0) {
					std::string diff = SessionProtocol::DescribeSnapshotDiff(localSnapshot, m.snapshot);
					if (_spectator) {
						spdlog::warn("Spectator: my view differs from the players at frame {}: {}", m.snapshot.frameIdx, diff);
					}
					else {
						HandleDesync(m.snapshot.frameIdx, localSnapshot, m.snapshot);
					}
				}

				if (bVerboseLogging) {
					spdlog::error("    - Client: snapshot receipt: valid snapshot @ frame {} on receipt confirmed", localSnapshot.frameIdx);
				}
				localSnapshotIter->second.second.confirmed = true;
				if (localSnapshotIter->second.second.confirmed && localSnapshotIter->second.second.sent) {

					if (bVerboseLogging) {
						spdlog::error("Client: snapshot receipt: erasing local snapshot @ frame {} on receipt due to confirmation+sent", m.snapshot.frameIdx);
					}
					fSystem::snapshotMap.erase(localSnapshotIter);
				}
			}
			else {
				// Opponent's ahead- can't compare yet.
				if (bVerboseLogging) {
					spdlog::error("Client: snapshot receipt: pendingRemoteSnapshots.emplace({})", m.snapshot.frameIdx);
				}
				// insert_or_assign, not emplace: emplace keeps the EXISTING entry
				// when the key is already present, so a leftover snapshot for this
				// frame could never be replaced by the current match's real one.
				// That is why the same stale opponent state was reported, field
				// for field, in two completely different matches.
				pendingRemoteSnapshots.insert_or_assign(m.snapshot.frameIdx, m.snapshot);
			}
		}
		else if (type == SessionProtocol::MT_DIRECT_PEER) {
			// The other player opted in too, and the server has told us where
			// to punch.
			//
			// Punch NOW, not at match start. The server sends both players
			// their endpoints in the SAME tick, so punching on receipt puts
			// the two windows within milliseconds of each other. Waiting for
			// match start made each side open its 900 ms window whenever it
			// personally got there -- different load times, different
			// hardware, a slower character select -- and two windows a second
			// apart never overlap, so neither side hears the other however
			// cooperative both routers are. Two players who both preserved
			// their port, and should certainly have connected, fell back to
			// the relay because of exactly that.
			SessionProtocol::DirectPeer peer;
			try {
				msg.get_to(peer);
			}
			catch (const json::exception&) {
				spdlog::info("Client: could not deserialize DirectPeer");
				continue;
			}
			if (!_directEnabled) {
				// We never asked for this. Ignore it rather than acting on an
				// address we did not solicit.
				continue;
			}
			_directPeerIp = peer.ip;
			_directPeerPort = peer.port;
			_directPeerLocalIp = peer.localIp;
			_directPeerLocalPort = peer.localPort;
			_directToken = peer.token;
			spdlog::info("Peer to peer: the other player has it on too; opening a path now");

			std::string chosenIp;
			uint16_t chosenPort = 0;
			int rttMs = -1;
			if (_punch.IsOpen() && _punch.Punch(_directPeerIp, _directPeerPort,
					_directPeerLocalIp, _directPeerLocalPort,
					_directToken, 900, chosenIp, chosenPort, rttMs)) {
				// Remember the address that actually answered; the match will
				// point GGPO straight at it.
				_provenIp = chosenIp;
				_provenPort = chosenPort;
				_directRttMs = rttMs;
				_punchProven = true;
				_lastKeepaliveTick = GetTickCount();
				spdlog::info("Peer to peer: path proven and held open until the match starts, {} ms round trip", _directRttMs);
			}
			else {
				_punchProven = false;
				spdlog::info("Peer to peer: no path could be opened; this lobby will use the server relay");
			}
		}
		else if (type == SessionProtocol::MT_DESYNC_REPORT) {
			// The other player hit a fork and is leaving. We may not have seen
			// it ourselves -- detection needs a snapshot from both sides at the
			// same frame, and whoever gets there first reports it. Leaving only
			// on our own detection left the other PC in a match with nobody in
			// it, staring at a black screen until it was killed by hand.
			if (!SessionClient::bDesyncAbort) {
				spdlog::warn("The other player's game detected a desync and left; ending this match too");
				AbortForDesync();
			}
		}
		else if (type == SessionProtocol::MT_FORWARD) {
			spdlog::debug("Received forwarded message: {}", msg.dump());
		}
		else {
			spdlog::warn("Client: got unrecognized message type: {}", (int)type);
		}
	}

	// Send all our outstanding local snapshots and compare any to pending
	// snapshots.
	if (_snapshotsEnabled) {
		// A snapshot may only be sent or compared once its frame can no longer
		// change. GGPO knows exactly when that is -- every player's input for
		// that frame has arrived -- so ask it, instead of assuming that being
		// 60 frames old is always far enough back to be settled. That
		// assumption was never checked, and comparing a frame before it settles
		// reports a desync between two states that were not meant to match yet.
		//
		// A depth rather than a frame number: GGPO counts its own frames from
		// the start of the session while these are the game's simulated-frame
		// numbers, so the two are not directly comparable -- but "how far back
		// is settled" is.
		int settledDepth = 60;
		if (fSystem::ggpo != nullptr) {
			int depth = 0;
			if (GGPO_SUCCEEDED(ggpo_get_unconfirmed_depth(fSystem::ggpo, &depth)) && depth > settledDepth) {
				// Inputs are running further behind than the old fixed window
				// allowed for -- a bad moment on the connection. Wait for them.
				settledDepth = depth;
			}
		}

		int mostRecentPredictiveFrame = (
			rSystem::GetNumFramesSimulated_FixedPoint(rSystem::staticMethods.GetSingleton())->integral
		);
		auto localSnapshotIter = fSystem::snapshotMap.begin();
		while (localSnapshotIter != fSystem::snapshotMap.end()) {
			if (mostRecentPredictiveFrame - localSnapshotIter->first < settledDepth) {
				// Snapshot not yet settled.
				localSnapshotIter++;
				continue;
			}

			if (bVerboseLogging) {
				spdlog::error("Client: snapshot reconciliation: checking snapshot @ {} due to mostRecentPredictiveFrame {}", localSnapshotIter->first, mostRecentPredictiveFrame);
			}

			if (_spectator) {
				// A spectator's state is nobody else's business.
				localSnapshotIter->second.second.sent = true;
			}
			if (!localSnapshotIter->second.second.sent) {
				if (bVerboseLogging) {
					spdlog::error("Client: snapshot reconciliation: snapshot @ {} not yet sent, confirmed val: {}", localSnapshotIter->first, localSnapshotIter->second.second.confirmed);
				}
				// Snapshot not yet sent. Send it.
				localSnapshotIter->second.second.sent = true;
				SessionProtocol::BattleSnapshot m;
				m.snapshot = localSnapshotIter->second.first;
				json msg = m;
				if (Send(msg, nullptr) != k_EResultOK) {
					spdlog::error("Client: Could not send snapshot update");
				}
			}

			if (!localSnapshotIter->second.second.confirmed) {
				if (bVerboseLogging) {
					spdlog::error("Client: snapshot reconciliation: snapshot @ {} not yet confirmed, sent val: {}", localSnapshotIter->first, localSnapshotIter->second.second.sent);
				}
				auto remoteSnapshotIter = pendingRemoteSnapshots.find(localSnapshotIter->first);
				if (remoteSnapshotIter != pendingRemoteSnapshots.end()) {
					if (bVerboseLogging) {
						spdlog::error("   - Client: snapshot reconciliation: snapshot @ {} got candidate remote snapshot", localSnapshotIter->first);
					}
					// Caught up to the opponent- compare to a snapshot already sent by the opponent.
					SessionProtocol::StateSnapshot& localSnapshot = localSnapshotIter->second.first;
					if (memcmp(&remoteSnapshotIter->second, &localSnapshot, sizeof(SessionProtocol::StateSnapshot)) != 0) {
						std::string diff = SessionProtocol::DescribeSnapshotDiff(localSnapshot, remoteSnapshotIter->second);
						if (_spectator) {
							spdlog::warn("Spectator: my view differs from the players at frame {} (pending): {}", localSnapshotIter->first, diff);
						}
						else {
							HandleDesync(localSnapshotIter->first, localSnapshot, remoteSnapshotIter->second);
						}
					}
					localSnapshotIter->second.second.confirmed = true;
					// Consumed: this frame is settled and will never be compared
					// again. Leaving it in the map is what let a snapshot outlive
					// its match.
					pendingRemoteSnapshots.erase(remoteSnapshotIter);
				}
			}

			if (localSnapshotIter->second.second.confirmed && localSnapshotIter->second.second.sent) {
				if (bVerboseLogging) {
					spdlog::error("   - Client: snapshot reconciliation: snapshot @ {} erased due to being confirmed and sent", localSnapshotIter->first);
				}
				localSnapshotIter = fSystem::snapshotMap.erase(localSnapshotIter);
			}
			else {
				localSnapshotIter++;
			}
		}
	}

	return 0;
}

void SessionClient::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
{

	switch (pInfo->m_info.m_eState)
	{
	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
	{
		// Print an appropriate message
		// NEVER pop a modal box here. MessageBoxA blocks the calling thread, so
		// a single network blip froze the whole game: still running, still
		// connected in Task Manager, but no rendering and no further log lines
		// until someone clicked OK. That is what made lost sessions look like
		// silent hangs, and it killed unattended runs outright (the process
		// never dies, so a keepalive watchdog cannot help). Log it and let the
		// lobby tell the player instead.
		if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting)
		{
			spdlog::error("Client could not connect ({}): {}",
				pInfo->m_info.m_eEndReason, pInfo->m_info.m_szEndDebug);
			SessionClient::bConnectionLost = true;
		}
		else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
		{
			// Reason 5003 is a plain timeout: we simply stopped hearing from
			// the server. Logging the numeric reason distinguishes that from a
			// refused connection or a bad address when reading a player's log.
			spdlog::error("Client lost contact with host ({}): {}",
				pInfo->m_info.m_eEndReason, pInfo->m_info.m_szEndDebug);
			SessionClient::bConnectionLost = true;
		}

		// Clean up the connection.  This is important!
		// The connection is "closed" in the network sense, but
		// it has not been destroyed.  We must close it on our end, too
		// to finish up.  The reason information do not matter in this case,
		// and we cannot linger because it's already closed on the other end,
		// so we just pass 0's.
		_interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
		_conn = k_HSteamNetConnection_Invalid;
		_connected = false;
		break;
	}
	case k_ESteamNetworkingConnectionState_Connected:
	{
		spdlog::info("Client connected to server OK, attempting to join...");
		_connected = true;
		SessionProtocol::SessionHelloMsg hello;
		json msg = hello;
		if (Send(msg, nullptr) != k_EResultOK) {
			spdlog::warn("Client could not send hello");
		}
		break;
	}
	default:
		break;
	}
}

EResult SessionClient::Send(nlohmann::json& msg, int64_t* outMessageNum) {
	std::string buf = msg.dump();
	return _interface->SendMessageToConnection(
		_conn, buf.c_str(), (uint32)buf.length(),
		k_nSteamNetworkingSend_Reliable, outMessageNum
	);
}

EResult SessionClient::Lobby_Ready(int inputDelay, int serverPingMs)
{
	LobbyReady msg;
	msg.inputDelay = inputDelay;
	msg.serverPingMs = serverPingMs;
	msg.directRoundTripMs = DirectRoundTripMs();
	json j = msg;
	EResult result = Send(j, &_outstandingReadyRequestNumber);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not send ready! Result: {}", (int)result);
	}
	return result;
}

EResult SessionClient::Lobby_Unready()
{
	LobbyReady msg;
	msg.ready = false;
	json j = msg;
	EResult result = Send(j, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not send not-ready! Result: {}", (int)result);
	}
	return result;
}

EResult SessionClient::Lobby_ReportResults(int loserSide)
{
	SessionProtocol::LobbyReportResults r;
	r.loserSide = loserSide;
	json msg = r;
	EResult result = Send(msg, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not report results! Result: {}", (int)result);
	}
	return result;
}

EResult SessionClient::PreBattle_SetEnv(uint32_t rngSeed)
{
	SessionProtocol::PreBattleSetEnv msg;
	msg.rngSeed = rngSeed;
	json j = msg;
	EResult result = Send(j, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not set prebattle environment! Result: {}", (int)result);
	}
	return result;
}

EResult SessionClient::PreBattle_SetChara(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& chara)
{
	SessionProtocol::PreBattleSetChara msg;
	msg.chara = chara;
	json j = msg;
	EResult result = Send(j, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not set prebattle character! Result: {}", (int)result);
	}
	return result;
}

EResult SessionClient::PreBattle_SetStage(int32_t stageID)
{
	SessionProtocol::PreBattleSetStage msg;
	msg.stageID = stageID;
	json j = msg;
	EResult result = Send(j, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not set prebattle stage! Result: {}", (int)result);
	}
	return result;
}

EResult SessionClient::Battle_Loaded()
{
	SessionProtocol::BattleLoaded msg;
	json j = msg;
	EResult result = Send(j, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not set battle loaded! Result: {}", (int)result);
	}
	return result;
}

bool SessionClient::EnableDirect(const SteamNetworkingIPAddr& matchmakerAddr,
	const SteamNetworkingIPAddr* altServer) {
	if (_directEnabled) {
		return true;
	}
	// A few spaced attempts, never one per frame.
	//
	// The lobby calls this every frame until it succeeds and the discovery
	// below BLOCKS, so retrying freely stalls the game thread for most of
	// every frame -- the lobby stops accepting input and looks hung. But one
	// single attempt is too few: the relay only answers players it already
	// knows are in the lobby, and the first attempt can land a fraction of a
	// second before the join completes. Spacing them fixes that without ever
	// making the lobby unresponsive.
	const DWORD RETRY_GAP_MS = 2000;
	const int MAX_ATTEMPTS = 4;
	if (_directAttempts >= MAX_ATTEMPTS) {
		return false;
	}
	if (_directAttempts > 0 && (int)(GetTickCount() - _directLastAttemptTick) < (int)RETRY_GAP_MS) {
		return false;
	}
	_directAttempts++;
	_directLastAttemptTick = GetTickCount();

	// The socket has to be the one GGPO will use, or the NAT would hand the
	// other player a mapping that stops existing the moment the match begins.
	if (!_punch.Open(_ggpoPort)) {
		return false;
	}

	sockaddr_in stun = { 0 };
	stun.sin_family = AF_INET;
	stun.sin_addr.s_addr = htonl(matchmakerAddr.GetIPv4());
	stun.sin_port = htons(matchmakerAddr.m_port);
	// Our own relay port for this lobby, as a fallback place to ask. The
	// server told us which port it assigned us when we joined, and it lives on
	// the same host as the session server.
	sockaddr_in relay = { 0 };
	bool haveRelay = false;
	for (size_t i = 0; i < _lobbyData.members.size(); i++) {
		if (_lobbyData.members[i].connId == _cid && _lobbyData.members[i].port != 0) {
			relay.sin_family = AF_INET;
			relay.sin_addr.s_addr = htonl(_serverAddr.GetIPv4());
			relay.sin_port = htons(_lobbyData.members[i].port);
			haveRelay = true;
			break;
		}
	}

	if (!haveRelay) {
		// Then the matchmaker is the only place we can ask. Worth saying: a
		// silent absence of the fallback looked exactly like the fallback
		// being sent and lost.
		spdlog::info("Peer to peer: no relay port known for us yet, so only the matchmaker can be asked");
	}

	// Short: this runs on the game thread, once, and the relay is right there.
	if (!_punch.Discover(stun, haveRelay ? &relay : nullptr, 400)) {
		_punch.Close();
		return false;
	}

	// Ask the router to open the game port before telling the other player
	// where to find us. Hole punching needs an unsolicited packet to be
	// accepted on the port we advertise, and plenty of home routers will not
	// do that unprompted but will when asked over UPnP -- which most of them
	// ship with switched on. Every player this rescues is one who would
	// otherwise have spent the match on the relay.
	if (!_upnpMapped && _upnp.Discover()) {
		_upnpMapped = _upnp.AddMapping(_punch.localPort, "sf4e netplay");
	}

	// One extra round trip, once per lobby, to learn whether this router can
	// ever do peer to peer. The punch still runs either way -- a router that
	// defeats the public candidate does not affect the local one, and two
	// players on the same network still connect directly.
	if (altServer == nullptr || altServer->IsIPv6AllZeros()) {
		spdlog::info("Peer to peer: no second server configured, so this router's NAT type "
			"was not tested. A failed punch cannot be attributed to either side.");
	}
	if (altServer != nullptr && !altServer->IsIPv6AllZeros()) {
		sockaddr_in alt = { 0 };
		alt.sin_family = AF_INET;
		alt.sin_addr.s_addr = htonl(altServer->GetIPv4());
		alt.sin_port = htons(altServer->m_port);
		_punch.CheckNatType(alt, 300);
	}

	SessionProtocol::DirectOffer offer;
	offer.ip = _punch.publicIp;
	offer.port = _punch.publicPort;
	offer.localIp = _punch.localIp;
	offer.localPort = _punch.localPort;
	json msg = offer;
	if (Send(msg, nullptr) != k_EResultOK) {
		_punch.Close();
		return false;
	}

	_directEnabled = true;
	spdlog::info("Peer to peer: enabled, waiting for the other player to do the same");
	return true;
}

void SessionClient::DisableDirect() {
	_directEnabled = false;
	_directAttempts = 0;
	_directPeerIp.clear();
	_directPeerPort = 0;
	_directPeerLocalIp.clear();
	_directPeerLocalPort = 0;
	_directToken.clear();
	// Releasing the port matters: GGPO takes it at match start, and a lingering
	// bind would push the whole match onto the relay for no reason.
	_punch.Close();
}


void SessionClient::PumpDirect() {
	if (!_punchProven || !_punch.IsOpen()) {
		return;
	}
	// Two seconds is comfortably inside the shortest NAT mapping timeouts
	// in the wild, and costs one small packet each way.
	if ((int)(GetTickCount() - _lastKeepaliveTick) < 2000) {
		return;
	}
	_lastKeepaliveTick = GetTickCount();
	_punch.Keepalive(_provenIp, _provenPort, _directToken);
}

bool SessionClient::TryDirectPath() {
	_matchIsDirect = false;

	// Prove the path NOW, together, instead of trusting what the last punch
	// said. After a rematch the last punch usually ran while the other PC was
	// still tearing down its match and port 23457 was held by its dying GGPO
	// session, so it failed -- and that stale failure became the decision for
	// the whole next match. Both sides reach this point at the same moment
	// (both readied), so both punch sockets are open and the probes cross: the
	// unproven side gets its last chance and the proven side reconfirms while
	// answering the other side for the full window. This also converges the
	// case where only one side had proven, which used to aim GGPO at two
	// different places.
	if (_directEnabled && _directPeerPort != 0 && _punch.IsOpen()) {
		std::string ip;
		uint16_t port = 0;
		int rttMs = -1;
		if (_punch.Punch(_directPeerIp, _directPeerPort, _directPeerLocalIp, _directPeerLocalPort,
				_directToken, 5000, ip, port, rttMs, true)) {
			_provenIp = ip;
			_provenPort = port;
			_directRttMs = rttMs;
			_punchProven = true;
			spdlog::info("Peer to peer: path proven at match start, {} ms round trip", _directRttMs);
		}
		else if (_punchProven) {
			_punchProven = false;
			spdlog::info("Peer to peer: the path proven earlier did not answer at match start; "
				"using the relay so both PCs agree");
		}
	}

	// The path was proven in the lobby, the moment both players were paired,
	// and kept alive since. Nothing to negotiate here -- just point GGPO at
	// the address that answered. Punching at this point instead was the bug:
	// each side opened its window when it personally reached match start, and
	// windows a second apart never meet.
	if (_punchProven && _provenPort != 0) {
		_directPeerIp = _provenIp;
		_directPeerPort = _provenPort;
		// GGPO needs the port, so the socket has to go.
		_punch.Close();
		_matchIsDirect = true;
		spdlog::info("Connection: PEER TO PEER for this match");
		// Re-armed for the next match: the socket is gone, so the path has to
		// be proven again once the server pairs us for the rematch.
		_punchProven = false;
		_directEnabled = false;
		_directAttempts = 0;
		return true;
	}

	if (!_directEnabled) {
		spdlog::info("Connection: server relay (peer to peer is off on this PC)");
	}
	else if (_directPeerPort == 0) {
		spdlog::info("Connection: server relay (the other player has peer to peer off)");
	}
	else {
		spdlog::info("Connection: server relay (no peer-to-peer path could be opened; "
			"normal behind a strict NAT, and when both PCs share one router)");
	}

	_punch.Close();
	_directEnabled = false;
	_directAttempts = 0;
	return false;
}


void SessionClient::ReportDesync(int frame, bool gameplay, bool flowDiffers, const std::string& diff) {
	if (_desyncReported || !_connected) {
		return;
	}
	_desyncReported = true;

	SessionProtocol::DesyncReport r;
	r.frame = frame;
	r.gameplay = gameplay;
	r.flowDiffers = flowDiffers;
	r.direct = _matchIsDirect;

	// The conditions matter as much as the fields: a fork at input delay 0 on
	// a 90 ms link is a very different event from one at delay 3 on a LAN,
	// because delay 0 means every frame is predicted and re-simulated.
	if (sf4e::UserApp::netplay != nullptr) {
		r.inputDelay = (int)sf4e::UserApp::netplay->delay;
	}
	if (fSystem::ggpo != nullptr && fSystem::localPlayerHandle != 0) {
		GGPONetworkStats stats;
		for (int i = 0; i < MAX_SF4E_PROTOCOL_USERS; i++) {
			if (fSystem::players[i].type == GGPO_PLAYERTYPE_REMOTE
				&& GGPO_SUCCEEDED(ggpo_get_network_stats(fSystem::ggpo, fSystem::players[i].handle, &stats))) {
				r.pingMs = stats.network.ping;
				break;
			}
		}
	}

	// Truncated: the full diff can run to a couple of thousand characters and
	// the first fields are the informative ones.
	r.diff = diff.size() > 700 ? diff.substr(0, 700) : diff;

	json msg = r;
	Send(msg, nullptr);
	spdlog::info("Reported this desync to the server (no name or address is sent)");
}

EResult SessionClient::Forward(const SessionProtocol::ConnectionID& dest, const json& fwd) {
	SessionProtocol::ForwardMessage msg;
	msg.dest = dest;
	msg.src = _cid;
	msg.msg = fwd;
	json j = msg;
	EResult result = Send(j, nullptr);
	if (result != k_EResultOK) {
		spdlog::warn("Client: could not forward! Result: {}", (int)result);
	}
	return result;
}

void SessionClient::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	SessionClient* instance = (SessionClient *)SteamNetworkingSockets()->GetConnectionUserData(pInfo->m_hConn);
	instance->OnSteamNetConnectionStatusChanged(pInfo);
}
