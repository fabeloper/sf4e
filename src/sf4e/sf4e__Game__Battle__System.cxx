#include <algorithm>
#include <fstream>
#include <sstream>
#include <string.h>
#include <utility>
#include <vector>

#include <windows.h>
#include <shlobj.h>
#include <detours/detours.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <ggponet.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__Camera.hxx"
#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__Command.hxx"
#include "../Dimps/Dimps__Game__Battle__Effect.hxx"
#include "../Dimps/Dimps__Game__Battle__Hud.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Game__Battle__Training.hxx"
#include "../Dimps/Dimps__Game__Battle__Vfx.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"

#include "../session/sf4e__SessionProtocol.hxx"

#include "sf4e.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__Hud.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__Platform.hxx"
#include "sf4e__Rtti.hxx"

using Dimps::Platform::WithReleaser;

namespace rHud = Dimps::Game::Battle::Hud;
using CameraUnit = Dimps::Game::Battle::Camera::Unit;
using CharaActor = Dimps::Game::Battle::Chara::Actor;
using CharaUnit = Dimps::Game::Battle::Chara::Unit;
using CommandUnit = Dimps::Game::Battle::Command::Unit;
using EffectUnit = Dimps::Game::Battle::Effect::Unit;
using GameManager = Dimps::Game::Battle::GameManager;
using HudUnit = Dimps::Game::Battle::Hud::Unit;
using NetworkUnit = Dimps::Game::Battle::Network::Unit;
using rSoundPlayerManager = Dimps::Game::Battle::Sound::SoundPlayerManager;
using rSystem = Dimps::Game::Battle::System;
using PauseUnit = Dimps::Game::Battle::Pause::Unit;
using TrainingManager = Dimps::Game::Battle::Training::Manager;
using VfxUnit = Dimps::Game::Battle::Vfx::Unit;
using rKey = Dimps::Game::GameMementoKey;
using FixedPoint = Dimps::Math::FixedPoint;
using fKey = sf4e::Game::GameMementoKey;
using rPadSystem = Dimps::Pad::System;
using fPadSystem = sf4e::Pad::System;
using StateSnapshot = sf4e::SessionProtocol::StateSnapshot;

namespace fHud = sf4e::Game::Battle::Hud;
using fSoundPlayerManager = sf4e::Game::Battle::Sound::SoundPlayerManager;
using fSystem = sf4e::Game::Battle::System;
using fVsBattle = sf4e::GameEvents::VsBattle;

bool fSystem::bHaltAfterNext = false;
bool fSystem::bUpdateAllowed = true;
int fSystem::nExtraFramesToSimulate = 0;
int fSystem::nFramesToSkip = 0;
int fSystem::nNextBattleStartFlowTarget = -1;
int fSystem::nRandomizeLocalInputsEveryXFramesInGGPO = 0;

GGPOPlayerHandle fSystem::localPlayerHandle = GGPO_INVALID_HANDLE;
GGPOSession* fSystem::ggpo = nullptr;
fSystem::PlayerConnectionInfo fSystem::players[MAX_SF4E_PROTOCOL_USERS];
fSystem::SaveState fSystem::saveStates[NUM_SAVE_STATES];
fSystem::SyncTest fSystem::syncTest;

rKey::MementoID GGPO_MEMENTO_ID = { 1, 1 };

bool fSystem::extendedLoadRequest = false;
bool fSystem::extendedSaveRequest = false;
bool fSystem::idempotenceCheckRequest = false;
bool fSystem::bSkipResetAfterMemento = false;
bool fSystem::bRestoreGfxLast = true;

// Set while a deferred Scaleform restore is outstanding. Points into the
// memento being restored, which stays alive for the whole restore.
static const sf4e::Platform::GFxApp::AdditionalMemento* g_pendingGfxRestore = nullptr;

// True only inside RestoreAllFromInternalMementos, which is the one caller
// that applies a deferred Scaleform restore afterwards. RestoreFromMemento is
// a detour, so it also runs for the game's own training-mode load, which never
// reaches that code; deferring there would drop the restore entirely.
static bool g_canDeferGfxRestore = false;
GameMementoKey::MementoID fSystem::mementoLoadRequest = { 0xffffffff, 0xffffffff };
GameMementoKey::MementoID fSystem::mementoSaveRequest = { 0xffffffff, 0xffffffff };

void fSystem::Install() {
    void (fSystem:: * _fBattleUpdate)() = &BattleUpdate;
    void (fSystem:: * _fCloseBattle)() = &CloseBattle;
    void (fSystem:: * _fSysMain_HandleTrainingModeFeatures)() = &SysMain_HandleTrainingModeFeatures;
    void (fSystem:: * _fSysMain_UpdatePauseState)() = &SysMain_UpdatePauseState;
    int (fSystem:: * _fGetMementoSize)() = &GetMementoSize;
    int (fSystem:: * _fRecordToMemento)(Memento * m, GameMementoKey::MementoID * id) = &RecordToMemento;
    int (fSystem:: * _fRestoreFromMemento)(Memento * m, GameMementoKey::MementoID * id) = &RestoreFromMemento;

    DetourAttach((PVOID*)&rSystem::mementoableMethods.GetMementoSize, *(PVOID*)&_fGetMementoSize);
    DetourAttach((PVOID*)&rSystem::mementoableMethods.RecordToMemento, *(PVOID*)&_fRecordToMemento);
    DetourAttach((PVOID*)&rSystem::mementoableMethods.RestoreFromMemento, *(PVOID*)&_fRestoreFromMemento);

    DetourAttach((PVOID*)&rSystem::publicMethods.BattleUpdate, *(PVOID*)&_fBattleUpdate);
    DetourAttach((PVOID*)&rSystem::publicMethods.CloseBattle, *(PVOID*)&_fCloseBattle);
    DetourAttach((PVOID*)&rSystem::publicMethods.SysMain_HandleTrainingModeFeatures, *(PVOID*)&_fSysMain_HandleTrainingModeFeatures);
    DetourAttach((PVOID*)&rSystem::publicMethods.SysMain_UpdatePauseState, *(PVOID*)&_fSysMain_UpdatePauseState);
    DetourAttach((PVOID*)&rSystem::staticMethods.OnBattleFlow_BattleStart, OnBattleFlow_BattleStart);
}

int fSystem::GetMementoSize() {
    return (this->*rSystem::mementoableMethods.GetMementoSize)() + sizeof(AdditionalMemento);
}

int fSystem::RecordToMemento(Memento* m, GameMementoKey::MementoID* id) {
    AdditionalMemento* additional = (AdditionalMemento*)((unsigned int)m + sizeof(Memento));
    rSystem* _this = rSystem::FromMementoable(this);
    additional->nFirstCharaToSimulate = *rSystem::GetFirstCharaToSimulate(_this);
    additional->skipRelatedFlags_0xd8c = *rSystem::GetSkipRelatedFlags_0xd8c(_this);
    additional->simulationFlags = *rSystem::GetSimulationFlags(_this);
    additional->transitionProgress  = *rSystem::GetTransitionProgress(_this);
    additional->transitionSpeed = *rSystem::GetTransitionSpeed(_this);
    additional->transitionType = *rSystem::GetTransitionType(_this);
    additional->network = *(NetworkUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_NETWORK);

    HudUnit* hud = (HudUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_HUD);
    fHud::Announce::Unit::RecordToAdditionalMemento(*HudUnit::GetAnnounce(hud), additional->announce);

    rHud::Notice::View* noticeView = *rHud::Notice::Unit::GetView(*HudUnit::GetNotice(hud));
    WithReleaser<rHud::Notice::Player>* noticePlayers = rHud::Notice::View::GetPlayers(noticeView);
    for (int playerIdx = 0; playerIdx < (_this->*rSystem::publicMethods.GetNumCharasToSimulateThisFrame)(); playerIdx++) {
        fHud::Notice::Player::RecordToAdditionalMemento(
            noticePlayers[playerIdx].obj,
            additional->playerNotices[playerIdx]
        );
    }

    Platform::GFxApp::RecordToAdditionalMemento(
        Dimps::Platform::GFxApp::staticMethods.GetSingleton(),
        additional->gfxApp
    );

    Eva::TaskCore::RecordToAdditionalMemento(
        (_this->*rSystem::publicMethods.GetTaskCore)(System::TCI_UPDATE),
        additional->updateCore
    );

    return (this->*rSystem::mementoableMethods.RecordToMemento)(m, id);
}

