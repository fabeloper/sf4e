#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sf4e__Pacing.hxx"

#include <GameNetworkingSockets/steam/steamnetworkingtypes.h>
#include <ggponet.h>

#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Math.hxx"

#include "../session/sf4e__SessionProtocol.hxx"

#include "sf4e__Platform.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__Hud.hxx"

#define NUM_SAVE_STATES (GGPO_MAX_PREDICTION_FRAMES + 2)

namespace sf4e {
	namespace Game {
		namespace Battle {
			using Dimps::Game::GameMementoKey;
			using Dimps::Math::FixedPoint;

			struct System : Dimps::Game::Battle::System
			{
				typedef struct AdditionalMemento {
					int nFirstCharaToSimulate;
					DWORD skipRelatedFlags_0xd8c;
					DWORD simulationFlags;
					FixedPoint transitionProgress;
					FixedPoint transitionSpeed;
					int transitionType;

					Dimps::Game::Battle::Network::Unit network;
					Hud::Announce::Unit::AdditionalMemento announce;
					Hud::Notice::Player::AdditionalMemento playerNotices[2];
					Platform::GFxApp::AdditionalMemento gfxApp;
					Eva::TaskCore::AdditionalMemento updateCore;
				} AdditionalMemento;

				struct PlayerConnectionInfo {
					GGPOPlayerType       type;
					GGPOPlayerHandle     handle;

					// Connection quality accumulated over the current logging
					// window. Sampling one frame in six hundred hid the spikes
					// players were watching on screen the whole time.
					int pingMin = 0x7fffffff;
					int pingMax = 0;
					long long pingSum = 0;
					int pingSamples = 0;
					int pingOver100 = 0;
					int remoteBehindMax = 0;
					// Outages are what players call freezes, and no ping figure can
					// show them: during one there is nothing to measure.
					int outages = 0;
					unsigned long outageMsTotal = 0;
					void ResetStatsWindow() {
						pingMin = 0x7fffffff; pingMax = 0; pingSum = 0;
						pingSamples = 0; pingOver100 = 0; remoteBehindMax = 0;
						outages = 0; outageMsTotal = 0;
					}
				};

				static bool bHaltAfterNext;
				static bool bUpdateAllowed;
				// While set, Start does not open the game's pause menu.
				static std::atomic<bool> bNativePauseBlocked;
				static int nExtraFramesToSimulate;

				// GGPO broke one of its own invariants and the match was torn
				// down because of it. Read by the lobby to tell the player why.
				static bool bGgpoAssertAbort;
				// Frames GGPO asked us to wait so the opponent catches up.
				static int nFramesToSkip;
				static int nNextBattleStartFlowTarget;
				static int nRandomizeLocalInputsEveryXFramesInGGPO;

				// Hold the simulation at round transitions until inputs confirm, so
				// no rollback can span a round reset. See the implementation.
				static bool bRoundCheckpoint;
				static int nCheckpointHeldFrames;

				static bool extendedLoadRequest;
				static bool extendedSaveRequest;
				static Dimps::Game::GameMementoKey::MementoID mementoLoadRequest;
				static Dimps::Game::GameMementoKey::MementoID mementoSaveRequest;

				static void Install();
				static void RestoreAllFromInternalMementos(Dimps::Game::Battle::System* system, GameMementoKey::MementoID* id);
				static void RecordAllToInternalMementos(Dimps::Game::Battle::System* system, GameMementoKey::MementoID* id);

				int GetMementoSize();
				int RecordToMemento(Memento* memento, GameMementoKey::MementoID* id);
				int RestoreFromMemento(Memento* memento, GameMementoKey::MementoID* id);

				void BattleUpdate();
				void CloseBattle();
				static void OnBattleFlow_BattleStart(System* s);
				void SysMain_HandleTrainingModeFeatures();
				void SysMain_UpdatePauseState();

				struct SaveState {
					bool used = false;
					// False while the records point at payloads the engine owns: a scratch
					// copy around a release, or a slot reclaimed after teardown. Clear then
					// drops the records without calling into the engine.
					bool ownsKeys = true;
					std::vector<std::pair<GameMementoKey*, GameMementoKey>> keys;
					std::map<
						Dimps::Game::Battle::Sound::SoundPlayerManager::CriPlayerAdapter*,
						Sound::SoundPlayerManager::DeferredSoundRequest
					> criPlayerState;
					std::map<
						Dimps::Game::Battle::Sound::SoundPlayerManager*,
						Platform::SoundObjectPool<4>::SaveState
					> managerState;

