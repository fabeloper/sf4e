#include <cstring>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

// Not exposed by every Windows SDK header set, but the control code itself is
// stable and documented. Define it if we did not get it.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "sf4e__HolePunch.hxx"

using nlohmann::json;
using sf4e::HolePunch;

namespace {
	// Prefix on every punch datagram. GGPO validates its own header, so the
	// handful of these that can still be in flight when GGPO takes the port
	// are discarded rather than confusing it.
	const char PUNCH_MAGIC[] = "SF4EPUNCH1";
	const size_t PUNCH_MAGIC_LEN = sizeof(PUNCH_MAGIC) - 1;

	bool WouldBlock() {
		return WSAGetLastError() == WSAEWOULDBLOCK;
	}
}

bool HolePunch::Open(uint16_t localPort_) {
	Close();

	_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_sock == INVALID_SOCKET) {
		spdlog::warn("Peer to peer: could not create a socket; staying on the server relay");
		return false;
	}

	sockaddr_in local = { 0 };
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port = htons(localPort_);
	if (bind(_sock, (sockaddr*)&local, sizeof(local)) != 0) {
		// Almost always "GGPO already owns this port", which means we were
		// called at the wrong time. The relay path is unaffected.
		spdlog::warn("Peer to peer: port {} is busy; staying on the server relay", localPort_);
		Close();
		return false;
	}

	_boundPort = localPort_;
	u_long nonblocking = 1;
	ioctlsocket(_sock, FIONBIO, &nonblocking);

	// Windows answers a UDP send to a port nobody is listening on with an ICMP
	// port-unreachable, and then fails the NEXT recvfrom on this socket with
	// WSAECONNRESET -- an error about a datagram we already sent, reported
	// against a read that has nothing to do with it. We punch two candidates
	// at once and the other side opens its socket a moment after we do, so
	// the first probe to a not-yet-listening peer is normal and expected. On
	// a LAN that rejection comes back in well under a millisecond, which is
	// how a 900 ms punch window used to die in 78. Turn the behaviour off.
	BOOL noConnReset = FALSE;
	DWORD unused = 0;
	WSAIoctl(_sock, SIO_UDP_CONNRESET, &noConnReset, sizeof(noConnReset),
		nullptr, 0, &unused, nullptr, nullptr);
	return true;
}

void HolePunch::Close() {
	if (_sock != INVALID_SOCKET) {
		closesocket(_sock);
		_sock = INVALID_SOCKET;
	}
}

HolePunch::~HolePunch() {
	Close();
}