int fSystem::RestoreFromMemento(Memento* m, GameMementoKey::MementoID* id) {
    AdditionalMemento* additional = (AdditionalMemento*)((unsigned int)m + sizeof(Memento));
    rSystem* _this = rSystem::FromMementoable(this);
    *rSystem::GetFirstCharaToSimulate(_this) = additional->nFirstCharaToSimulate;
    *rSystem::GetSkipRelatedFlags_0xd8c(_this) = additional->skipRelatedFlags_0xd8c;
    *rSystem::GetSimulationFlags(_this) = additional->simulationFlags;
    *rSystem::GetTransitionProgress(_this) = additional->transitionProgress;
    *rSystem::GetTransitionSpeed(_this) = additional->transitionSpeed;
    *rSystem::GetTransitionType(_this) = additional->transitionType;
    *(NetworkUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_NETWORK) = additional->network;

    HudUnit* hud = (HudUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(System::U_HUD);
    rHud::Announce::Unit* announce = *HudUnit::GetAnnounce(hud);
    fHud::Announce::Unit::RestoreFromAdditionalMemento(announce, additional->announce);

    rHud::Notice::View* noticeView = *rHud::Notice::Unit::GetView(*HudUnit::GetNotice(hud));
    WithReleaser<rHud::Notice::Player>* noticePlayers = rHud::Notice::View::GetPlayers(noticeView);
    for (int playerIdx = 0; playerIdx < (_this->*rSystem::publicMethods.GetNumCharasToSimulateThisFrame)(); playerIdx++) {
        fHud::Notice::Player::RestoreFromAdditionalMemento(
            noticePlayers[playerIdx].obj,
            additional->playerNotices[playerIdx]
        );
    }

    if (bRestoreGfxLast && g_canDeferGfxRestore) {
        g_pendingGfxRestore = &additional->gfxApp;
    }
    else {
        Platform::GFxApp::RestoreFromAdditionalMemento(
            Dimps::Platform::GFxApp::staticMethods.GetSingleton(),
            additional->gfxApp
        );
    }

    Dimps::Eva::TaskCore* updateCore = (_this->*rSystem::publicMethods.GetTaskCore)(System::TCI_UPDATE);
    Eva::TaskCore::RestoreFromAdditionalMemento(updateCore, additional->updateCore);

    // Now that the task core is restored, update all the handles.
    CameraUnit* cam = (CameraUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(U_CAMERA);
    PauseUnit* pause = (PauseUnit*)(_this->*rSystem::publicMethods.GetUnitByIndex)(U_PAUSE);
    *PauseUnit::GetPauseTask(pause) = nullptr;
    *CameraUnit::GetCamShakeTask(cam) = nullptr;
    *rHud::Announce::Unit::GetHudAnnounceUpdateTask(announce) = nullptr;
    *rHud::Cockpit::Unit::GetHudCockpitUpdateTask(*HudUnit::GetCockpit(hud)) = nullptr;
    if (*HudUnit::GetContinue(hud)) {
        *rHud::Continue::Unit::GetHudContinueUpdateTask(*HudUnit::GetContinue(hud)) = nullptr;
    }
    *rHud::Cursor::Unit::GetHudCursorUpdateTask(*HudUnit::GetCursor(hud)) = nullptr;
    *rHud::Notice::Unit::GetHudNoticeUpdateTask(*HudUnit::GetNotice(hud)) = nullptr;
    if (*HudUnit::GetResult(hud)) {
        *rHud::Result::Unit::GetHudResultUpdateTask(*HudUnit::GetResult(hud)) = nullptr;
    }
    if (*HudUnit::GetSubtitle(hud)) {
        *rHud::Subtitle::Unit::GetHudSubtitleUpdateTask(*HudUnit::GetSubtitle(hud)) = nullptr;
    }
    if (*HudUnit::GetTraining(hud)) {
        *rHud::Training::Unit::GetHudTrainingUpdateTask(*HudUnit::GetTraining(hud)) = nullptr;
    }

    Dimps::Eva::Task* cursor;
    for (
        cursor = Dimps::Eva::TaskCore::GetTaskHead(updateCore);
        cursor != nullptr;
        cursor = *Dimps::Eva::Task::GetNext(cursor)
    ) {
        char* name = (updateCore->*Dimps::Eva::TaskCore::publicMethods.GetTaskName)(&cursor);
        if (strcmp(name, "PAUSE") == 0) {
            *PauseUnit::GetPauseTask(pause) = cursor;
        } else if (strcmp(name, "CAM SHAKE") == 0) {
            *CameraUnit::GetCamShakeTask(cam) = cursor;
        }
        else if (strcmp(name, "HUD ANNOUNCE") == 0) {
            *rHud::Announce::Unit::GetHudAnnounceUpdateTask(announce) = cursor;
        }
        else if (strcmp(name, "HUD COCKPIT") == 0) {
            *rHud::Cockpit::Unit::GetHudCockpitUpdateTask(*HudUnit::GetCockpit(hud)) = cursor;
        }
        else if (strcmp(name, "HUD CONTINUE") == 0) {
            *rHud::Continue::Unit::GetHudContinueUpdateTask(*HudUnit::GetContinue(hud)) = cursor;
        }
        else if (strcmp(name, "HUD CURSOR") == 0) {
            *rHud::Cursor::Unit::GetHudCursorUpdateTask(*HudUnit::GetCursor(hud)) = cursor;
        }
        else if (strcmp(name, "HUD NOTICE") == 0) {
            *rHud::Notice::Unit::GetHudNoticeUpdateTask(*HudUnit::GetNotice(hud)) = cursor;
        }
        else if (strcmp(name, "HUD RESULT") == 0) {
            *rHud::Result::Unit::GetHudResultUpdateTask(*HudUnit::GetResult(hud)) = cursor;
        }
        else if (strcmp(name, "HUD SUBTITLE") == 0) {
            *rHud::Subtitle::Unit::GetHudSubtitleUpdateTask(*HudUnit::GetSubtitle(hud)) = cursor;
        }
        else if (strcmp(name, "HUD TRAINING") == 0) {
            if (*HudUnit::GetTraining(hud)) {
                *rHud::Training::Unit::GetHudTrainingUpdateTask(*HudUnit::GetTraining(hud)) = cursor;
            }
        }
    }

    return (this->*rSystem::mementoableMethods.RestoreFromMemento)(m, id);
}

void fSystem::BattleUpdate() {
    rSystem* _this = (rSystem*)this;
    rSystem::__publicMethods& sysMethods = rSystem::publicMethods;
    rPadSystem* p = rPadSystem::staticMethods.GetSingleton();
    rPadSystem::__publicMethods& padMethods = rPadSystem::publicMethods;
    static int nLastRandomInputFrame = -1;
    static fPadSystem::Inputs randomInputs[2] = { { 0, 0 }, { 0, 0 } };

    if (!bUpdateAllowed) {
        return;
    }

    if (ggpo && nFramesToSkip > 0) {
        // Honour a time-sync request: hold the simulation this frame while
        // rendering continues, so the opponent can catch up without a freeze.
        nFramesToSkip--;
        return;
    }

    if (ggpo && *rSystem::staticVars.CurrentBattleFlow != BF__IDLE) {
        GGPOErrorCode result = GGPO_OK;
        if (localPlayerHandle != GGPO_INVALID_HANDLE) {
            if (nRandomizeLocalInputsEveryXFramesInGGPO != 0) {
                int currentFrame = rSystem::GetNumFramesSimulated_FixedPoint(_this)->integral;
                if (
                    nLastRandomInputFrame < 0 ||
                    (currentFrame - nLastRandomInputFrame) > nRandomizeLocalInputsEveryXFramesInGGPO
                ) {
                    randomInputs[0] = { localRand(), localRand() };
                    randomInputs[1] = { localRand(), localRand() };
                    nLastRandomInputFrame = currentFrame;
                }
            }

            // Submit inputs for every local side. Online sessions have exactly
            // one; sync tests have both.
            for (int i = 0; i < 2 && GGPO_SUCCEEDED(result); i++) {
                if (players[i].type != GGPO_PLAYERTYPE_LOCAL) {
                    continue;
                }
                fPadSystem::Inputs inputs;
                if (nRandomizeLocalInputsEveryXFramesInGGPO != 0) {
                    inputs = randomInputs[i];
                }
                else {
                    inputs = { (p->*padMethods.GetButtons_MappedOn)(i), (p->*padMethods.GetButtons_RawOn)(i) };
                }
                result = ggpo_add_local_input(ggpo, players[i].handle, &inputs, sizeof(fPadSystem::Inputs));
            }
        }

        if (GGPO_SUCCEEDED(result)) {
            fPadSystem::Inputs ggpoInputs[2] = { {0, 0}, {0, 0} };
            int disconnect_flags = 0;
            result = ggpo_synchronize_input(ggpo, (void*)ggpoInputs, sizeof(fPadSystem::Inputs) * 2, &disconnect_flags);
            if (GGPO_SUCCEEDED(result)) {
                fPadSystem::playbackFrame = 0;
                fPadSystem::playbackData[0][0] = ggpoInputs[0];
                fPadSystem::playbackData[0][1] = ggpoInputs[1];
                if (fSoundPlayerManager::bUsePureSounds) {
                    fSoundPlayerManager::SyncState();
                }
                (_this->*sysMethods.BattleUpdate)();
                fPadSystem::playbackFrame = -1;
                GGPOErrorCode err = ggpo_advance_frame(ggpo);
                if (!GGPO_SUCCEEDED(err)) {
                    MessageBoxA(NULL, "sf4e system could not advance frame after normal sim! Will likely crash!", NULL, MB_OK);
                }
                else {
                    if (fSoundPlayerManager::bUsePureSounds) {
                        fSoundPlayerManager::SyncState();
                    }
                    CaptureSnapshot(_this);

                    // Network health every ten seconds, so a tester's log says
                    // what the connection was like without reading the screen.
                    int frame = rSystem::GetNumFramesSimulated_FixedPoint(_this)->integral;
                    if (frame > 0 && frame % 600 == 0) {
                        for (int i = 0; i < MAX_SF4E_PROTOCOL_USERS; i++) {
                            if (players[i].type != GGPO_PLAYERTYPE_REMOTE) {
                                continue;
                            }
                            GGPONetworkStats stats;
                            if (GGPO_SUCCEEDED(ggpo_get_network_stats(ggpo, players[i].handle, &stats))) {
                                spdlog::info(
                                    "GGPO stats @ frame {}: ping {} ms, send queue {}, recv queue {}, "
                                    "local {} frames behind, remote {} frames behind, {} kbps",
                                    frame,
                                    stats.network.ping,
                                    stats.network.send_queue_len,
                                    stats.network.recv_queue_len,
                                    stats.timesync.local_frames_behind,
                                    stats.timesync.remote_frames_behind,
                                    stats.network.kbps_sent
                                );
                            }
                        }
                    }
                }
            }
        }
    }
    else {
        if (fSoundPlayerManager::bUsePureSounds) {
            fSoundPlayerManager::SyncState();
        }
        (_this->*rSystem::publicMethods.BattleUpdate)();
    }
    
    if (nExtraFramesToSimulate > 0) {
        for (int i = 0; i < nExtraFramesToSimulate; i++) {
            fPadSystem::playbackFrame = i;
            (_this->*sysMethods.BattleUpdate)();
        }
        fPadSystem::playbackFrame = -1;
        nExtraFramesToSimulate = 0;
    }

    if (bHaltAfterNext) {
        bHaltAfterNext = false;
        bUpdateAllowed = false;
    }
}

void fSystem::CloseBattle() {
    rSystem* _this = (rSystem*)this;
    if (ggpo) {
        ggpo_close_session(ggpo);
        ggpo = nullptr;
    }
    nFramesToSkip = 0;
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used) {
            SaveState::Free(&saveStates[i]);
        }
    }

    // Snapshots are per-battle. Leaving them around makes the next battle
    // compare against stale frames (upstream issue #9).
    snapshotMap.clear();

    if (syncTest.bActive) {
        spdlog::info(
            "Sync test finished: {} frames verified, {} state mismatches (last @ {}), {} raw-byte differences, {} GAMEPLAY divergences (last @ {})",
            syncTest.nFramesVerified,
            syncTest.nMismatches,
            syncTest.nLastMismatchFrame,
            syncTest.nRawMismatches,
            syncTest.nGameplayMismatches,
            syncTest.nLastGameplayMismatchFrame
        );
    }
    syncTest.bActive = false;
    syncTest.records.clear();

    (_this->*rSystem::publicMethods.CloseBattle)();

}