					struct GlobalData {
						DWORD CurrentBattleFlow = 0;
						DWORD PreviousBattleFlow = 0;
						DWORD CurrentBattleFlowSubstate = 0;
						DWORD PreviousBattleFlowSubstate = 0;
						FixedPoint CurrentBattleFlowFrame = { 0, 0 };
						FixedPoint CurrentBattleFlowSubstateFrame = { 0, 0 };
						FixedPoint PreviousBattleFlowFrame = { 0, 0 };
						FixedPoint PreviousBattleFlowSubstateFrame = { 0, 0 };
						void (*BattleFlowSubstateCallable_aa9258)(Dimps::Game::Battle::System * s) = nullptr;
						void (*BattleFlowCallback_CallEveryFrame_aa9254)(Dimps::Game::Battle::System * s) = nullptr;

						Dimps::Game::Battle::GameManager gameManager = { 0 };
					};
					GlobalData d;

					// Per-key checksums, in `keys` order, plus a checksum of the
					// non-memento global data, folded into a single value that is
					// handed to GGPO. Sound state is intentionally excluded for now:
					// it holds live adapter pointers and handles.
					struct KeyChecksum {
						GameMementoKey* key;
						void* mementoable;
						uint32_t size;
						uint32_t checksum;    // pointer-normalized; compare this
						uint32_t checksumRaw; // raw bytes; diagnostic only
					};
					std::vector<KeyChecksum> keyChecksums;
					uint32_t globalChecksum = 0;
					uint32_t checksum = 0;
					uint32_t checksumRaw = 0;

					SaveState();

					static void Free(SaveState* dst);
					static void Reclaim(SaveState* victim, const char* reason, int slotIndex);
					static void Save(SaveState* dst);
					static void SaveWithChecksum(SaveState* dst);
					static void Load(SaveState* src);
					static void ComputeChecksum(SaveState* s);
				};

				// Offline rollback verification built on GGPO's synctest backend.
				// The match runs with both players local; every `nCheckDistance`
				// frames GGPO loads the last verified state, re-simulates, and the
				// checksum of every re-saved frame is compared with the checksum
				// recorded on the original pass. Any mismatch is state that the
				// save/load path does not capture (or non-determinism in the sim).
				struct SyncTest {
					struct FrameRecord {
						uint32_t checksum;
						uint32_t checksumRaw;
						uint32_t globalChecksum;
						SessionProtocol::StateSnapshot snapshot;
						std::vector<SaveState::KeyChecksum> keys;
						// Concatenated memento buffers followed by the raw global
						// data, only captured while dumps are still being written.
						std::vector<uint8_t> blob;
						std::vector<std::pair<uint32_t, uint32_t>> ranges;
					};

					bool bArmed = false;
					bool bActive = false;
					bool bDumpOnMismatch = true;
					// Unattended soak: both sides fed random inputs, a fresh random
					// pairing every match, and re-armed when a match ends. Left
					// running it builds the volume of evidence a single hand-played
					// match cannot.
					bool bSoak = false;
					int nMatchesRun = 0;
					int nTotalFramesVerified = 0;
					int nTotalGameplayMismatches = 0;
					int nCheckDistance = 1;
					int nFramesVerified = 0;
					int nMismatches = 0;
					// Mismatches in the raw byte checksum. Heap addresses move on
					// every save, so this is expected to be nonzero even when the
					// simulation is perfectly deterministic.
					int nRawMismatches = 0;
					// Mismatches in observable gameplay state. This is the number
					// that decides whether rollback is playable: a save state can
					// differ in engine bookkeeping without the match diverging,
					// but if this is nonzero the two sides are playing different
					// games.
					int nGameplayMismatches = 0;
					int nLastGameplayMismatchFrame = -1;
					int nLastMismatchFrame = -1;
					// Dumps are ~2MB per frame per side. Without a cap a bad run
					// fills the disk.
					int nMaxDumps = 3;
					int nDumpsWritten = 0;
					std::string lastMismatchSummary;
					std::string lastDumpPath;
					std::map<int, FrameRecord> records;
				};
				static SyncTest syncTest;
				static void ArmSyncTest(int checkDistance);
				static void DisarmSyncTest();
				static void StartSyncTest();
				static void SyncTestPickRandomCharacters();
				static void SyncTestVerify(int frame, SaveState* state);

