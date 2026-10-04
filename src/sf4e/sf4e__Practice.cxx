#include <cstdint>
#include <cstring>

#include <windows.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"

#include "sf4e__GameEvents.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__Practice.hxx"

using Dimps::App;
using Dimps::Event::EventBaseWithEC;
using Dimps::Game::ProgressData;
using Dimps::GameEvents::RootEvent;
using rMainMenu = Dimps::GameEvents::MainMenu;
using rVsMode = Dimps::GameEvents::VsMode;
using rSystem = Dimps::Game::Battle::System;
using rPadSystem = Dimps::Pad::System;
using CharaActor = Dimps::Game::Battle::Chara::Actor;
using CharaUnit = Dimps::Game::Battle::Chara::Unit;
using fVsBattle = sf4e::GameEvents::VsBattle;
using fVsPreBattle = sf4e::GameEvents::VsPreBattle;
using fPadSystem = sf4e::Pad::System;

namespace {
	// Where a fighter keeps its gauges, as 16.16 fixed point. Read from the
	// engine's own getters (Chara::Actor::Get*_FixedPoint).
	const size_t ACTOR_VITALITY = 0x6c20;
	const size_t ACTOR_VITALITY_MAX = 0x6c24;
	const size_t ACTOR_RECOVERABLE_VITALITY = 0x6c30;
	const size_t ACTOR_SUPER = 0x6c38;
	const size_t ACTOR_SUPER_MAX = 0x6c3c;
	const size_t ACTOR_REVENGE = 0x6c4c;
	const size_t ACTOR_REVENGE_MAX = 0x6c50;
	// GameManager::GetRoundTime reads this.
	const size_t MANAGER_ROUND_TIME = 0x458;

	const int ROUND_SECONDS = 99;
	const int REFILL_AFTER_QUIET_FRAMES = 60;
	// Low enough to be reached only mid-combo; refilled at once so no hit
	// can end the round.
	const int VITALITY_FLOOR_PERCENT = 35;

	bool g_active = false;
	sf4e::Practice::Setup g_setup;
	uint32_t g_lastVitality[2] = { 0, 0 };
	int g_quietFrames[2] = { 0, 0 };

	uint32_t& Fixed(void* base, size_t offset) {
		return *(uint32_t*)((char*)base + offset);
	}

	void ApplyCharactersAndStage() {
		char* query[1] = { "VSMode" };
		rVsMode* mode = (rVsMode*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), query, 1);
		if (!mode) {
			spdlog::error("Practice: the versus mode is not in the foreground");
			return;
		}
		const rVsMode::ConfirmedCharaConditions* chosen[2] = { &g_setup.player, &g_setup.dummy };
		rVsMode::ConfirmedPlayerConditions* conditions = rVsMode::GetConfirmedPlayerConditions(mode);
		for (int side = 0; side < 2; side++) {
			*rVsMode::ConfirmedPlayerConditions::GetCharaID(&conditions[side]) = chosen[side]->charaID;
			*rVsMode::ConfirmedPlayerConditions::GetSideActive(&conditions[side]) = 1;
			memcpy(rVsMode::ConfirmedPlayerConditions::GetCharaConditions(&conditions[side]), chosen[side], sizeof(*chosen[side]));
		}
		Dimps::Platform::dString* stageName = rVsMode::GetStageName(mode);
		(stageName->*Dimps::Platform::dString::publicMethods.assign)(Dimps::stageCodes[g_setup.stageId], 4);
		*rVsMode::GetStageCode(mode) = g_setup.stageId;
	}

	void GivePlayerThePad() {
		rPadSystem* pad = rPadSystem::staticMethods.GetSingleton();
		rPadSystem::__publicMethods& methods = rPadSystem::publicMethods;
		(pad->*methods.AssociatePlayerAndGamepad)(sf4e::Practice::PLAYER_SIDE, g_setup.deviceIdx);
		(pad->*methods.SetDeviceTypeForPlayer)(sf4e::Practice::PLAYER_SIDE, g_setup.deviceType);
		(pad->*methods.SetSideHasAssignedController)(sf4e::Practice::PLAYER_SIDE, 1);
		(pad->*methods.SetActiveButtonMapping)(rPadSystem::BUTTON_MAPPING_FIGHT);
	}

	void RefillVitality(CharaActor* actor, int side) {
		uint32_t& vitality = Fixed(actor, ACTOR_VITALITY);
		const uint32_t max = Fixed(actor, ACTOR_VITALITY_MAX);
		g_quietFrames[side] = vitality < g_lastVitality[side] ? 0 : g_quietFrames[side] + 1;

		const bool belowFloor = (uint64_t)vitality * 100 < (uint64_t)max * VITALITY_FLOOR_PERCENT;
		if (belowFloor || g_quietFrames[side] >= REFILL_AFTER_QUIET_FRAMES) {
			vitality = max;
			Fixed(actor, ACTOR_RECOVERABLE_VITALITY) = max;
		}
		g_lastVitality[side] = vitality;
	}

	void FillGauges(CharaActor* actor) {
		Fixed(actor, ACTOR_SUPER) = Fixed(actor, ACTOR_SUPER_MAX);
		Fixed(actor, ACTOR_REVENGE) = Fixed(actor, ACTOR_REVENGE_MAX);
	}
}