void fSystem::OnBattleFlow_BattleStart(System* s) {
    if (nNextBattleStartFlowTarget > -1) {
        rSystem::staticMethods.SetBattleFlow(s, nNextBattleStartFlowTarget);
        nNextBattleStartFlowTarget = -1;
        return;
    }

    return rSystem::staticMethods.OnBattleFlow_BattleStart(s);
}

void fSystem::SysMain_HandleTrainingModeFeatures() {
    rSystem* _this = (rSystem*)this;
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    CharaUnit* charaUnit = (CharaUnit*)(_this->*GetUnitByIndex)(rSystem::U_CHARA);

    if (mementoLoadRequest.lo != -1 && mementoLoadRequest.hi != -1) {
        fSystem::RestoreAllFromInternalMementos(_this, &mementoLoadRequest);
        mementoLoadRequest.lo = -1;
        mementoLoadRequest.hi = -1;
    }

    if (mementoSaveRequest.lo != -1 && mementoSaveRequest.hi != -1) {
        fSystem::RecordAllToInternalMementos(_this, &mementoSaveRequest);

        mementoSaveRequest.lo = -1;
        mementoSaveRequest.hi = -1;
    }

    if (idempotenceCheckRequest) {
        idempotenceCheckRequest = false;
        RunIdempotenceCheck();
    }

    if (extendedLoadRequest) {
        if (saveStates[0].used) {
            fSystem::SaveState::Load(&saveStates[0]);
        }
        extendedLoadRequest = false;
    }

    if (extendedSaveRequest) {
        if (saveStates[0].used) {
            fSystem::SaveState::Free(&saveStates[0]);
        }
        fSystem::SaveState::Save(&saveStates[0]);
        extendedSaveRequest = false;
    }

    (_this->*rSystem::publicMethods.SysMain_HandleTrainingModeFeatures)();
}

void fSystem::SysMain_UpdatePauseState() {
    if (!ggpo) {
        (this->*rSystem::publicMethods.SysMain_UpdatePauseState)();
    }
}

void fSystem::RestoreAllFromInternalMementos(rSystem* system, rKey::MementoID * id) {
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    CharaUnit* charaUnit = (CharaUnit*)(system->*GetUnitByIndex)(rSystem::U_CHARA);

    // Only this function applies a deferred Scaleform restore, so only this
    // function may ask for one.
    bool previousCanDefer = g_canDeferGfxRestore;
    g_canDeferGfxRestore = true;

    (system->*rSystem::publicMethods.RestoreFromInternalMementoKey)(id);
    (charaUnit->*CharaUnit::publicMethods.RestoreFromInternalMementoKey)(id);
    (
        ((EffectUnit*)(system->*GetUnitByIndex)(rSystem::U_EFFECT))->*
        EffectUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);

    (
        ((VfxUnit*)(system->*GetUnitByIndex)(rSystem::U_VFX))->*
        VfxUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);


    (
        ((CommandUnit*)(system->*GetUnitByIndex)(rSystem::U_COMMAND))->*
        CommandUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);


    (
        ((HudUnit*)(system->*GetUnitByIndex)(rSystem::U_HUD))->*
        HudUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);

    (
        ((CameraUnit*)(system->*GetUnitByIndex)(rSystem::U_CAMERA))->*
        CameraUnit::publicMethods.RestoreFromInternalMementoKey
        )(id);

    (
        TrainingManager::staticMethods.GetSingleton()->*
        TrainingManager::publicMethods.RestoreFromInternalMementoKey
        )(id);

    if (!bSkipResetAfterMemento) {
        CharaActor::staticMethods.ResetAfterMemento((charaUnit->*CharaUnit::publicMethods.GetActorByIndex)(0));
        CharaActor::staticMethods.ResetAfterMemento((charaUnit->*CharaUnit::publicMethods.GetActorByIndex)(1));
    }

    // Re-apply the Scaleform pool once nothing else can disturb it.
    g_canDeferGfxRestore = previousCanDefer;
    if (g_pendingGfxRestore) {
        Platform::GFxApp::RestoreFromAdditionalMemento(
            Dimps::Platform::GFxApp::staticMethods.GetSingleton(),
            *g_pendingGfxRestore
        );
        g_pendingGfxRestore = nullptr;
    }

    // Intentionally omit the reset of the Network unit. All in-game inputs
    // are passed into and read back out of the network unit, regardless
    // of whether or not the match is local or network. The network unit's
    // reset is used to zero the inputs of the first frame after a memento
    // is loaded in training mode, for no real practical reason.
}

void fSystem::RecordAllToInternalMementos(rSystem* system, GameMementoKey::MementoID* id) {
    // This method exists entirely to work around the check that actors are
    // movable before the training mode mementos are saveable. This could be
    // replaced just by no-oping the `jz` instruction at 0x5d7fa0, but this
    // is probably more legible.
    void* (rSystem:: * GetUnitByIndex)(unsigned int) = rSystem::publicMethods.GetUnitByIndex;
    (system->*rSystem::publicMethods.RecordToInternalMementoKey)(id);

    (
        ((CharaUnit*)(system->*GetUnitByIndex)(rSystem::U_CHARA))->*
        CharaUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((EffectUnit*)(system->*GetUnitByIndex)(rSystem::U_EFFECT))->*
        EffectUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((VfxUnit*)(system->*GetUnitByIndex)(rSystem::U_VFX))->*
        VfxUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((CommandUnit*)(system->*GetUnitByIndex)(rSystem::U_COMMAND))->*
        CommandUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((HudUnit*)(system->*GetUnitByIndex)(rSystem::U_HUD))->*
        HudUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        ((CameraUnit*)(system->*GetUnitByIndex)(rSystem::U_CAMERA))->*
        CameraUnit::publicMethods.RecordToInternalMementoKey
        )(id);

    (
        TrainingManager::staticMethods.GetSingleton()->*
        TrainingManager::publicMethods.RecordToInternalMementoKey
        )(id);
}


