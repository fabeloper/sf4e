#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

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
				};

				static bool bHaltAfterNext;
				static bool bUpdateAllowed;
				static int nExtraFramesToSimulate;
				static int nNextBattleStartFlowTarget;
				static int nRandomizeLocalInputsEveryXFramesInGGPO;

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
					static void Save(SaveState* dst);
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
						std::vector<SaveState::KeyChecksum> keys;
						// Concatenated memento buffers followed by the raw global
						// data, only captured while dumps are still being written.
						std::vector<uint8_t> blob;
						std::vector<std::pair<uint32_t, uint32_t>> ranges;
					};

					bool bArmed = false;
					bool bActive = false;
					bool bDumpOnMismatch = true;
					int nCheckDistance = 1;
					int nFramesVerified = 0;
					int nMismatches = 0;
					// Mismatches in the raw byte checksum. Heap addresses move on
					// every save, so this is expected to be nonzero even when the
					// simulation is perfectly deterministic.
					int nRawMismatches = 0;
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
				static void SyncTestVerify(int frame, SaveState* state);

				// Saves the state, restores it immediately, and saves again. No
				// simulation runs in between, so the two saves must be identical.
				// If they aren't, the restore path is lossy, which separates a
				// broken save/load from a simulation that reads state we never
				// captured. Runs inside the battle update, not the render pass.
				static bool idempotenceCheckRequest;
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

				struct StateSnapshotMeta {
					bool sent;
					bool confirmed;
				};

				static void CaptureSnapshot(Dimps::Game::Battle::System* src);
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