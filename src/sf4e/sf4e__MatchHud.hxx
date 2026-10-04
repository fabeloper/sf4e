#pragma once

#include <string>

namespace sf4e {
	// What an online match shows on top of the game's own HUD: both players'
	// names under the health bars, and the connection under the timer.
	//
	// The game thread reports the match; the render thread draws it. Each side
	// only ever touches a copy taken under a lock.
	namespace MatchHud {
		void BeginMatch(const std::string& p1Name, const std::string& p2Name, int inputDelay, bool direct);
		void Tick(bool fighting, int pingMs, bool waitingForOpponent);
		void EndMatch();

		void Draw();
		void DrawTuningWindow(bool* open);
	}
}