void fSystem::StartGGPO(GGPOPlayer* inPlayers, int numPlayers, int port, int frameDelay, DWORD rngSeed) {
    GGPOSessionCallbacks cb = { 0 };
    cb.begin_game = ggpo_begin_game_callback;
    cb.advance_frame = ggpo_advance_frame_callback;
    cb.load_game_state = ggpo_load_game_state_callback;
    cb.save_game_state = ggpo_save_game_state_callback;
    cb.free_buffer = ggpo_free_buffer;
    cb.on_event = ggpo_on_event_callback;
    cb.log_game_state = ggpo_log_game_state;

    GGPOErrorCode result = ggpo_start_session(
        &ggpo,
        &cb,
        "sf4e",
        2,
        sizeof(fPadSystem::Inputs),
        port
    );
    if (result != GGPO_OK) {
        spdlog::error("GGPO session could not start: {}", (int)result);
        MessageBoxA(NULL, "GGPO could not start, check logs", NULL, MB_OK);
    }
    // A 1s timeout dropped a real match on the first WiFi hiccup. Ten seconds
    // is what rollback games actually ship with; notify at two so the UI can
    // warn before it gives up.
    ggpo_set_disconnect_timeout(ggpo, 10000);
    ggpo_set_disconnect_notify_start(ggpo, 2000);

    int localPlayerIdx = -1;
    for (int i = 0; i < 2; i++) {
        players[i].type = inPlayers[i].type;
        result = ggpo_add_player(ggpo, inPlayers + i, &players[i].handle);
        if (!GGPO_SUCCEEDED(result)) {
            spdlog::error("GGPO session could not add player: {}", (int)result);
            MessageBoxA(NULL, "GGPO could not add player", NULL, MB_OK);
            continue;
        }

        if (players[i].type == GGPO_PLAYERTYPE_LOCAL) {
            ggpo_set_frame_delay(ggpo, players[i].handle, frameDelay);
            localPlayerHandle = players[i].handle;
            localPlayerIdx = i;
        }
    }
    if (localPlayerIdx == 0) {
        for (int i = 2; i < numPlayers; i++) {
            players[i].type = inPlayers[i].type;
            result = ggpo_add_player(ggpo, inPlayers + i, &players[i].handle);
            if (!GGPO_SUCCEEDED(result)) {
                spdlog::error("GGPO session could not add spectator: {}", (int)result);
                MessageBoxA(NULL, "GGPO could not add spectator", NULL, MB_OK);
                continue;
            }
        }
    }

    nNextBattleStartFlowTarget = BF__MATCH_START;
    bUpdateAllowed = false;
    fVsBattle::bTerminateOnNextLeftBattle = true;
    fVsBattle::bOverrideNextRandomSeed = true;
    fVsBattle::nextMatchRandomSeed = rngSeed;
}

void fSystem::StartSpectating(unsigned short localport, int num_players, char* host_ip, unsigned short host_port, DWORD rngSeed) {
    localPlayerHandle = GGPO_INVALID_HANDLE;
    GGPOSessionCallbacks cb = { 0 };
    cb.begin_game = ggpo_begin_game_callback;
    cb.advance_frame = ggpo_advance_frame_callback;
    cb.load_game_state = ggpo_load_game_state_callback;
    cb.save_game_state = ggpo_save_game_state_callback;
    cb.free_buffer = ggpo_free_buffer;
    cb.on_event = ggpo_on_event_callback;
    cb.log_game_state = ggpo_log_game_state;

    GGPOErrorCode result = ggpo_start_spectating(
        &ggpo,
        &cb,
        "sf4e",
        num_players,
        sizeof(fPadSystem::Inputs),
        localport,
        host_ip,
        host_port
    );
    if (result != GGPO_OK) {
        spdlog::error("GGPO session could not start: {}", (int)result);
        MessageBoxA(NULL, "GGPO could not start, check logs", NULL, MB_OK);
    }

    nNextBattleStartFlowTarget = BF__MATCH_START;
    bUpdateAllowed = false;
    fVsBattle::bTerminateOnNextLeftBattle = true;
    fVsBattle::bOverrideNextRandomSeed = true;
    fVsBattle::nextMatchRandomSeed = rngSeed;
}

bool fSystem::ggpo_begin_game_callback(const char*)
{
    return true;
}

bool fSystem::ggpo_advance_frame_callback(int)
{
    fPadSystem::Inputs inputs[2] = { {0, 0}, {0, 0} };
    int disconnect_flags = 0;

    // Make sure we fetch new inputs from GGPO and use those to update
    // the game state instead of reading from the selected input device.
    GGPOErrorCode result = ggpo_synchronize_input(ggpo, (void*)inputs, sizeof(fPadSystem::Inputs) * 2, &disconnect_flags);
    if (!GGPO_SUCCEEDED(result)) {
        MessageBoxA(NULL, "sf4e system could not sync input during forward-sim! Will likely crash!", NULL, MB_OK);
    }
    fPadSystem::playbackFrame = 0;
    fPadSystem::playbackData[0][0] = inputs[0];
    fPadSystem::playbackData[0][1] = inputs[1];

    // Actually update.
    // It's important that this calls the _original_, undetoured method-
    // if it called fSystem::BattleUpdate, it'd be restricted to the same
    // update-halting that the detoured method is.
    rSystem* system = rSystem::staticMethods.GetSingleton();
    (system->*rSystem::publicMethods.BattleUpdate)();

    result = ggpo_advance_frame(ggpo);
    if (!GGPO_SUCCEEDED(result)) {
        MessageBoxA(NULL, "sf4e system could not advance frame after callback! Will likely crash!", NULL, MB_OK);
    }
    else {
        CaptureSnapshot(system);
    }

    fPadSystem::playbackFrame = -1;
    return true;
}

bool fSystem::ggpo_load_game_state_callback(unsigned char* buffer, int len)
{
    SaveState* state = (SaveState*)buffer;
    SaveState::Load(state);
    return true;
}

bool fSystem::ggpo_save_game_state_callback(unsigned char** buffer, int* len, int* checksum, int frame)
{
    // No GGPO callback allocates data, then hands ownership to GGPO-
    // sf4e preallocates and manages all its savestates, and the memory
    // allocation all happens internally. Consequently the memory
    // utilization of _GGPO_ is technically zero- but GGPO
    // errors with an assertion if the length is zero.
    *len = 1;

    // Find an empty position in our array, and store if we can
    // find one.
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used) {
            continue;
        }

        SaveState::Save(&saveStates[i]);
        *buffer = (unsigned char*)&saveStates[i];
        *checksum = (int)saveStates[i].checksum;

        if (syncTest.bActive) {
            SyncTestVerify(frame, &saveStates[i]);
        }

        return true;
    }

    // No empty position in the array- either there aren't enough available
    // states, or the states aren't being released or tracked correctly.
    *buffer = nullptr;
    spdlog::error("FATAL: Could not store GGPO state!");
    MessageBoxA(NULL, "FATAL: Could not store GGPO state! Will likely crash! Attach a debugger here!", NULL, MB_OK);
    return false;
}

bool fSystem::ggpo_log_game_state(char* filename, unsigned char* buffer, int)
{
    return true;
}

void fSystem::ggpo_free_buffer(void* buffer)
{
    SaveState* victim = (SaveState*)buffer;
    SaveState::Free(victim);
}

bool fSystem::ggpo_on_event_callback(GGPOEvent* info) {
    rSystem* system = rSystem::staticMethods.GetSingleton();
    int progress;

    switch (info->code) {
    case GGPO_EVENTCODE_CONNECTED_TO_PEER:
        spdlog::info("GGPO: Connected!");
        break;
    case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
        progress = 100 * info->u.synchronizing.count / info->u.synchronizing.total;
        spdlog::info("GGPO: Synchronizing: {}", progress);
        break;
    case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
        spdlog::info("GGPO: Synchronized with peer");
        break;
    case GGPO_EVENTCODE_RUNNING:
        bUpdateAllowed = true;
        spdlog::info("GGPO: Running");
        break;
    case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
        spdlog::info("GGPO: GGPO_EVENTCODE_CONNECTION_INTERRUPTED");
        break;
    case GGPO_EVENTCODE_CONNECTION_RESUMED:
        spdlog::info("GGPO: GGPO_EVENTCODE_CONNECTION_RESUMED");
        break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
        spdlog::error("GGPO: disconnected from peer (no packets for the timeout); leaving the match");
        *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
        break;
    case GGPO_EVENTCODE_TIMESYNC:
        // We are ahead of the opponent. Skip that many simulation frames so
        // they catch up, but keep rendering; the old Sleep() froze the whole
        // game for up to 130ms and read as stutter.
        nFramesToSkip += info->u.timesync.frames_ahead;
        spdlog::debug("GGPO: timesync, skipping {} frames", info->u.timesync.frames_ahead);
        break;
    }
    return true;
}

fSystem::SaveState::SaveState() {
    // There are at least 88 keys in every save state. The upper bound
    // is unclear, but we can minimize memory allocation delays by
    // reserving the lower bound.
    keys.reserve(88);
}

std::map<int, std::pair<StateSnapshot, fSystem::StateSnapshotMeta>> fSystem::snapshotMap;

void fSystem::CaptureSnapshot(rSystem* src) {
    int frameIdx = rSystem::GetNumFramesSimulated_FixedPoint(src)->integral;

    // Only capture snapshots every second.
    if (frameIdx % 60 != 0) {
        return;
    }

    auto iter = snapshotMap.find(frameIdx);
    if (iter != snapshotMap.end()) {
        snapshotMap.erase(iter);
    }

    StateSnapshot snapshot;
    BuildSnapshot(src, snapshot);

    StateSnapshotMeta meta{ false, false };
    snapshotMap.emplace(frameIdx, std::make_pair(std::move(snapshot), meta));
}

