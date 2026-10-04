#pragma once

#include <windows.h>

#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"

namespace sf4e {
	// A local match against a motionless opponent, for practising combos
	// while waiting in a lobby: life comes back after each combo, both gauges
	// stay full and the round timer never runs down.
	//
	// Everything here runs on the game thread.
	namespace Practice {
		struct Setup {
			Dimps::GameEvents::VsMode::ConfirmedCharaConditions player;
			Dimps::GameEvents::VsMode::ConfirmedCharaConditions dummy;
			int stageId;
			BYTE deviceType;
			BYTE deviceIdx;
		};

		const int PLAYER_SIDE = 0;
		const int DUMMY_SIDE = 1;

		// False when the game is not on the main menu.
		bool Start(const Setup& setup);
		void Stop();
		bool IsActive();

		void OnBattleTick(Dimps::Game::Battle::System* system);
		void OnBattleClosed();
	}
}