				// Saves the state, restores it immediately, and saves again. No
				// simulation runs in between, so the two saves must be identical.
				// If they aren't, the restore path is lossy, which separates a
				// broken save/load from a simulation that reads state we never
				// captured. Runs inside the battle update, not the render pass.
				static bool idempotenceCheckRequest;
				// Run the round-trip check every N battle frames (SF4E_IDEM_EVERY),
				// offline only. 0 disables.
				// Set when a sync-test soak match ends: the battle has closed and
				// the game is heading back to the menu, so something on the menu
				// side has to start the next one.
				static bool bSoakRestartPending;
				// Frames simulated with a session live but outside GGPO, because the
				// flow was BF__IDLE. These cannot be rolled back.
				static int nUntrackedIdleFrames;
				// Run GGPO even while the battle flow is idle (SF4E_GGPO_IDLE), so
				// round transitions land inside the rollback timeline.
				static bool bGgpoDuringIdle;
				// Set once a match has advanced past the pre-battle idle period, so
				// later idle frames can be told apart from "no battle yet".
				static bool bMatchLeftIdle;
				// Idle frames that ran inside the rollback timeline this match. Must
				// agree between two machines or their timelines differ in length.
				static int nIdleFramesInTimeline;
				static bool bSoakCharasPreset;
				// Time-sync pacing: samples GGPO's frame advantage every tick and
				// asks the frame limiter for a few milliseconds more or less.
				static sf4e::Pacing::Controller pacer;
				static bool bPredictionStalled;
				// Set once a match has run without the frame limiter: the game's
				// frames-per-second setting is not Fixed. Shown in the lobby.
				static bool bFrameRateSettingWrong;
				static void StepPacing();
				static void ResetPacing(const char* label);
				// Characters the sync test may pick (SF4E_SYNCTEST_CHARAS); empty = all.
				static std::vector<int> soakCharaPool;
				static int PickSoakChara();
				static int nIdemEveryFrames;
				static int nIdemCounter;
				static void RunIdempotenceCheck();

				// RestoreAllFromInternalMementos calls Chara::Actor's
				// ResetAfterMemento on both actors, which recomputes derived
				// state. If that recomputation doesn't reproduce what was
				// saved, a restored actor differs from one that was never
				// rolled back. Set to skip those calls for a controlled
				// comparison.
				static bool bSkipResetAfterMemento;

				// The System restores first, which includes the Scaleform action
				// pool, and every other unit restores on top of it. One of those
				// later restores drives Scaleform and perturbs the pool after it
				// was set, which measurably broke the round trip. Deferring the
				// pool until every other unit has finished fixes it, so that is
				// the default; the flag remains for A/B measurement.
				static bool bRestoreGfxLast;

				// True only while GGPO is re-simulating rolled-back frames.
				// The soak-test flow logger reports it, so we can see whether a
				// round/match transition was decided during a rollback -- the
				// suspected trigger for the two machines taking different
				// branches at a round end.
				static bool bInRollback;

				struct StateSnapshotMeta {
					bool sent;
					bool confirmed;
				};

				static void CaptureSnapshot(Dimps::Game::Battle::System* src);

				// Fills a snapshot of the state players actually observe:
				// positions, status, health, meters and damage. Everything else
				// in a save state is engine bookkeeping, so this is what has to
				// match for a rollback to be correct in practice.
				static void BuildSnapshot(
					Dimps::Game::Battle::System* src,
					SessionProtocol::StateSnapshot& out
				);
				static std::map<int, std::pair<SessionProtocol::StateSnapshot, StateSnapshotMeta>> snapshotMap;
				static GGPOPlayerHandle localPlayerHandle;
				static PlayerConnectionInfo players[MAX_SF4E_PROTOCOL_USERS];
				static GGPOSession* ggpo;
				static SaveState saveStates[NUM_SAVE_STATES];

				static void StartGGPO(GGPOPlayer* players, int numPlayers, int port, int frameDelay, DWORD rngSeed);
				static void StartSpectating(unsigned short localport, int num_players, char* host_ip, unsigned short host_port, DWORD rngSeed);
				static bool ggpo_on_event_callback(GGPOEvent* info);
				static bool ggpo_begin_game_callback(const char*);
				static bool ggpo_advance_frame_callback(int);
				static bool ggpo_load_game_state_callback(unsigned char*, int);
				static bool ggpo_save_game_state_callback(unsigned char** buffer, int* len, int* checksum, int);
				static void ggpo_free_buffer(void* buffer);
				static bool ggpo_log_game_state(char* filename, unsigned char* buffer, int);
			};
		}
	}
}