void fSystem::BuildSnapshot(rSystem* src, StateSnapshot& snapshot) {
    snapshot.frameIdx = rSystem::GetNumFramesSimulated_FixedPoint(src)->integral;

    CharaActor::__publicMethods& methods = CharaActor::publicMethods;
    CharaUnit* lpCharaUnit = (src->*rSystem::publicMethods.GetCharaUnit)();
    for (int i = 0; i < 2; i++) {
        CharaActor* a = (lpCharaUnit->*CharaUnit::publicMethods.GetActorByIndex)(i);
        memcpy_s(
            snapshot.chara[i].rootPos,
            sizeof(float) * 4,
            (a->*methods.GetCurrentRootPosition)(),
            sizeof(float) * 4
        );
        snapshot.chara[i].status = (a->*methods.GetStatus)();
        snapshot.chara[i].side = (a->*methods.GetCurrentSide)();

        (a->*methods.GetVitalityAmt_FixedPoint)(&snapshot.chara[i].vit);
        (a->*methods.GetVitalityMax_FixedPoint)(&snapshot.chara[i].vitmax);
        (a->*methods.GetRevengeAmt_FixedPoint)(&snapshot.chara[i].revenge);
        (a->*methods.GetRevengeMax_FixedPoint)(&snapshot.chara[i].revengemax);
        (a->*methods.GetRecoverableVitalityAmt_FixedPoint)(&snapshot.chara[i].recoverable);
        (a->*methods.GetRecoverableVitalityMax_FixedPoint)(&snapshot.chara[i].recoverablemax);
        (a->*methods.GetSuperComboAmt_FixedPoint)(&snapshot.chara[i].super);
        (a->*methods.GetSuperComboMax_FixedPoint)(&snapshot.chara[i].supermax);
        (a->*methods.GetSCTimeAmt_FixedPoint)(&snapshot.chara[i].sctimeamt);
        (a->*methods.GetSCTimeMax_FixedPoint)(&snapshot.chara[i].sctimemax);
        (a->*methods.GetUCTimeAmt_FixedPoint)(&snapshot.chara[i].uctime);
        (a->*methods.GetUCTimeMax_FixedPoint)(&snapshot.chara[i].uctimemax);
        (a->*methods.GetComboDamage)(&snapshot.chara[i].combodamage);
        (a->*methods.GetDamage)(&snapshot.chara[i].damage);
    }
}

void CopyIntoPlace(fSystem::SaveState* src) {
    rSystem* system = rSystem::staticMethods.GetSingleton();

    *rSystem::staticVars.CurrentBattleFlow = src->d.CurrentBattleFlow;
    *rSystem::staticVars.PreviousBattleFlow = src->d.PreviousBattleFlow;
    *rSystem::staticVars.CurrentBattleFlowSubstate = src->d.CurrentBattleFlowSubstate;
    *rSystem::staticVars.PreviousBattleFlowSubstate = src->d.PreviousBattleFlowSubstate;
    *rSystem::staticVars.CurrentBattleFlowFrame = src->d.CurrentBattleFlowFrame;
    *rSystem::staticVars.CurrentBattleFlowSubstateFrame = src->d.CurrentBattleFlowSubstateFrame;
    *rSystem::staticVars.PreviousBattleFlowFrame = src->d.PreviousBattleFlowFrame;
    *rSystem::staticVars.PreviousBattleFlowSubstateFrame = src->d.PreviousBattleFlowSubstateFrame;
    *rSystem::staticVars.BattleFlowSubstateCallable_aa9258 = src->d.BattleFlowSubstateCallable_aa9258;
    *rSystem::staticVars.BattleFlowCallback_CallEveryFrame_aa9254 = src->d.BattleFlowCallback_CallEveryFrame_aa9254;
    memcpy_s((system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager), &src->d.gameManager, sizeof(GameManager));

    for (
        auto managerIter = fSoundPlayerManager::shadowManagerMap.begin();
        managerIter != fSoundPlayerManager::shadowManagerMap.end();
        managerIter++) {
        rSoundPlayerManager* stubManager = managerIter->first;
        rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(stubManager);
        for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
            fSoundPlayerManager::adapterToCurrentSound[&adapters[i]] = src->criPlayerState[&adapters[i]];
        }
        sf4e::Platform::SoundObjectPool<4>::SaveState poolState;
        sf4e::Platform::SoundObjectPool<4>::Load(
            rSoundPlayerManager::GetAdapterPool(stubManager),
            &src->managerState[stubManager]
        );
    }

    // Place each memento key back into its position.
    for (auto iter = src->keys.begin(); iter != src->keys.end(); iter++) {
        *iter->first = iter->second;
    }

    // Force the system to reload from the replaced mementos.
    fSystem::RestoreAllFromInternalMementos(system, &GGPO_MEMENTO_ID);
}

void Clear(fSystem::SaveState* victim) {
    for (auto iter = victim->keys.begin(); iter != victim->keys.end(); iter++) {
        if (iter->first) {
            (iter->first->*rKey::publicMethods.ClearKey)();
            memset(iter->first, 0, sizeof(rKey));
        }
    }
    victim->keys.clear();

    // Restore all non-memento-key state to a sane default.
    victim->used = false;
    victim->d.CurrentBattleFlow = 0;
    victim->d.PreviousBattleFlow = 0;
    victim->d.CurrentBattleFlowSubstate = 0;
    victim->d.PreviousBattleFlowSubstate = 0;
    victim->d.CurrentBattleFlowFrame = { 0, 0 };
    victim->d.CurrentBattleFlowSubstateFrame = { 0, 0 };
    victim->d.PreviousBattleFlowFrame = { 0, 0 };
    victim->d.PreviousBattleFlowSubstateFrame = { 0, 0 };
    victim->d.BattleFlowSubstateCallable_aa9258 = nullptr;
    victim->d.BattleFlowCallback_CallEveryFrame_aa9254 = nullptr;
    victim->criPlayerState.clear();
    victim->managerState.clear();
    victim->keyChecksums.clear();
    victim->globalChecksum = 0;
    victim->checksum = 0;
}

void fSystem::SaveState::Free(SaveState* victim) {
    SaveState tmp;

    SaveState::Save(&tmp);

    // Calls to clear SF4's mementos delegate those calls to the mementoable
    // object. If the mementoable object pointer isn't valid, the key can't
    // be cleared. This isn't relevant to SF4's training mode, because clearing
    // is only ever done on re-initialization after a save, but manually
    // clearing keys when releasing the state is necessary for GGPO to avoid
    // memory leaks.
    // 
    // Copy the victim state into the engine. Once the victim state is copied,
    // the mementoable object pointers in each key are valid, and each key can
    // be safely cleared.
    CopyIntoPlace(victim);
    Clear(victim);

    // Restore the state at the start of the function. We don't need to
    // handle clearing the keys injected by this operation, because the
    // SaveState managing the keys is short-lived.
    CopyIntoPlace(&tmp);
}

void fSystem::SaveState::Load(SaveState* src) {
    std::vector<std::pair<rKey*, rKey>> tmpVec;

    // Copy and zero all currently tracked keys. It's possible that the
    // initialization detour started tracking keys that were only
    // initialized after the save state was created.
    for (auto iter = fKey::trackedKeys.begin(); iter != fKey::trackedKeys.end(); iter++) {
        tmpVec.push_back(std::make_pair(*iter, **iter));
        memset(*iter, 0, sizeof(rKey));
    }

    CopyIntoPlace(src);

    // Zero the keys that were injected by the load.
    //
    // If the memento key data from the source state were left in the key,
    // the next save would result in invalidating the memento key data and
    // the `SaveState()` pointing at invalid memory. It's also possible
    // that the keys in the loaded state are not a proper subset of the
    // keys that existed in the state when load was called, so this
    // function can't iterate over the existing tracked keys.
    for (auto iter = src->keys.begin(); iter != src->keys.end(); iter++) {
        if (iter->first) {
            memset(iter->first, 0, sizeof(rKey));
        }
    }

    // Finally, restore the original state of all tracked keys.
    for (auto iter = tmpVec.begin(); iter != tmpVec.end(); iter++) {
        *iter->first = iter->second;
    }
}

