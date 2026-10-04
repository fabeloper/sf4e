#pragma once

#include <cstdint>
#include <string>

#include <winsock2.h>

namespace sf4e {
	// Opportunistic UDP hole punching for the GGPO port.
	//
	// The relay is always there and always works, so this is a pure
	// optimisation: if a direct path opens, the match skips the server hop and
	// saves the detour (roughly a frame within Europe, far more across an
	// ocean). If anything at all goes wrong we fall back to the relay and the
	// players never notice.
	//
	// The socket binds the SAME port GGPO will later use, and is held open
	// from the moment the player opts in until the match starts. That is
	// deliberate: a NAT maps an internal port to an external one and keeps
	// that mapping alive while traffic flows, so holding the socket is what
	// makes the address we discovered still valid by the time we punch.
	//
	// This exposes each player's address to the other. It is on by default
	// and the lobby's CONNECTION option turns it off.
	struct HolePunch {
		// Binds `localPort`. False if the port is taken (then we just relay).
		bool Open(uint16_t localPort);
		bool IsOpen() const { return _sock != INVALID_SOCKET; }
		void Close();

		// Asks the server's matchmaker port what address this socket appears
		// to come from -- the classic STUN question, which the lobby server
		// can answer because it already sees the source of every datagram.
		// Blocking, with a short timeout; false if the server did not answer.
		// Asks the matchmaker first. If that gets no answer -- one player's
		// network drops exactly that combination -- asks the lobby's relay port,
		// which any player who can hold a match must already be able to reach.
		bool Discover(const sockaddr_in& matchmakerAddr, const sockaddr_in* relayAddr, int timeoutMs);

		// Ask a SECOND server the same question. A router that answers with a
		// different public port each time assigns one per destination, and the
		// port we discovered is then useless to our peer. See the implementation.
		void CheckNatType(const sockaddr_in& secondServer, int timeoutMs);
		bool natChecked = false;
		bool symmetricNat = false;

		// Filled in by Discover().
		std::string publicIp;
		uint16_t publicPort = 0;

		// This machine's address on its own network, learned from the socket
		// once it has a route to the server. Offered alongside the public one
		// so two players behind the same router can still find each other.
		std::string localIp;
		uint16_t localPort = 0;

		// Both sides send to each other at once; whoever's packet arrives
		// first opens the other's NAT for the reply. Returns true once a
		// packet from the peer is seen, meaning the path is open both ways.
		// `token` must match on both sides so a stray datagram cannot pass.
		// Punches at both candidates at once and returns the one that answered
		// in `chosenIp`/`chosenPort`. Whichever replies first wins; on a LAN that
		// is normally the local address, across the internet the public one.
		bool Punch(const std::string& peerIp, uint16_t peerPort,
			const std::string& peerLocalIp, uint16_t peerLocalPort,
			const std::string& token, int timeoutMs,
			std::string& chosenIp, uint16_t& chosenPort, int& rttMs, bool matchStart = false);

		// Keeps a proven path alive. A NAT forgets an idle UDP mapping in
		// anywhere from thirty seconds upward, and a lobby can sit far longer
		// than that between proving the path and the match starting -- so
		// without this the path would be proven and then quietly expire.
		// Cheap and non-blocking: one small packet every couple of seconds.
		void Keepalive(const std::string& peerIp, uint16_t peerPort, const std::string& token);

		~HolePunch();

	private:
		bool QueryMapping(const sockaddr_in& addr, int timeoutMs, std::string& outIp, uint16_t& outPort);
		// The relay port speaks a nine-byte probe rather than the matchmaker's
		// JSON; the reply is the same either way.
		bool _probeAsRelay = false;

		SOCKET _sock = INVALID_SOCKET;
		uint16_t _boundPort = 0;
	};
}