bool sf4e::Practice::Start(const Setup& setup) {
	RootEvent* root = App::GetRootEvent();
	char* query[1] = { "MainMenu" };
	rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(root, query, 1);
	if (!mainMenu) {
		return false;
	}

	g_setup = setup;
	g_active = true;
	g_lastVitality[0] = g_lastVitality[1] = 0;
	g_quietFrames[0] = g_quietFrames[1] = 0;
	fPadSystem::mutedSide = DUMMY_SIDE;

	ProgressData* progress = *RootEvent::GetProgressData(root);
	ProgressData::BattleTypeSettings* settings = &ProgressData::GetBattleTypeSettings(progress)[ProgressData::NBT_PVP];
	*ProgressData::GetNextBattleType(progress) = ProgressData::NBT_PVP;
	settings->rounds = 1;
	settings->timeLimit = { 0, (short)ROUND_SECONDS };
	settings->editionSelect = TRUE;

	fVsPreBattle::bSkipToVersus = true;
	fVsPreBattle::OnTasksRegistered = ApplyCharactersAndStage;
	fVsBattle::OnTasksRegistered = GivePlayerThePad;
	// However the battle ends, go back to the main menu, not the game's own
	// rematch screens.
	fVsBattle::bTerminateOnNextLeftBattle = true;

	spdlog::info("Practice: starting a practice match");
	(rMainMenu::ToItemObserver(mainMenu)->*rMainMenu::itemObserverMethods.GoToVersusMode)();
	return true;
}

void sf4e::Practice::Stop() {
	rSystem* system = rSystem::staticMethods.GetSingleton();
	if (!g_active || !system) {
		return;
	}
	spdlog::info("Practice: leaving the practice match");
	fVsBattle::bTerminateOnNextLeftBattle = true;
	*rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
}

bool sf4e::Practice::IsActive() {
	return g_active;
}

void sf4e::Practice::OnBattleTick(rSystem* system) {
	CharaUnit* charas = (CharaUnit*)(system->*rSystem::publicMethods.GetUnitByIndex)(rSystem::U_CHARA);
	for (int side = 0; side < 2; side++) {
		CharaActor* actor = (charas->*CharaUnit::publicMethods.GetActorByIndex)(side);
		if (!actor) {
			continue;
		}
		RefillVitality(actor, side);
		FillGauges(actor);
	}
	Fixed((system->*rSystem::publicMethods.GetGameManager)(), MANAGER_ROUND_TIME) = (uint32_t)ROUND_SECONDS << 16;
}

void sf4e::Practice::OnBattleClosed() {
	if (!g_active) {
		return;
	}
	g_active = false;
	fPadSystem::mutedSide = -1;
	spdlog::info("Practice: practice match closed");
}