void fSystem::SaveState::Save(SaveState* dst) {
    rSystem* system = rSystem::staticMethods.GetSingleton();
    assert(dst->keys.empty());

    dst->used = true;

    RecordAllToInternalMementos(system, &GGPO_MEMENTO_ID);
    for (auto iter = fKey::trackedKeys.begin(); iter != fKey::trackedKeys.end(); iter++) {
        dst->keys.emplace_back(*iter, **iter);

        // If we leave the data in the source key, reinitialization
        // of the source key will end up freeing _our_ data. Make
        // absolutely sure to zero the source key. Ideally, we could
        // just replace the key's state with the state the key had
        // before the call to RecordAll... but the mementos won't
        // be tracked until after that call.
        memset(*iter, 0, sizeof(rKey));
    }

    for (
        auto managerIter = Sound::SoundPlayerManager::shadowManagerMap.begin();
        managerIter != Sound::SoundPlayerManager::shadowManagerMap.end();
        managerIter++) {
        rSoundPlayerManager* stubManager = managerIter->first;
        rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(stubManager);
        for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
            dst->criPlayerState[&adapters[i]] = fSoundPlayerManager::adapterToCurrentSound[&adapters[i]];
        }
        Platform::SoundObjectPool<4>::SaveState poolState;
        Platform::SoundObjectPool<4>::Save(rSoundPlayerManager::GetAdapterPool(stubManager), &poolState);
        dst->managerState[stubManager] = poolState;
    }

    dst->d.CurrentBattleFlow = *rSystem::staticVars.CurrentBattleFlow;
    dst->d.PreviousBattleFlow = *rSystem::staticVars.PreviousBattleFlow;
    dst->d.CurrentBattleFlowSubstate = *rSystem::staticVars.CurrentBattleFlowSubstate;
    dst->d.PreviousBattleFlowSubstate = *rSystem::staticVars.PreviousBattleFlowSubstate;
    dst->d.CurrentBattleFlowFrame = *rSystem::staticVars.CurrentBattleFlowFrame;
    dst->d.CurrentBattleFlowSubstateFrame = *rSystem::staticVars.CurrentBattleFlowSubstateFrame;
    dst->d.PreviousBattleFlowFrame = *rSystem::staticVars.PreviousBattleFlowFrame;
    dst->d.PreviousBattleFlowSubstateFrame = *rSystem::staticVars.PreviousBattleFlowSubstateFrame;
    dst->d.BattleFlowSubstateCallable_aa9258 = *rSystem::staticVars.BattleFlowSubstateCallable_aa9258;
    dst->d.BattleFlowCallback_CallEveryFrame_aa9254 = *rSystem::staticVars.BattleFlowCallback_CallEveryFrame_aa9254;

    memcpy_s(&dst->d.gameManager, sizeof(GameManager), (system->*rSystem::publicMethods.GetGameManager)(), sizeof(GameManager));

    SaveState::ComputeChecksum(dst);
}

static uint32_t HashGlobalData(
    const fSystem::SaveState::GlobalData& d,
    sf4e::Game::Hash::PointerNormalizer& normalizer
) {
    using sf4e::Game::Hash::Bytes;
    using sf4e::Game::Hash::Mix;

    // Hash field by field rather than the struct as a whole, so that
    // compiler padding can never leak into the result. The battle-flow
    // callbacks are code addresses in the image, which are stable.
    uint32_t h = 0x474c4f42; // 'GLOB'
    h = Mix(h, d.CurrentBattleFlow);
    h = Mix(h, d.PreviousBattleFlow);
    h = Mix(h, d.CurrentBattleFlowSubstate);
    h = Mix(h, d.PreviousBattleFlowSubstate);
    h = Bytes(&d.CurrentBattleFlowFrame, sizeof(FixedPoint), h);
    h = Bytes(&d.CurrentBattleFlowSubstateFrame, sizeof(FixedPoint), h);
    h = Bytes(&d.PreviousBattleFlowFrame, sizeof(FixedPoint), h);
    h = Bytes(&d.PreviousBattleFlowSubstateFrame, sizeof(FixedPoint), h);
    h = Mix(h, (uint32_t)d.BattleFlowSubstateCallable_aa9258);
    h = Mix(h, (uint32_t)d.BattleFlowCallback_CallEveryFrame_aa9254);
    h = normalizer.Hash(&d.gameManager, sizeof(GameManager), h);
    return h;
}

void fSystem::SaveState::ComputeChecksum(SaveState* s) {
    using sf4e::Game::Hash::Mix;

    // One normalizer for the whole save, so that a pointer shared between two
    // keys normalizes to the same ordinal in both.
    static sf4e::Game::Hash::PointerNormalizer normalizer;
    normalizer.Reset();

    // Deliberately do NOT reset the block classification here. Refreshing it
    // per save makes the checksum depend on live process memory rather than on
    // the bytes being hashed: the game commits memory as it runs, so the same
    // word can be judged a pointer in one save and a plain value in the next,
    // and two byte-identical buffers then hash differently. That was observed:
    // a System memento with zero differing bytes reported a changed checksum.
    // A stale entry is harmless by comparison, since it applies to both sides.

    s->keyChecksums.clear();
    s->keyChecksums.reserve(s->keys.size());

    uint32_t total = 0x53463445; // 'SF4E'
    uint32_t totalRaw = 0x53463445;
    for (auto iter = s->keys.begin(); iter != s->keys.end(); iter++) {
        const rKey& keyCopy = iter->second;
        KeyChecksum kc;
        kc.key = iter->first;
        kc.mementoable = keyCopy.mementoableObject;
        kc.size = (uint32_t)fKey::GetMementoDataSize(&keyCopy);

        // Number pointers within each key independently. Sharing one sequence
        // across all keys sounded better -- it would catch two objects pointing
        // at the same thing -- but it makes every key's checksum depend on how
        // many distinct pointers the earlier keys happened to hold. One real
        // difference early then cascades into every later key, reporting them
        // as changed when their bytes are identical.
        normalizer.Reset();
        kc.checksum = fKey::ChecksumNormalized(&keyCopy, normalizer);
        kc.checksumRaw = fKey::Checksum(&keyCopy);
        s->keyChecksums.push_back(kc);
        total = Mix(total, kc.checksum);
        totalRaw = Mix(totalRaw, kc.checksumRaw);
    }

    normalizer.Reset();
    s->globalChecksum = HashGlobalData(s->d, normalizer);
    s->checksum = Mix(total, s->globalChecksum);
    s->checksumRaw = Mix(totalRaw, s->globalChecksum);
}

void fSystem::ArmSyncTest(int checkDistance) {
    if (checkDistance < 1) {
        checkDistance = 1;
    }
    if (checkDistance > GGPO_MAX_PREDICTION_FRAMES) {
        checkDistance = GGPO_MAX_PREDICTION_FRAMES;
    }
    syncTest.nCheckDistance = checkDistance;
    syncTest.bArmed = true;
    fVsBattle::OnTasksRegistered = StartSyncTest;
    spdlog::info("Sync test armed with check distance {}; start a VS match to begin", checkDistance);
}

void fSystem::DisarmSyncTest() {
    syncTest.bArmed = false;
    if (fVsBattle::OnTasksRegistered == StartSyncTest) {
        fVsBattle::OnTasksRegistered = nullptr;
    }
}

void fSystem::StartSyncTest() {
    syncTest.bArmed = false;
    syncTest.nFramesVerified = 0;
    syncTest.nMismatches = 0;
    syncTest.nRawMismatches = 0;
    syncTest.nGameplayMismatches = 0;
    syncTest.nLastGameplayMismatchFrame = -1;
    syncTest.nLastMismatchFrame = -1;
    syncTest.nDumpsWritten = 0;
    syncTest.lastMismatchSummary.clear();
    syncTest.lastDumpPath.clear();
    syncTest.records.clear();
    sf4e::Game::Hash::PointerNormalizer::ResetBlockCache();

    GGPOSessionCallbacks cb = { 0 };
    cb.begin_game = ggpo_begin_game_callback;
    cb.advance_frame = ggpo_advance_frame_callback;
    cb.load_game_state = ggpo_load_game_state_callback;
    cb.save_game_state = ggpo_save_game_state_callback;
    cb.free_buffer = ggpo_free_buffer;
    cb.on_event = ggpo_on_event_callback;
    cb.log_game_state = ggpo_log_game_state;

    GGPOErrorCode result = ggpo_start_synctest(
        &ggpo,
        &cb,
        "sf4e",
        2,
        sizeof(fPadSystem::Inputs),
        syncTest.nCheckDistance
    );
    if (result != GGPO_OK) {
        spdlog::error("GGPO sync test session could not start: {}", (int)result);
        ggpo = nullptr;
        return;
    }

    localPlayerHandle = GGPO_INVALID_HANDLE;
    for (int i = 0; i < MAX_SF4E_PROTOCOL_USERS; i++) {
        players[i].type = GGPO_PLAYERTYPE_SPECTATOR;
        players[i].handle = GGPO_INVALID_HANDLE;
    }
    for (int i = 0; i < 2; i++) {
        GGPOPlayer player;
        player.size = sizeof(GGPOPlayer);
        player.type = GGPO_PLAYERTYPE_LOCAL;
        player.player_num = i + 1;
        players[i].type = GGPO_PLAYERTYPE_LOCAL;
        result = ggpo_add_player(ggpo, &player, &players[i].handle);
        if (!GGPO_SUCCEEDED(result)) {
            spdlog::error("GGPO sync test could not add player {}: {}", i + 1, (int)result);
            continue;
        }
        if (localPlayerHandle == GGPO_INVALID_HANDLE) {
            localPlayerHandle = players[i].handle;
        }
    }

    syncTest.bActive = true;

    // Mirror the online battle start so the sync test exercises the
    // same flow that real sessions do.
    nNextBattleStartFlowTarget = BF__MATCH_START;
    bUpdateAllowed = false;
    fVsBattle::bTerminateOnNextLeftBattle = true;
    fVsBattle::bOverrideNextRandomSeed = true;
    fVsBattle::nextMatchRandomSeed = localRand();
    spdlog::info("Sync test started (check distance {})", syncTest.nCheckDistance);
}

static bool GetDesyncDumpDir(std::wstring& out) {
    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &path) != S_OK) {
        return false;
    }
    std::wstring dir(path);
    CoTaskMemFree(path);
    dir += L"\\sf4e";
    CreateDirectoryW(dir.c_str(), NULL);
    dir += L"\\desync";
    CreateDirectoryW(dir.c_str(), NULL);
    out = dir;
    return true;
}