bool HolePunch::QueryMapping(const sockaddr_in& matchmakerAddr, int timeoutMs,
	std::string& outIp, uint16_t& outPort) {
	if (_sock == INVALID_SOCKET) {
		return false;
	}

	const std::string request = _probeAsRelay
		? std::string("SF4ESTUN1")
		: json{ {"type", "stun"} }.dump();
	DWORD deadline = GetTickCount() + timeoutMs;
	DWORD nextSend = 0;

	while ((int)(GetTickCount() - deadline) < 0) {
		// Resend a few times over the window: this is the one request with no
		// retransmit above it, and a single dropped datagram would needlessly
		// cost the match its direct path.
		if ((int)(GetTickCount() - nextSend) >= 0) {
			sendto(_sock, request.c_str(), (int)request.size(), 0,
				(const sockaddr*)&matchmakerAddr, sizeof(matchmakerAddr));
			nextSend = GetTickCount() + 120;
		}

		char buf[1500];
		sockaddr_in from = { 0 };
		int fromLen = sizeof(from);
		int n = recvfrom(_sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
		if (n <= 0) {
			// A failed read is about one datagram, never about the punch. One
			// candidate being unreachable says nothing about the other, and a
			// peer that is not listening yet will be listening in 50 ms. Keep
			// probing until the deadline instead of giving up on the first
			// refusal -- the deadline is what ends this loop.
			Sleep(WouldBlock() ? 5 : 1);
			continue;
		}
		// Only the server we asked can answer this.
		if (from.sin_addr.s_addr != matchmakerAddr.sin_addr.s_addr
			|| from.sin_port != matchmakerAddr.sin_port) {
			continue;
		}

		buf[n] = 0;
		try {
			json reply = json::parse(buf);
			if (!reply.value("ok", false)) {
				return false;
			}
			outIp = reply.at("ip").get<std::string>();
			outPort = reply.at("port").get<uint16_t>();
		}
		catch (const json::exception&) {
			return false;
		}
		return outPort != 0;
	}
	return false;
}

bool HolePunch::Discover(const sockaddr_in& matchmakerAddr, const sockaddr_in* relayAddr, int timeoutMs) {
	// The matchmaker port first: it is the one every client already talks to,
	// and it answers for almost everyone.
	_probeAsRelay = false;
	bool got = QueryMapping(matchmakerAddr, timeoutMs, publicIp, publicPort);

	if (!got && relayAddr != nullptr) {
		// It did not answer. For at least one player that request never
		// arrived at all, while his match traffic to the relay port went
		// through from the very same socket -- so ask there instead. Any
		// player able to hold a match can reach this port by definition.
		spdlog::info("Peer to peer: the matchmaker did not answer; asking the relay port instead");
		_probeAsRelay = true;
		got = QueryMapping(*relayAddr, timeoutMs, publicIp, publicPort);
		_probeAsRelay = false;
	}

	if (!got) {
		spdlog::info("Peer to peer: no address could be discovered; staying on the server relay");
		return false;
	}

	// Our own LAN address. The punch socket is bound to INADDR_ANY, so asking
	// it directly just returns 0.0.0.0 -- it has no single local address until
	// it is connected. A throwaway socket connected to the server answers the
	// real question ("which of this machine's addresses would be used to reach
	// the outside world?") without sending a byte; connect() on UDP only sets
	// the default destination.
	{
		SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (probe != INVALID_SOCKET) {
			if (connect(probe, (const sockaddr*)&matchmakerAddr, sizeof(matchmakerAddr)) == 0) {
				sockaddr_in me = { 0 };
				int meLen = sizeof(me);
				if (getsockname(probe, (sockaddr*)&me, &meLen) == 0 && me.sin_addr.s_addr != 0) {
					char localBuf[INET_ADDRSTRLEN] = { 0 };
					inet_ntop(AF_INET, &me.sin_addr, localBuf, sizeof(localBuf));
					localIp = localBuf;
					// The port is ours, not the probe's: the peer will be
					// reaching the punch socket, which is bound to it.
					localPort = _boundPort;
				}
			}
			closesocket(probe);
		}
	}

	// The address itself is not logged: it is the player's own public IP.
	spdlog::info("Peer to peer: our public mapping was discovered");
	return true;
}

void HolePunch::CheckNatType(const sockaddr_in& secondServer, int timeoutMs) {
	// The classic question: does this router give us the SAME public port no
	// matter who we talk to?
	//
	// Ask a second server from the same socket. If the port we are told differs
	// from the first answer, the router assigns a new external port per
	// destination -- a symmetric NAT. Everything about hole punching depends on
	// the port we discovered being the port our peer will actually see, so on
	// such a router the public candidate cannot work: we would be punching at a
	// port that only ever existed for the conversation with the server.
	//
	// Knowing this does not fix it -- nothing on our side can -- but it turns
	// "peer to peer did not work and we don't know why" into a plain answer,
	// and the relay handles it exactly as it always has.
	std::string ip2;
	uint16_t port2 = 0;
	if (!QueryMapping(secondServer, timeoutMs, ip2, port2)) {
		// No answer is not evidence either way; leave the verdict unknown.
		return;
	}
	natChecked = true;
	symmetricNat = (port2 != publicPort);
	if (symmetricNat) {
		spdlog::info("Peer to peer: this router gives a different public port per destination "
			"(symmetric NAT). A direct connection over the internet is very unlikely; "
			"the match will use the server relay. This is a property of your router, not a fault.");
	}
	else {
		// Say so explicitly. Logging only the bad case made silence ambiguous:
		// it could mean "your router is fine" or "the check never ran", and those
		// call for completely different advice when a punch fails.
		spdlog::info("Peer to peer: this router keeps the same public port across destinations "
			"(cone NAT) - our side can hole punch. If a direct path still fails, the "
			"restriction is at the other end.");
	}
}


bool HolePunch::Punch(const std::string& peerIp, uint16_t peerPort,
	const std::string& peerLocalIp, uint16_t peerLocalPort,
	const std::string& token, int timeoutMs,
	std::string& chosenIp, uint16_t& chosenPort, int& rttMs, bool matchStart) {
	if (_sock == INVALID_SOCKET) {
		return false;
	}

	// Try every address the other side offered, at the same time. Two players
	// behind one router usually cannot reach each other at their shared public
	// address (that needs hairpin NAT), but the LAN address always works;
	// across the internet it is the other way round. Sending to both costs one
	// extra datagram every 50 ms and removes the guesswork entirely.
	struct Candidate {
		sockaddr_in addr;
		bool valid;
		const char* what;
	};
	Candidate candidates[2] = { 0 };
	const std::string ips[2] = { peerLocalIp, peerIp };
	const uint16_t ports[2] = { peerLocalPort, peerPort };
	const char* names[2] = { "local", "public" };
	int numCandidates = 0;
	for (int i = 0; i < 2; i++) {
		if (ips[i].empty() || ports[i] == 0) {
			continue;
		}
		Candidate& c = candidates[numCandidates];
		c.addr.sin_family = AF_INET;
		c.addr.sin_port = htons(ports[i]);
		if (inet_pton(AF_INET, ips[i].c_str(), &c.addr.sin_addr) != 1) {
			continue;
		}
		c.valid = true;
		c.what = names[i];
		numCandidates++;
	}
	if (numCandidates == 0) {
		return false;
	}

	// Two kinds of packet, and the distinction is the whole point.
	//
	// PROBE says "I can reach you". ACK says "I heard your probe". Declaring
	// success on a bare probe only proves THEIR packets reach US -- it says
	// nothing about the other direction. With an asymmetric firewall each side
	// then reaches a different conclusion: one goes direct while the other uses
	// the relay, they aim their traffic at different places, and the match is
	// broken before it starts. That really happened -- one machine logged
	// "path open" while the other logged "no reply" for the same match.
	//
	// Requiring an ACK to OUR OWN probe proves the round trip, so both sides
	// agree or both fall back.
	// At match start the probe carries an extra mark. A keepalive from the
	// results screen answers plain probes too, so a plain exchange proves
	// reachability but not that the other PC has reached match start; only
	// a marked probe does. Both sides reach match start whenever each finishes
	// loading, seconds apart at worst, so the early side waits here for the
	// late one instead of handing the port to GGPO on its own proof.
	std::string probe = std::string(PUNCH_MAGIC) + (matchStart ? "M" : "") + token;
	std::string plainProbe = PUNCH_MAGIC + token;
	std::string matchProbe = std::string(PUNCH_MAGIC) + "M" + token;
	std::string ack = std::string(PUNCH_MAGIC) + "A" + token;

	DWORD deadline = GetTickCount() + timeoutMs;
	DWORD nextSend = 0;
	DWORD lastProbeAt = 0;
	rttMs = -1;
	int sent = 0;
	bool gotAck = false;    // they answered one of our probes
	bool ackedPeer = false; // we answered one of theirs (a marked one at match start)
	const char* which = nullptr;
	// Set once the path is proven: a short stay to answer a retransmit, and,
	// in the lobby, long enough for a peer that has not started yet.
	DWORD settle = 0;

	while ((int)(GetTickCount() - deadline) < 0) {
		if (settle != 0 && (int)(GetTickCount() - settle) >= 0) {
			break;
		}
		if ((int)(GetTickCount() - nextSend) >= 0) {
			for (int i = 0; i < numCandidates; i++) {
				sendto(_sock, probe.c_str(), (int)probe.size(), 0,
					(const sockaddr*)&candidates[i].addr, sizeof(candidates[i].addr));
			}
			nextSend = GetTickCount() + 50;
			lastProbeAt = GetTickCount();
			sent++;
		}

		char buf[1500];
		sockaddr_in from = { 0 };
		int fromLen = sizeof(from);
		int n = recvfrom(_sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
		if (n <= 0) {
			if (!WouldBlock()) {
				return false;
			}
			Sleep(5);
			continue;
		}

		const bool isPlain = (size_t)n == plainProbe.size() && memcmp(buf, plainProbe.c_str(), plainProbe.size()) == 0;
		const bool isMarked = (size_t)n == matchProbe.size() && memcmp(buf, matchProbe.c_str(), matchProbe.size()) == 0;
		if (isPlain || isMarked) {
			// Their probe: answer it so they can prove their own round trip.
			sendto(_sock, ack.c_str(), (int)ack.size(), 0, (const sockaddr*)&from, sizeof(from));
			if (isMarked || !matchStart) {
				if (!ackedPeer && settle != 0) {
					// First answer they get from us: a retransmit, then go.
					settle = GetTickCount() + 150;
				}
				ackedPeer = true;
			}
		}
		else if ((size_t)n == ack.size() && memcmp(buf, ack.c_str(), ack.size()) == 0) {
			// Their ack: our probes arrive and so do their replies. Only an
			// address the peer offered may become the match endpoint; the
			// port may differ from what the server observed, the host not.
			for (int i = 0; i < numCandidates; i++) {
				if (from.sin_addr.s_addr == candidates[i].addr.sin_addr.s_addr) {
					which = candidates[i].what;
				}
			}
			if (which == nullptr) {
				continue;
			}
			char fromIp[INET_ADDRSTRLEN] = { 0 };
			inet_ntop(AF_INET, &from.sin_addr, fromIp, sizeof(fromIp));
			chosenIp = fromIp;
			chosenPort = ntohs(from.sin_port);

			if(!gotAck) {
				rttMs = (int)(GetTickCount() - lastProbeAt);
			}

			gotAck = true;
		}
		else {
			// A stray datagram, someone spraying the port: ignored.
			continue;
		}

		if (settle == 0 && gotAck && (ackedPeer || !matchStart)) {
			settle = GetTickCount() + (ackedPeer ? 150 : 1200);
		}
	}

	if (gotAck && (ackedPeer || !matchStart)) {
		spdlog::info("Peer to peer: round trip proven via the {} address after {} rounds", which, sent);
		return true;
	}
	if (gotAck) {
		spdlog::info("Peer to peer: the other PC answered but never reached match start within {} ms; using the server relay", timeoutMs);
	}
	else {
		spdlog::info("Peer to peer: no round trip from {} candidate(s) after {} rounds; using the server relay",
			numCandidates, sent);
	}
	return false;
}

void HolePunch::Keepalive(const std::string& peerIp, uint16_t peerPort, const std::string& token) {
	if (_sock == INVALID_SOCKET || peerPort == 0 || peerIp.empty()) {
		return;
	}
	sockaddr_in peer = { 0 };
	peer.sin_family = AF_INET;
	peer.sin_port = htons(peerPort);
	if (inet_pton(AF_INET, peerIp.c_str(), &peer.sin_addr) != 1) {
		return;
	}
	std::string probe = PUNCH_MAGIC + token;
	sendto(_sock, probe.c_str(), (int)probe.size(), 0, (const sockaddr*)&peer, sizeof(peer));

	// Drain whatever they sent us, and answer their probes so their own
	// mapping stays open too. Never blocks: whatever is queued, nothing more.
	std::string ack = std::string(PUNCH_MAGIC) + "A" + token;
	for (int i = 0; i < 8; i++) {
		char buf[1500];
		sockaddr_in from = { 0 };
		int fromLen = sizeof(from);
		int n = recvfrom(_sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
		if (n <= 0) {
			break;
		}
		if ((size_t)n == probe.size() && memcmp(buf, probe.c_str(), probe.size()) == 0) {
			sendto(_sock, ack.c_str(), (int)ack.size(), 0, (const sockaddr*)&from, sizeof(from));
		}
	}
}