static void CaptureBlob(fSystem::SaveState* state, fSystem::SyncTest::FrameRecord& rec) {
    size_t total = 0;
    for (auto iter = state->keys.begin(); iter != state->keys.end(); iter++) {
        total += fKey::GetMementoDataSize(&iter->second);
    }
    total += sizeof(fSystem::SaveState::GlobalData);

    rec.blob.clear();
    rec.blob.reserve(total);
    rec.ranges.clear();
    rec.ranges.reserve(state->keys.size() + 1);
    for (auto iter = state->keys.begin(); iter != state->keys.end(); iter++) {
        const rKey& keyCopy = iter->second;
        size_t size = fKey::GetMementoDataSize(&keyCopy);
        rec.ranges.push_back(std::make_pair((uint32_t)rec.blob.size(), (uint32_t)size));
        if (size > 0 && keyCopy.mementos != nullptr) {
            const uint8_t* p = (const uint8_t*)keyCopy.mementos;
            rec.blob.insert(rec.blob.end(), p, p + size);
        }
    }
    rec.ranges.push_back(std::make_pair((uint32_t)rec.blob.size(), (uint32_t)sizeof(fSystem::SaveState::GlobalData)));
    const uint8_t* g = (const uint8_t*)&state->d;
    rec.blob.insert(rec.blob.end(), g, g + sizeof(fSystem::SaveState::GlobalData));
}

static size_t FirstDifference(const uint8_t* a, const uint8_t* b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return len;
}

static void WriteDesyncDump(int frame, const fSystem::SyncTest::FrameRecord& original, const fSystem::SyncTest::FrameRecord& replay) {
    std::wstring dir;
    if (!GetDesyncDumpDir(dir)) {
        spdlog::error("Sync test: could not resolve the dump directory");
        return;
    }

    wchar_t name[MAX_PATH];
    swprintf_s(name, MAX_PATH, L"%s\\synctest-%06d-original.bin", dir.c_str(), frame);
    std::ofstream(name, std::ios::binary).write((const char*)original.blob.data(), original.blob.size());
    swprintf_s(name, MAX_PATH, L"%s\\synctest-%06d-replay.bin", dir.c_str(), frame);
    std::ofstream(name, std::ios::binary).write((const char*)replay.blob.data(), replay.blob.size());

    swprintf_s(name, MAX_PATH, L"%s\\synctest-%06d-index.txt", dir.c_str(), frame);
    std::ofstream index(name);
    index << "frame " << frame << "\n";
    index << "layout: entries are [offset, len) into the .bin files; the last entry is the global (non-memento) data\n";
    index << "orig_total=" << std::hex << original.checksum << " replay_total=" << replay.checksum << std::dec << "\n\n";

    size_t n = original.ranges.size() < replay.ranges.size() ? original.ranges.size() : replay.ranges.size();
    for (size_t i = 0; i < n; i++) {
        uint32_t oOff = original.ranges[i].first, oLen = original.ranges[i].second;
        uint32_t rOff = replay.ranges[i].first, rLen = replay.ranges[i].second;
        bool isGlobal = (i == original.ranges.size() - 1);
        index << (isGlobal ? "global" : "key") << " " << i;
        if (!isGlobal && i < original.keys.size()) {
            index << " key=" << original.keys[i].key << " mementoable=" << original.keys[i].mementoable
                  << " orig_cs=" << std::hex << original.keys[i].checksum;
            if (i < replay.keys.size()) {
                index << " replay_cs=" << replay.keys[i].checksum;
            }
            index << std::dec;
        }
        index << " offset=" << oOff << " len=" << oLen;
        if (oLen != rLen) {
            index << " DIFF (length " << oLen << " vs " << rLen << ")\n";
            continue;
        }
        if (oOff + oLen > original.blob.size() || rOff + rLen > replay.blob.size()) {
            index << " (blob truncated)\n";
            continue;
        }
        size_t d = FirstDifference(original.blob.data() + oOff, replay.blob.data() + rOff, oLen);
        if (d == oLen) {
            index << " same\n";
        }
        else {
            index << " DIFF first_diff_at=+" << d << "\n";
        }
    }

    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, narrow, MAX_PATH, NULL, NULL);
    fSystem::syncTest.lastDumpPath = narrow;
    spdlog::error("Sync test: state dump written to {}", narrow);
}

// Describes a key by the class of the object it belongs to, falling back to
// the raw pointer when RTTI can't identify it.
static std::string DescribeKey(size_t index, const fSystem::SaveState::KeyChecksum& kc) {
    std::ostringstream out;
    out << "#" << index << " ";
    const std::string& className = sf4e::Rtti::GetClassName(kc.mementoable);
    if (className.empty()) {
        out << "<" << kc.mementoable << ">";
    }
    else {
        out << className;
    }
    return out.str();
}

static int CompareSaves(fSystem::SaveState* a, fSystem::SaveState* b);

// Saves, restores, saves again and reports what changed. Nothing simulates in
// between, so any difference is state the restore failed to reproduce.
// Returns the number of differing keys, or -1 if the pass couldn't run.
static int RunIdempotencePass(
    const char* label,
    fSystem::SaveState* baseline,
    bool skipResetAfterMemento,
    bool restoreGfxLast
) {
    int slotA = -1;
    int slotB = -1;
    for (int i = 0; i < NUM_SAVE_STATES && slotB < 0; i++) {
        if (fSystem::saveStates[i].used) {
            continue;
        }
        if (slotA < 0) {
            slotA = i;
        }
        else {
            slotB = i;
        }
    }
    if (slotB < 0) {
        spdlog::error("Idempotence check: need two free save slots");
        return -1;
    }

    fSystem::SaveState* a = &fSystem::saveStates[slotA];
    fSystem::SaveState* b = &fSystem::saveStates[slotB];

    spdlog::info("--- pass: {} ---", label);

    // Every pass starts from the same state. Without this each pass would run
    // against whatever the previous one left behind, and the counts would not
    // be comparable.
    if (baseline) {
        fSystem::SaveState::Load(baseline);
    }

    bool previousSkipReset = fSystem::bSkipResetAfterMemento;
    bool previousGfxLast = fSystem::bRestoreGfxLast;

    fSystem::SaveState::Save(a);
    fSystem::bSkipResetAfterMemento = skipResetAfterMemento;
    fSystem::bRestoreGfxLast = restoreGfxLast;
    fSystem::SaveState::Load(a);
    fSystem::bSkipResetAfterMemento = previousSkipReset;
    fSystem::bRestoreGfxLast = previousGfxLast;
    fSystem::SaveState::Save(b);

    int differing = CompareSaves(a, b);

    fSystem::SaveState::Free(a);
    fSystem::SaveState::Free(b);
    return differing;
}

static int CompareSaves(fSystem::SaveState* a, fSystem::SaveState* b) {
    int differingTotal = 0;
    if (a->checksum == b->checksum) {
        spdlog::info(
            "PASS: {} keys round-tripped exactly (checksum {:08x})",
            a->keyChecksums.size(),
            a->checksum
        );
    }
    else {
        size_t n = a->keyChecksums.size() < b->keyChecksums.size()
            ? a->keyChecksums.size()
            : b->keyChecksums.size();
        int differing = 0;
        int valueWordTotal = 0;
        spdlog::error(
            "FAIL: restore is lossy. {} keys before, {} after",
            a->keyChecksums.size(),
            b->keyChecksums.size()
        );
        int sampleBudget = 32;
        for (size_t i = 0; i < n; i++) {
            if (a->keyChecksums[i].checksum == b->keyChecksums[i].checksum) {
                continue;
            }
            differing++;

            const rKey& ka = a->keys[i].second;
            const rKey& kb = b->keys[i].second;
            size_t sizeA = fKey::GetMementoDataSize(&ka);
            size_t sizeB = fKey::GetMementoDataSize(&kb);
            bool comparable =
                a->keys[i].first == b->keys[i].first &&
                sizeA == sizeB && sizeA > 0 &&
                ka.mementos != nullptr && kb.mementos != nullptr;

            // Count every differing word, splitting moved heap addresses from
            // changed values. Addresses moving is expected after a restore;
            // changed values are state the restore failed to reproduce.
            const uint32_t* wa = (const uint32_t*)ka.mementos;
            const uint32_t* wb = (const uint32_t*)kb.mementos;
            size_t words = comparable ? sizeA / 4 : 0;
            int ptrWords = 0;
            int valWords = 0;
            for (size_t w = 0; w < words; w++) {
                if (wa[w] == wb[w]) {
                    continue;
                }
                if (sf4e::Game::Hash::PointerNormalizer::IsHeapPointer(wa[w]) &&
                    sf4e::Game::Hash::PointerNormalizer::IsHeapPointer(wb[w])) {
                    ptrWords++;
                }
                else {
                    valWords++;
                }
            }

            valueWordTotal += valWords;
            spdlog::error(
                "  {} size={} {:08x} -> {:08x}  ({} ptr, {} VALUE words differ)",
                DescribeKey(i, a->keyChecksums[i]),
                a->keyChecksums[i].size,
                a->keyChecksums[i].checksum,
                b->keyChecksums[i].checksum,
                ptrWords,
                valWords
            );
            if (!comparable) {
                spdlog::error("      (buffers not comparable: {} vs {} bytes)", sizeA, sizeB);
                continue;
            }

            int shown = 0;
            for (size_t w = 0; w < words && shown < 4 && sampleBudget > 0; w++) {
                if (wa[w] == wb[w]) {
                    continue;
                }
                if (sf4e::Game::Hash::PointerNormalizer::IsHeapPointer(wa[w]) &&
                    sf4e::Game::Hash::PointerNormalizer::IsHeapPointer(wb[w])) {
                    continue;
                }
                spdlog::error("      +{:<7} {:08x} -> {:08x}", w * 4, wa[w], wb[w]);
                shown++;
                sampleBudget--;
            }
        }
        if (a->globalChecksum != b->globalChecksum) {
            spdlog::error("  global battle-flow data differs");
        }
        spdlog::error(
            "  {} of {} keys differ, {} value words total",
            differing,
            n,
            valueWordTotal
        );
        differingTotal = valueWordTotal;
    }
    return differingTotal;
}

void fSystem::RunIdempotenceCheck() {
    spdlog::info("=== Save/load idempotence check ===");

    // Print where each part of the System's memento lives, so a reported byte
    // offset can be attributed to a subsystem instead of guessed at.
    {
        size_t base = sizeof(rSystem::Memento);
        spdlog::info(
            "System memento layout: game Memento = {} bytes, then AdditionalMemento ({} bytes)",
            base,
            sizeof(AdditionalMemento)
        );
        spdlog::info(
            "  network +{}, announce +{}, playerNotices +{}, gfxApp +{}, updateCore +{}",
            base + offsetof(AdditionalMemento, network),
            base + offsetof(AdditionalMemento, announce),
            base + offsetof(AdditionalMemento, playerNotices),
            base + offsetof(AdditionalMemento, gfxApp),
            base + offsetof(AdditionalMemento, updateCore)
        );
    }

    // Hold a baseline so every variant below starts from identical state.
    int baselineSlot = -1;
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (!saveStates[i].used) {
            baselineSlot = i;
            break;
        }
    }
    if (baselineSlot < 0) {
        spdlog::error("Idempotence check: no free save slot for the baseline");
        return;
    }
    SaveState* baseline = &saveStates[baselineSlot];
    SaveState::Save(baseline);

    struct Variant {
        const char* label;
        bool skipReset;
        bool gfxLast;
        int result;
    };
    Variant variants[] = {
        { "current default (Scaleform last)", false, true, -1 },
        { "upstream order (Scaleform inline)", false, false, -1 },
        { "Scaleform last, no ResetAfterMemento", true, true, -1 },
    };

    for (int i = 0; i < (int)(sizeof(variants) / sizeof(variants[0])); i++) {
        variants[i].result = RunIdempotencePass(
            variants[i].label,
            baseline,
            variants[i].skipReset,
            variants[i].gfxLast
        );
    }

    // Does a second round trip change anything more? If restoring a restored
    // state reproduces it exactly, the first restore is normalising something
    // rather than losing it, and only frames that never went through a restore
    // disagree. That is a different bug with a different fix, so measure it.
    int firstTrip = -1;
    int secondTrip = -1;
    {
        int slots[3] = { -1, -1, -1 };
        int found = 0;
        for (int i = 0; i < NUM_SAVE_STATES && found < 3; i++) {
            if (!saveStates[i].used) {
                slots[found++] = i;
            }
        }
        if (found == 3) {
            SaveState* x = &saveStates[slots[0]];
            SaveState* y = &saveStates[slots[1]];
            SaveState* z = &saveStates[slots[2]];
            spdlog::info("--- pass: convergence (two round trips) ---");
            SaveState::Load(baseline);
            SaveState::Save(x);
            SaveState::Load(x);
            SaveState::Save(y);
            SaveState::Load(y);
            SaveState::Save(z);
            spdlog::info("  first round trip:");
            firstTrip = CompareSaves(x, y);
            spdlog::info("  second round trip:");
            secondTrip = CompareSaves(y, z);
            SaveState::Free(x);
            SaveState::Free(y);
            SaveState::Free(z);
        }
    }

    // Leave the battle on the state we started from.
    SaveState::Load(baseline);
    SaveState::Free(baseline);

    // Count differing value words, not differing keys. The key count depends on
    // the checksum, which normalises pointers and so can flag a key whose bytes
    // are identical. Byte counts are a direct measurement with nothing in the
    // way, and they are what actually has to reach zero.
    spdlog::info("VERDICT (differing value words, lower is better):");
    for (int i = 0; i < (int)(sizeof(variants) / sizeof(variants[0])); i++) {
        spdlog::info("  {:<38} {}", variants[i].label, variants[i].result);
    }
    spdlog::info("  {:<38} {} then {}", "convergence (1st then 2nd trip)", firstTrip, secondTrip);
    if (secondTrip == 0 && firstTrip > 0) {
        spdlog::info("  -> state converges after one restore: the restore normalises, it does not lose");
    }
    else if (secondTrip > 0) {
        spdlog::info("  -> state does not converge: each restore keeps changing it");
    }
}

void fSystem::SyncTestVerify(int frame, SaveState* state) {
    bool mayDump = syncTest.bDumpOnMismatch && syncTest.nDumpsWritten < syncTest.nMaxDumps;

    SyncTest::FrameRecord rec;
    rec.checksum = state->checksum;
    rec.checksumRaw = state->checksumRaw;
    rec.globalChecksum = state->globalChecksum;
    rec.keys = state->keyChecksums;
    BuildSnapshot(rSystem::staticMethods.GetSingleton(), rec.snapshot);
    if (mayDump) {
        CaptureBlob(state, rec);
    }

    auto existing = syncTest.records.find(frame);
    if (existing == syncTest.records.end()) {
        // First time this frame is saved: the original pass.
        syncTest.records.emplace(frame, std::move(rec));
    }
    else {
        // Second time: the replay pass. Compare the normalized checksums; the
        // raw ones are tracked separately because embedded heap addresses
        // legitimately differ between passes.
        const SyncTest::FrameRecord& original = existing->second;
        if (original.checksumRaw != rec.checksumRaw) {
            syncTest.nRawMismatches++;
        }

        // The verdict that matters. A save state can differ in engine
        // bookkeeping while both sides still play the same match; if the
        // observable state differs, they do not.
        if (memcmp(&original.snapshot, &rec.snapshot, sizeof(StateSnapshot)) != 0) {
            syncTest.nGameplayMismatches++;
            syncTest.nLastGameplayMismatchFrame = frame;
            if (syncTest.nGameplayMismatches <= 5 || (syncTest.nGameplayMismatches % 100) == 0) {
                const StateSnapshot::CharaStateSnapshot& o = original.snapshot.chara[0];
                const StateSnapshot::CharaStateSnapshot& r = rec.snapshot.chara[0];
                spdlog::error(
                    "Sync test GAMEPLAY divergence #{} @ frame {}: P1 status {}->{}, "
                    "pos ({:.4f},{:.4f}) -> ({:.4f},{:.4f}), vit {}->{}",
                    syncTest.nGameplayMismatches,
                    frame,
                    o.status, r.status,
                    o.rootPos[0], o.rootPos[1], r.rootPos[0], r.rootPos[1],
                    o.vit.integral, r.vit.integral
                );
            }
        }

        if (original.checksum == rec.checksum) {
            syncTest.nFramesVerified++;
        }
        else {
            syncTest.nMismatches++;
            syncTest.nLastMismatchFrame = frame;

            std::ostringstream summary;
            int differing = 0;
            size_t n = original.keys.size() < rec.keys.size() ? original.keys.size() : rec.keys.size();
            summary << "frame " << frame << ": ";
            if (original.keys.size() != rec.keys.size()) {
                summary << "key count " << original.keys.size() << " vs " << rec.keys.size() << "; ";
            }
            for (size_t i = 0; i < n; i++) {
                if (original.keys[i].checksum != rec.keys[i].checksum) {
                    if (differing < 6) {
                        summary << DescribeKey(i, original.keys[i]) << " ";
                    }
                    differing++;
                }
            }
            summary << differing << " of " << n << " keys differ";
            if (original.globalChecksum != rec.globalChecksum) {
                summary << ", global data differs";
            }
            syncTest.lastMismatchSummary = summary.str();

            // A broken save state mismatches on every single frame. Log the
            // first few in full, then only occasionally.
            if (syncTest.nMismatches <= 5 || (syncTest.nMismatches % 100) == 0) {
                spdlog::error(
                    "Sync test MISMATCH #{}: {}",
                    syncTest.nMismatches,
                    syncTest.lastMismatchSummary
                );
            }

            if (mayDump && !rec.blob.empty() && !original.blob.empty()) {
                WriteDesyncDump(frame, original, rec);
                syncTest.nDumpsWritten++;
                if (syncTest.nDumpsWritten >= syncTest.nMaxDumps) {
                    spdlog::warn("Sync test: dump limit ({}) reached, no more will be written", syncTest.nMaxDumps);
                }
            }
        }
        syncTest.records.erase(existing);
    }

    // Drop anything that can no longer be replayed.
    int oldest = frame - (2 * syncTest.nCheckDistance + NUM_SAVE_STATES + 8);
    while (!syncTest.records.empty() && syncTest.records.begin()->first < oldest) {
        syncTest.records.erase(syncTest.records.begin());
    }
}
