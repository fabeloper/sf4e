#include <algorithm>
#include <fstream>
#include <sstream>
#include <string.h>
#include <utility>
#include <vector>

#include <float.h>

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
#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"

#include "../session/sf4e__SessionProtocol.hxx"

#include "sf4e.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Lobby.hxx"
#include "sf4e__MatchHud.hxx"
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
bool fSystem::bGgpoAssertAbort = false;
int fSystem::nFramesToSkip = 0;
int fSystem::nNextBattleStartFlowTarget = -1;
int fSystem::nRandomizeLocalInputsEveryXFramesInGGPO = 0;
// Off by default. The idea was to stop a rollback crossing a round reset by
// holding the transition until inputs confirmed -- but "unconfirmed > 0" is
// permanently true at any real ping, because GGPO always runs a few frames
// ahead of its last confirmed frame while both sides are sending. So the hold
// could never clear, every boundary cost a flat 30-frame stall, and those were
// 30 frames with no network pump. Kept behind SF4E_ROUND_CHECKPOINT purely so
// the experiment can be re-run; it needs a different mechanism, not a longer
// deadline.
bool fSystem::bRoundCheckpoint = false;
int fSystem::nCheckpointHeldFrames = 0;

namespace {
    // Pad bits, as the overlay names them.
    const uint32_t PAD_UP = 0x1, PAD_DOWN = 0x2, PAD_LEFT = 0x4, PAD_RIGHT = 0x8;
    const uint32_t PAD_LP = 0x10, PAD_MP = 0x20, PAD_LK = 0x40, PAD_MK = 0x80;
    const uint32_t PAD_HP = 0x400, PAD_HK = 0x800;
    const uint32_t PAD_PPP = PAD_LP | PAD_MP | PAD_HP;
    const uint32_t PAD_KKK = PAD_LK | PAD_MK | PAD_HK;

    // Scripted motions, written facing RIGHT. Mirrored at playback for the
    // side that faces left, so both players actually throw moves.
    //
    // Random button mashing effectively never produces a quarter circle, so
    // the earlier soak exercised walking and normals and nothing else -- no
    // specials, no supers, no ultra cinematics, which are the heaviest state
    // the engine has and exactly where a lossy restore would show.
    struct Motion { const uint32_t* steps; int len; const char* name; };

    const uint32_t MOT_QCF_LP[]  = { PAD_DOWN, PAD_DOWN|PAD_RIGHT, PAD_RIGHT, PAD_RIGHT|PAD_LP };
    const uint32_t MOT_QCF_HP[]  = { PAD_DOWN, PAD_DOWN|PAD_RIGHT, PAD_RIGHT, PAD_RIGHT|PAD_HP };
    const uint32_t MOT_QCB_HK[]  = { PAD_DOWN, PAD_DOWN|PAD_LEFT,  PAD_LEFT,  PAD_LEFT|PAD_HK };
    const uint32_t MOT_DP_HP[]   = { PAD_RIGHT, PAD_DOWN, PAD_DOWN|PAD_RIGHT, PAD_DOWN|PAD_RIGHT|PAD_HP };
    const uint32_t MOT_CHARGE[]  = { PAD_LEFT, PAD_LEFT, PAD_LEFT, PAD_LEFT, PAD_LEFT, PAD_RIGHT|PAD_HK };
    // Two quarter circles: super with one punch, ultra with all three.
    const uint32_t MOT_SUPER[]   = { PAD_DOWN, PAD_DOWN|PAD_RIGHT, PAD_RIGHT,
                                     PAD_DOWN, PAD_DOWN|PAD_RIGHT, PAD_RIGHT, PAD_RIGHT|PAD_HP };
    const uint32_t MOT_ULTRA_P[] = { PAD_DOWN, PAD_DOWN|PAD_RIGHT, PAD_RIGHT,
                                     PAD_DOWN, PAD_DOWN|PAD_RIGHT, PAD_RIGHT, PAD_RIGHT|PAD_PPP };
    const uint32_t MOT_ULTRA_K[] = { PAD_DOWN, PAD_DOWN|PAD_RIGHT, PAD_RIGHT,
                                     PAD_DOWN, PAD_DOWN|PAD_RIGHT, PAD_RIGHT, PAD_RIGHT|PAD_KKK };

    const Motion MOTIONS[] = {
        { MOT_QCF_LP,  4, "qcf+LP" },
        { MOT_QCF_HP,  4, "qcf+HP" },
        { MOT_QCB_HK,  4, "qcb+HK" },
        { MOT_DP_HP,   4, "dp+HP" },
        { MOT_CHARGE,  6, "charge+HK" },
        { MOT_SUPER,   7, "super" },
        { MOT_ULTRA_P, 7, "ultra PPP" },
        { MOT_ULTRA_K, 7, "ultra KKK" },
    };
    const int NUM_MOTIONS = sizeof(MOTIONS) / sizeof(MOTIONS[0]);

    struct MotionState {
        const Motion* m = nullptr;
        int step = 0;
        int hold = 0;
        bool mirror = false;
    };
    MotionState g_motion[2];

    uint32_t MirrorLeftRight(uint32_t bits) {
        uint32_t lr = bits & (PAD_LEFT | PAD_RIGHT);
        bits &= ~(PAD_LEFT | PAD_RIGHT);
        if (lr & PAD_LEFT)  bits |= PAD_RIGHT;
        if (lr & PAD_RIGHT) bits |= PAD_LEFT;
        return bits;
    }

    // One frame of input for a side: continue a motion if one is running,
    // otherwise usually a plain button and occasionally start a new motion.
    uint32_t NextSoakInput(int side) {
        MotionState& st = g_motion[side];
        if (st.m != nullptr) {
            uint32_t bits = st.m->steps[st.step];
            if (st.mirror) {
                bits = MirrorLeftRight(bits);
            }
            // Three frames per step: long enough for the engine to register
            // each direction, short enough to stay inside the motion window.
            if (++st.hold >= 3) {
                st.hold = 0;
                if (++st.step >= st.m->len) {
                    st.m = nullptr;
                    st.step = 0;
                }
            }
            return bits;
        }
        // Roughly one motion every couple of seconds per side.
        if ((sf4e::localRand() % 24) == 0) {
            st.m = &MOTIONS[sf4e::localRand() % NUM_MOTIONS];
            st.step = 0;
            st.hold = 0;
            // Side 1 starts facing left; randomise a little so both
            // orientations get exercised as they swap sides.
            st.mirror = (side == 1) ? ((sf4e::localRand() % 8) != 0) : ((sf4e::localRand() % 8) == 0);
            return st.mirror ? MirrorLeftRight(st.m->steps[0]) : st.m->steps[0];
        }
        uint32_t bits = 1u << (sf4e::localRand() % 14);
        if ((sf4e::localRand() & 3) == 0) {
            bits |= 1u << (sf4e::localRand() % 14);
        }
        return bits;
    }
}


GGPOPlayerHandle fSystem::localPlayerHandle = GGPO_INVALID_HANDLE;
GGPOSession* fSystem::ggpo = nullptr;
fSystem::PlayerConnectionInfo fSystem::players[MAX_SF4E_PROTOCOL_USERS];
fSystem::SaveState fSystem::saveStates[NUM_SAVE_STATES];
fSystem::SyncTest fSystem::syncTest;

rKey::MementoID GGPO_MEMENTO_ID = { 1, 1 };

// Floating-point mode consistency for rollback determinism.
//
// Two peers only stay in sync if their float physics computes bit-identically.
// The FPU control word (precision, rounding, denormal handling) decides that,
// and it is NOT the same everywhere: Direct3D 9 resets it while rendering
// (this game does not use D3DCREATE_FPU_PRESERVE), and different CPUs, drivers
// and Windows builds leave it in different states. When two machines run the
// physics under different words, positions drift apart by a tiny amount every
// frame and the match desyncs -- even on a perfect connection. This was seen
// as one specific opponent whose PC desynced from everyone while others played
// flawlessly at the same ping.
//
// An earlier version pinned each machine to its OWN captured word, which fixed
// rollback-vs-original consistency within a machine but did nothing to make
// two machines agree. Instead, force the SAME fixed word on every machine,
// before every simulated and re-simulated frame, so every peer does the physics
// identically regardless of hardware.
//
// The mode to force is the one the game actually runs at, observed in the log
// as PC=0x20000 (_PC_24, single precision) | _RC_NEAR | _DN_SAVE. Forcing that
// is a no-op on machines already there (the working majority) and pulls an
// outlier -- a PC whose CPU/driver/D3D left the FPU in a different state -- into
// line. (An earlier build wrongly forced _PC_53, the CRT default, which is NOT
// what this game uses.)
static bool g_fpLogged = false;

static void EnforceSimFpControl() {
    unsigned int current = 0;
    if (!g_fpLogged) {
        _controlfp_s(&current, 0, 0);   // read what this machine was left in
        spdlog::info("Sim FP: machine word 0x{:08x} (PC=0x{:x} RC=0x{:x} DN=0x{:x}) -> pinning to PC_24|RC_NEAR|DN_SAVE",
            current, current & _MCW_PC, current & _MCW_RC, current & _MCW_DN);
        g_fpLogged = true;
    }
    _controlfp_s(&current, _PC_24 | _RC_NEAR | _DN_SAVE, _MCW_PC | _MCW_RC | _MCW_DN);
}

bool fSystem::extendedLoadRequest = false;
bool fSystem::extendedSaveRequest = false;
bool fSystem::idempotenceCheckRequest = false;
bool fSystem::bSoakRestartPending = false;
int fSystem::nUntrackedIdleFrames = 0;
// ON by default from this build. Round transitions belong inside the rollback
// timeline; leaving them outside is what produced every round-boundary desync.
// Both players must agree, so an env var that one side forgets is worse than no
// switch at all -- SF4E_GGPO_IDLE=0 can still turn it off for an A/B.
bool fSystem::bGgpoDuringIdle = true;
bool fSystem::bMatchLeftIdle = false;
int fSystem::nIdleFramesInTimeline = 0;
bool fSystem::bSoakCharasPreset = false;
sf4e::Pacing::Controller fSystem::pacer;
bool fSystem::bPredictionStalled = false;
bool fSystem::bFrameRateSettingWrong = false;
std::vector<int> fSystem::soakCharaPool;

static LONGLONG QpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static double QpcToMs(LONGLONG ticks) {
    static LONGLONG frequency = 0;
    if (frequency == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        frequency = f.QuadPart;
    }
    return ticks * 1000.0 / frequency;
}

// What saving and rolling back cost on this PC over one logging window.
struct RollbackCost {
    int saves = 0;
    LONGLONG saveTicks = 0;
    LONGLONG slowestSaveTicks = 0;

    int rollbacks = 0;
    int resimFrames = 0;
    int deepestRollback = 0;
    LONGLONG rollbackTicks = 0;
    LONGLONG slowestRollbackTicks = 0;

    bool rollbackOpen = false;
    int openDepth = 0;
    LONGLONG openTicks = 0;

    void OnSave(LONGLONG ticks) {
        saves++;
        saveTicks += ticks;
        if (ticks > slowestSaveTicks) slowestSaveTicks = ticks;
    }

    void OnLoad(LONGLONG ticks) {
        CloseRollback();
        rollbackOpen = true;
        openTicks = ticks;
    }

    void OnResimFrame(LONGLONG ticks) {
        openDepth++;
        openTicks += ticks;
    }

    void CloseRollback() {
        if (!rollbackOpen) {
            return;
        }
        rollbacks++;
        resimFrames += openDepth;
        rollbackTicks += openTicks;
        if (openDepth > deepestRollback) deepestRollback = openDepth;
        if (openTicks > slowestRollbackTicks) slowestRollbackTicks = openTicks;
        rollbackOpen = false;
        openDepth = 0;
        openTicks = 0;
    }
};
static RollbackCost g_rollbackCost;
static int g_remotePingMs = 0;

static void LogRollbackCost(int frame) {
    RollbackCost& cost = g_rollbackCost;
    cost.CloseRollback();
    spdlog::info(
        "Rollback cost @ frame {}: {} rollbacks, {} re-simulated frames (deepest {}), "
        "{:.2f} ms per rollback (slowest {:.2f}), {:.2f} ms per save (slowest {:.2f})",
        frame,
        cost.rollbacks,
        cost.resimFrames,
        cost.deepestRollback,
        cost.rollbacks ? QpcToMs(cost.rollbackTicks) / cost.rollbacks : 0.0,
        QpcToMs(cost.slowestRollbackTicks),
        cost.saves ? QpcToMs(cost.saveTicks) / cost.saves : 0.0,
        QpcToMs(cost.slowestSaveTicks)
    );
    cost = RollbackCost();
}

int fSystem::PickSoakChara() {
    if (soakCharaPool.empty()) {
        return localRand() % 0x2c;
    }
    return soakCharaPool[localRand() % soakCharaPool.size()];
}
int fSystem::nIdemEveryFrames = 0;
int fSystem::nIdemCounter = 0;
bool fSystem::bSkipResetAfterMemento = false;
bool fSystem::bRestoreGfxLast = true;
bool fSystem::bInRollback = false;

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

// When a spectator last stopped receiving input, or 0 while it flows. Reset
// for every session: left over from an earlier bailout it would fire the
// moment the next match started.
static ULONGLONG g_spectatorStarvedSince = 0;
static bool g_spectatorLoggedFirstFrame = false;

// One simulated frame from the host's confirmed inputs, for a spectator that
// has fallen behind. Returns false when the next frame has not arrived yet.
static bool AdvanceSpectatorFrame(rSystem* _this) {
    fPadSystem::Inputs in[2] = { {0, 0}, {0, 0} };
    int flags = 0;
    if (!GGPO_SUCCEEDED(ggpo_synchronize_input(fSystem::ggpo, (void*)in, sizeof(fPadSystem::Inputs) * 2, &flags))) {
        return false;
    }
    fPadSystem::playbackFrame = 0;
    fPadSystem::playbackData[0][0] = in[0];
    fPadSystem::playbackData[0][1] = in[1];
    if (fSoundPlayerManager::bUsePureSounds) {
        fSoundPlayerManager::SyncState();
    }
    (_this->*rSystem::publicMethods.BattleUpdate)();
    fPadSystem::playbackFrame = -1;
    ggpo_advance_frame(fSystem::ggpo);
    return true;
}

// Soak watchdog state: when the current match was asked to start, and whether
// it ever reached GGPO_EVENTCODE_RUNNING. A rematch can stall with one side in
// the match and the other still sitting in the lobby (seen after a draw), which
// leaves a black screen and ends an unattended run. See the watchdog in
// BattleUpdate.
static DWORD g_ggpoStartTick = 0;
static bool g_ggpoReachedRunning = false;
static DWORD g_connInterruptedTick = 0;
static void AbortMatchStart(const char* why);   // defined with StartGGPO below

static const char* BattleFlowName(DWORD f) {
    switch (f) {
    case rSystem::BF__START_DEMO: return "START_DEMO";
    case rSystem::BF__READY: return "READY";
    case rSystem::BF__FIGHT: return "FIGHT";
    case rSystem::BF__FINISH: return "FINISH";
    case rSystem::BF__ROUND_RESULT: return "ROUND_RESULT";
    case rSystem::BF__MATCH_RESULT: return "MATCH_RESULT";
    case rSystem::BF__DRAW_RESULT: return "DRAW_RESULT";
    case rSystem::BF__BONUS_RESULT: return "BONUS_RESULT";
    case rSystem::BF__CONTINUE: return "CONTINUE";
    case rSystem::BF__GAME_OVER: return "GAME_OVER";
    case rSystem::BF__BTL_START: return "BTL_START";
    case rSystem::BF__BTL_OVER: return "BTL_OVER";
    case rSystem::BF__MATCH_START: return "MATCH_START";
    case rSystem::BF__MATCH_OVER: return "MATCH_OVER";
    case rSystem::BF__ROUND_START: return "ROUND_START";
    case rSystem::BF__ROUND_OVER: return "ROUND_OVER";
    case rSystem::BF__IDLE: return "IDLE";
    case rSystem::BF__RESTART: return "RESTART";
    default:               return "?";
    }
}

// Soak-test diagnostic. The round-end desync shows up as the two machines
// taking DIFFERENT branches out of a round: one goes ROUND_RESULT ->
// ROUND_START -> FIGHT (another round) while the other goes ROUND_RESULT ->
// MATCH_RESULT (match over). Logging every transition with the frame, both
// players' vitality, and whether we were inside a rollback re-simulation makes
// that fork obvious when the two PCs' logs are compared side by side -- and
// shows whether the deciding transition happened during a rollback.
// Called from both the normal frame path and the rollback re-sim path.
static void LogFlowTransition(rSystem* sys) {
    if (!sf4e::bDiagLogging || fSystem::ggpo == nullptr) {
        return;
    }
    static DWORD lastFlow = 0xffffffff;
    static DWORD lastSub = 0xffffffff;
    DWORD flow = *rSystem::staticVars.CurrentBattleFlow;
    DWORD sub = *rSystem::staticVars.CurrentBattleFlowSubstate;
    if (flow == lastFlow && sub == lastSub) {
        return;
    }
    DWORD prevFlow = lastFlow;
    DWORD prevSub = lastSub;
    lastFlow = flow;
    lastSub = sub;

    int frame = rSystem::GetNumFramesSimulated_FixedPoint(sys)->integral;
    FixedPoint vit[2] = { { 0, 0 }, { 0, 0 } };
    CharaUnit* unit = (sys->*rSystem::publicMethods.GetCharaUnit)();
    if (unit != nullptr) {
        for (int i = 0; i < 2; i++) {
            CharaActor* a = (unit->*CharaUnit::publicMethods.GetActorByIndex)(i);
            if (a != nullptr) {
                (a->*CharaActor::publicMethods.GetVitalityAmt_FixedPoint)(&vit[i]);
            }
        }
    }
    spdlog::info(
        "FLOW f{} {}({}).{} -> {}({}).{}  vit P1={} P2={}  {}",
        frame,
        BattleFlowName(prevFlow), prevFlow, prevSub,
        BattleFlowName(flow), flow, sub,
        vit[0].integral, vit[1].integral,
        fSystem::bInRollback ? "DURING-ROLLBACK" : "live"
    );
}

void fSystem::BattleUpdate() {
    rSystem* _this = (rSystem*)this;
    rSystem::__publicMethods& sysMethods = rSystem::publicMethods;
    rPadSystem* p = rPadSystem::staticMethods.GetSingleton();
    rPadSystem::__publicMethods& padMethods = rPadSystem::publicMethods;
    static int nLastRandomInputFrame = -1;
    static fPadSystem::Inputs randomInputs[2] = { { 0, 0 }, { 0, 0 } };

    // Soak watchdog. A rematch can stall with this side in the match while the
    // peer is still in the lobby (reproduced after a draw): GGPO never reaches
    // RUNNING, the screen stays black and an unattended run dies there. Bail
    // back to the lobby so the auto-ready starts a fresh match instead. Checked
    // before the bUpdateAllowed bail-out, because a stalled match is exactly
    // the case where the update is not allowed to proceed.
    // Soak runs only, again. Arming this for real matches turned a black
    // screen into a CRASH: AbortMatchStart only sets RS_ISLEAVING, it does not
    // close the GGPO session, so GGPO kept calling back into a battle system
    // that was tearing down and the game dereferenced null half a second
    // later. Recovering from a stalled start needs the session closed first,
    // which is a bigger change than this watchdog.
    if (sf4e::bSoakTest && ggpo != nullptr && !g_ggpoReachedRunning &&
        g_ggpoStartTick != 0 && (GetTickCount() - g_ggpoStartTick) > 30000) {
        g_ggpoStartTick = 0;
        AbortMatchStart("soak watchdog: GGPO never reached RUNNING (peer never joined)");
    }

    if (!bUpdateAllowed) {
        return;
    }

    // Take in what arrived while the frame limiter waited, so this frame runs
    // on the opponent's real input instead of a guess that is rolled back.
    if (ggpo != nullptr && !syncTest.bActive) {
        ggpo_idle(ggpo, 0);
    }

    // Pin the FP mode before this frame is simulated, so a later rollback of
    // this frame re-simulates under the identical mode. See EnforceSimFpControl.
    EnforceSimFpControl();

    // Soak test: report any battle-flow change the previous frame produced.
    LogFlowTransition(_this);

    // Round-boundary checkpoint.
    //
    // The round reset -- health restored, characters returned to their start
    // marks, flow advanced -- is not rollback-safe. When a rollback spans it,
    // one path has applied the reset and the other has not, and the restore
    // produces a mixture of the two. The sync test caught it exactly:
    //
    //   GAMEPLAY divergence @ frame 2226: battleFlow a=1 b=16,
    //     P1.vit a=1000 b=0, P1.rootPos[0] a=-1.500000 b=-3.108732
    //
    // State `a` is a freshly reset round; state `b` is mid-match. That is the
    // desync players hit "al acabar partida", and it is why input delay 0 --
    // which rolls back further and more often -- makes it far more likely.
    //
    // So: roll back freely during the FIGHT, where responsiveness matters, and
    // hold at every other flow state until the inputs are confirmed. Nothing
    // can then roll back across a reset. The hold costs about one round trip
    // and lands during the KO freeze and round-change animation.
    if (ggpo != nullptr && !syncTest.bActive && bRoundCheckpoint) {
        DWORD flowNow = *rSystem::staticVars.CurrentBattleFlow;
        if (flowNow != BF__FIGHT) {
            int unconfirmed = 0;
            if (GGPO_SUCCEEDED(ggpo_get_unconfirmed_depth(ggpo, &unconfirmed)) && unconfirmed > 0) {
                // Bounded, so a peer that stops sending can never freeze the
                // game here: past the cap we proceed and accept the risk,
                // which is the same behaviour as before this existed.
                if (nCheckpointHeldFrames < 30) {
                    nCheckpointHeldFrames++;
                    if (nCheckpointHeldFrames == 1) {
                        spdlog::info("Round checkpoint: holding at flow {} until inputs confirm ({} unconfirmed)",
                            (int)flowNow, unconfirmed);
                    }
                    return;
                }
                if (nCheckpointHeldFrames == 30) {
                    nCheckpointHeldFrames++;
                    spdlog::warn("Round checkpoint: gave up waiting after 30 frames; continuing");
                }
            }
            else {
                nCheckpointHeldFrames = 0;
            }
        }
        else {
            nCheckpointHeldFrames = 0;
        }
    }

    if (ggpo && nFramesToSkip > 0) {
        // Honour a time-sync request: hold the simulation this frame while
        // rendering continues, so the opponent can catch up without a freeze.
        nFramesToSkip--;
        return;
    }

    // The BF__IDLE bypass, and why this is a switch rather than a deletion.
    //
    // Skipping GGPO while the flow is idle simulates those frames outside the
    // rollback timeline: not saved, not advanced, unreplayable. Round
    // transitions pass through idle, which is why every divergence logged so
    // far is a round reset the re-simulation failed to perform.
    //
    // Bringing them in is the principled fix, but it is not obviously safe
    // online: two machines can spend DIFFERENT numbers of frames idle (load
    // times, rendering), and if those become GGPO frames the two timelines are
    // different lengths, which is a desync by construction. A sync test has one
    // machine and no network, so it can answer whether the idle bypass is the
    // cause without taking that risk.
    // BF__IDLE covers two completely different situations, and only one of them
    // may go through GGPO.
    //
    // Before the battle exists, the flow is idle and there is nothing to save:
    // letting GGPO run here calls save_game_state on a battle that has not been
    // constructed and faults immediately (measured -- first frame after "GGPO:
    // Running", null deref in the game). That is what the original guard was
    // protecting, and it was right to.
    //
    // Mid-match round transitions are also idle, and those are exactly the
    // frames that must be in the timeline: skipping them simulates the round
    // reset outside the rollback, which cannot then be replayed. Every
    // divergence logged so far is that reset failing to reproduce.
    //
    // A match that has already left idle once is in the second case.
    DWORD flowNow = *rSystem::staticVars.CurrentBattleFlow;
    if (ggpo && flowNow != BF__IDLE) {
        bMatchLeftIdle = true;
    }
    if (ggpo && flowNow == BF__IDLE && bGgpoDuringIdle && bMatchLeftIdle) {
        // An idle frame that now goes THROUGH the timeline. Online, both
        // machines must run the same number of these or their timelines are
        // different lengths -- a desync by construction, and invisible to a
        // single-machine sync test. Count them so the two logs can be compared
        // directly instead of inferring it from whether the match survived.
        nIdleFramesInTimeline++;
    }
    if (ggpo && (flowNow != BF__IDLE || (bGgpoDuringIdle && bMatchLeftIdle))) {
        GGPOErrorCode result = GGPO_OK;
        if (localPlayerHandle != GGPO_INVALID_HANDLE) {
            if (syncTest.bSoak) {
                // Every frame, so a motion plays out as a real sequence. The
                // every-N-frames path below cannot express one.
                for (int s = 0; s < 2; s++) {
                    uint32_t bits = NextSoakInput(s);
                    randomInputs[s] = { bits, bits };
                }
            }
            else if (nRandomizeLocalInputsEveryXFramesInGGPO != 0) {
                int currentFrame = rSystem::GetNumFramesSimulated_FixedPoint(_this)->integral;
                // The frame counter restarts at 0 on every new match while
                // nLastRandomInputFrame is a static that survives it, so the
                // difference goes negative and, without the `delta < 0` case
                // below, the inputs would freeze on their last value forever --
                // both characters standing still for the rest of the run. That
                // is exactly what limited earlier soaks to a single KO followed
                // by nothing but time-overs.
                int delta = currentFrame - nLastRandomInputFrame;
                if (
                    nLastRandomInputFrame < 0 ||
                    delta < 0 ||
                    delta > nRandomizeLocalInputsEveryXFramesInGGPO
                ) {
                    // One (occasionally two) random button rather than a full
                    // random 32-bit mask. Mashing every bit at once holds all
                    // directions and all attacks simultaneously, which nets out
                    // to standing and blocking -- measured: the soak produced
                    // nothing but 1000-vs-1000 time-overs. Pressing one button
                    // at a time actually walks and swings, which is what
                    // produces the KOs the round-end desync needs. Low bits
                    // only, to stay clear of system buttons like start.
                    for (int s = 0; s < 2; s++) {
                        uint32_t bits = 1u << (localRand() % 14);
                        if ((localRand() & 3) == 0) {
                            bits |= 1u << (localRand() % 14);
                        }
                        randomInputs[s] = { bits, bits };
                    }
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

        bPredictionStalled = (result == GGPO_ERRORCODE_PREDICTION_THRESHOLD);
        if (!syncTest.bActive) {
            bool fighting = flowNow == BF__READY || flowNow == BF__FIGHT || flowNow == BF__FINISH;
            sf4e::MatchHud::Tick(fighting, g_remotePingMs, bPredictionStalled);
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
                    {
                        int f = rSystem::GetNumFramesSimulated_FixedPoint(_this)->integral;
                        if (f == 60 || f == 600 || f == 3600) {
                            StateSnapshot probe;
                            BuildSnapshot(_this, probe);
                            spdlog::info("Snapshot probe @ frame {}: P1 action {} frame {}+{} posture {} timescale {}+{}; P2 action {} frame {}+{} posture {} timescale {}+{}",
                                f,
                                probe.chara[0].action, probe.chara[0].actionFrame.integral, probe.chara[0].actionFrame.fractional, probe.chara[0].posture, probe.chara[0].timeScale.integral, probe.chara[0].timeScale.fractional,
                                probe.chara[1].action, probe.chara[1].actionFrame.integral, probe.chara[1].actionFrame.fractional, probe.chara[1].posture, probe.chara[1].timeScale.integral, probe.chara[1].timeScale.fractional);
                            if (f == 60) {
                                // A time scale that is not exactly 1 means this PC is not on the
                                // Fixed frame-rate setting and will drift from the other one.
                                const bool offClock = probe.chara[0].timeScale.integral != 1 || probe.chara[0].timeScale.fractional != 0;
                                if (offClock) {
                                    bFrameRateSettingWrong = true;
                                    spdlog::warn("Frame-rate setting: this PC's time scale is {}+{}/65536, not 1. Set frames per second to Fixed or the match will desync.",
                                        probe.chara[0].timeScale.integral, probe.chara[0].timeScale.fractional);
                                }
                                else if (sf4e::Platform::D3D::LimiterActive()) {
                                    bFrameRateSettingWrong = false;
                                }
                            }
                        }
                    }

                    // Network health, summarised over the whole ten-second
                    // window rather than sampled at one instant.
                    //
                    // This used to read the stats only on the 600th frame and
                    // print that single value. A player watching the on-screen
                    // table -- which reads the SAME field every frame -- saw
                    // spikes to 180 ms while the log serenely reported 67, and
                    // the log was used to argue the connection was fine. One
                    // sample in six hundred cannot support that claim, so take
                    // every frame and report the range.
                    int frame = rSystem::GetNumFramesSimulated_FixedPoint(_this)->integral;
                    if (frame > 0 && frame % 600 == 0) {
                        LogRollbackCost(frame);
                    }
                    for (int i = 0; i < MAX_SF4E_PROTOCOL_USERS; i++) {
                        if (players[i].type != GGPO_PLAYERTYPE_REMOTE) {
                            continue;
                        }
                        GGPONetworkStats stats;
                        if (!GGPO_SUCCEEDED(ggpo_get_network_stats(ggpo, players[i].handle, &stats))) {
                            continue;
                        }
                        PlayerConnectionInfo& p = players[i];
                        g_remotePingMs = stats.network.ping;
                        if (stats.network.ping < p.pingMin) p.pingMin = stats.network.ping;
                        if (stats.network.ping > p.pingMax) p.pingMax = stats.network.ping;
                        p.pingSum += stats.network.ping;
                        // A spike is what players actually feel, so count them
                        // rather than letting an average bury them.
                        if (stats.network.ping > 100) p.pingOver100++;
                        if (stats.timesync.remote_frames_behind > p.remoteBehindMax) {
                            p.remoteBehindMax = stats.timesync.remote_frames_behind;
                        }
                        p.pingSamples++;

                        if (frame > 0 && frame % 600 == 0 && p.pingSamples > 0) {
                            spdlog::info(
                                "GGPO stats @ frame {}: ping now {} ms (min {} / avg {} / max {}, "
                                "{} of {} samples over 100 ms), send queue {}, recv queue {}, "
                                "local {} frames behind, remote {} frames behind (worst {}), {} kbps, "
                                "{} outage(s) totalling {:.1f}s",
                                frame,
                                stats.network.ping,
                                p.pingMin,
                                (int)(p.pingSum / p.pingSamples),
                                p.pingMax,
                                p.pingOver100,
                                p.pingSamples,
                                stats.network.send_queue_len,
                                stats.network.recv_queue_len,
                                stats.timesync.local_frames_behind,
                                stats.timesync.remote_frames_behind,
                                p.remoteBehindMax,
                                stats.network.kbps_sent,
                                p.outages,
                                p.outageMsTotal / 1000.0
                            );
                            p.ResetStatsWindow();
                        }
                    }
                }
            }
        }

        // Spectating: no local input, so the only failure is not having the
        // host's next frame yet. A short gap is normal (it is the network);
        // a long one means the host is gone, since a spectator session has
        // no disconnect timeout of its own. And after a hiccup on our side
        // the host's frames pile up, so simulate extra ones to close the gap.
        if (localPlayerHandle == GGPO_INVALID_HANDLE && !syncTest.bActive) {
            ULONGLONG& starvedSince = g_spectatorStarvedSince;
            if (GGPO_SUCCEEDED(result)) {
                if (!g_spectatorLoggedFirstFrame) {
                    // The game's own frame counter must agree with GGPO's, or
                    // the snapshots are labelled wrong and every comparison lies.
                    g_spectatorLoggedFirstFrame = true;
                    spdlog::info("Spectator: first frame simulated; game frame counter is now {}", rSystem::GetNumFramesSimulated_FixedPoint(_this)->integral);
                }
                starvedSince = 0;
                GGPONetworkStats stats;
                for (int extra = 0; extra < 3; extra++) {
                    if (!GGPO_SUCCEEDED(ggpo_get_network_stats(ggpo, 0, &stats)) || stats.timesync.local_frames_behind <= 6) {
                        break;
                    }
                    if (!AdvanceSpectatorFrame(_this)) {
                        break;
                    }
                }
            }
            else {
                ULONGLONG now = GetTickCount64();
                if (starvedSince == 0) {
                    starvedSince = now;
                }
                else if (now - starvedSince > 8000) {
                    spdlog::error("GGPO: no input from the host for 8 s; leaving the match");
                    *rSystem::GetReadyState(_this) = rSystem::RS_ISLEAVING;
                    starvedSince = now;
                }
            }
        }
    }
    else {
        if (fSoundPlayerManager::bUsePureSounds) {
            fSoundPlayerManager::SyncState();
        }

        // With a session live, reaching here means the flow is BF__IDLE and the
        // guard above sent us around GGPO: the game simulates, but the frame is
        // never saved, never advanced through ggpo_advance_frame, and so is not
        // in the rollback timeline at all. Anything that changes here cannot be
        // replayed -- which is the shape of every divergence we have logged, a
        // round reset the original run performed and the re-simulation did not.
        // Measure it rather than argue it: count the frames and name the
        // transitions that happen on them.
        DWORD flowBefore = *rSystem::staticVars.CurrentBattleFlow;
        (_this->*rSystem::publicMethods.BattleUpdate)();
        if (ggpo != nullptr) {
            nUntrackedIdleFrames++;
            DWORD flowAfter = *rSystem::staticVars.CurrentBattleFlow;
            if (flowAfter != flowBefore) {
                spdlog::warn("Battle flow {} -> {} happened OUTSIDE ggpo (untracked frame {}); "
                    "this transition is not in the rollback timeline",
                    (int)flowBefore, (int)flowAfter, nUntrackedIdleFrames);
            }
        }
    }
    
    if (nExtraFramesToSimulate > 0) {
        for (int i = 0; i < nExtraFramesToSimulate; i++) {
            fPadSystem::playbackFrame = i;
            (_this->*sysMethods.BattleUpdate)();
        }
        fPadSystem::playbackFrame = -1;
        nExtraFramesToSimulate = 0;
    }

    // Online match over: the moment the flow reaches the match result, the
    // deciding round has just ended, so whoever has more life left won.
    // Time-over and KO both satisfy that; a double KO reads as a draw.
    static DWORD lastFlow = 0xffffffff;
    DWORD flow = *rSystem::staticVars.CurrentBattleFlow;
    if (flow != lastFlow) {
        if (ggpo && !syncTest.bActive && (flow == BF__MATCH_OVER || flow == BF__MATCH_RESULT)) {
            StateSnapshot s;
            BuildSnapshot(_this, s);
            int winner = -1;
            if (s.chara[0].vit.integral > s.chara[1].vit.integral) winner = 0;
            else if (s.chara[1].vit.integral > s.chara[0].vit.integral) winner = 1;
            sf4e::Lobby::OnMatchResult(winner, -1, -1);
        }
        lastFlow = flow;
    }

    if (bHaltAfterNext) {
        bHaltAfterNext = false;
        bUpdateAllowed = false;
    }
}

void fSystem::CloseBattle() {
    rSystem* _this = (rSystem*)this;
    if (ggpo) {
        // Report the idle-frame counts for ANY session, not just a sync test.
        // Online is the only place the two numbers can disagree, and online is
        // exactly where there is no sync-test summary to carry them.
        if (bGgpoDuringIdle) {
            spdlog::info("Match idle frames: {} inside the timeline, {} outside. "
                "The inside count must match the other PC exactly.",
                nIdleFramesInTimeline, nUntrackedIdleFrames);
        }
        nIdleFramesInTimeline = 0;
        nUntrackedIdleFrames = 0;
        sf4e::MatchHud::EndMatch();
        ResetPacing("match");
        ggpo_close_session(ggpo);
        ggpo = nullptr;
    }
    bMatchLeftIdle = false;
    nFramesToSkip = 0;
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used) {
            SaveState::Free(&saveStates[i]);
        }
    }
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        SaveState::Reclaim(&saveStates[i], "battle_close_sweep", i);
    }

    // Snapshots are per-battle. Leaving them around makes the next battle
    // compare against stale frames (upstream issue #9).
    snapshotMap.clear();

    if (syncTest.bActive) {
        syncTest.nMatchesRun++;
        syncTest.nTotalFramesVerified += syncTest.nFramesVerified;
        syncTest.nTotalGameplayMismatches += syncTest.nGameplayMismatches;
        // Report the two signals separately. "Frames verified" counts only
        // fully checksum-clean frames, and the actor's cosmetic bytes drift on
        // every single rollback -- so it reads as a flat 0 and looks like
        // nothing was checked at all. What matters is how many frames were
        // checked and how many of those diverged in state the GAME reads.
        {
            int checked = syncTest.nFramesVerified + syncTest.nMismatches;
            // Frames the rollback could never reproduce, because they never
            // entered the timeline. If the divergence count tracks this, the
            // round-reset failure is a consequence of the BF__IDLE bypass.
            spdlog::info(
                "Sync test finished: {} frames checked, {} GAMEPLAY-clean ({} divergences, last @ {}); "
                "checksum-clean {} ({} differ, last @ {}, {} raw-byte) - checksum noise is expected, gameplay is the verdict",
                checked,
                checked - syncTest.nGameplayMismatches,
                syncTest.nGameplayMismatches,
                syncTest.nLastGameplayMismatchFrame,
                syncTest.nFramesVerified,
                syncTest.nMismatches,
                syncTest.nLastMismatchFrame,
                syncTest.nRawMismatches
            );
            spdlog::info("  {} frames simulated OUTSIDE ggpo this match (battle flow was idle) - "
                "these are not in the rollback timeline", nUntrackedIdleFrames);
            nUntrackedIdleFrames = 0;
            spdlog::info("  {} idle frames ran INSIDE the timeline this match - compare this number "
                "against the other PC; they must match exactly", nIdleFramesInTimeline);
            nIdleFramesInTimeline = 0;
        }
        if (syncTest.bSoak) {
            // The running total is the number that matters across a long
            // unattended run; a single match proves very little.
            spdlog::warn(
                "SOAK TOTALS after {} matches: {} frames verified, {} GAMEPLAY divergences",
                syncTest.nMatchesRun,
                syncTest.nTotalFramesVerified,
                syncTest.nTotalGameplayMismatches
            );
            // Re-arm so the next match keeps testing without anyone touching
            // the menus.
            int d = syncTest.nCheckDistance;
            bool soak = syncTest.bSoak;
            syncTest.bActive = false;
            syncTest.records.clear();
            ArmSyncTest(d);
            syncTest.bSoak = soak;
            bSoakRestartPending = true;
            // The GAME still has to tear its battle down. Returning here ran our
            // half of the cleanup (GGPO closed, save states freed) and skipped
            // the game's half entirely, so the next match started on top of a
            // battle that was never closed and faulted walking stale objects.
            // That is why every soak run died after exactly one match.
            (_this->*rSystem::publicMethods.CloseBattle)();
            return;
        }
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

    // Sample the round trip WHILE the fight is moving, not once while a dummy
    // stands still. An idle character has static meter, no move in flight and
    // no effects -- close to the emptiest state the game can be in, and the
    // least likely to catch state that restore drops. Driven by the soak input
    // generator this walks through specials, supers and ultras on its own and
    // checks the round trip at each of them.
    if (nIdemEveryFrames > 0 && ggpo == nullptr) {
        if (++nIdemCounter >= nIdemEveryFrames) {
            nIdemCounter = 0;
            idempotenceCheckRequest = true;
        }
    }

    if (idempotenceCheckRequest) {
        idempotenceCheckRequest = false;
        // Stamp what the fight was doing at this sample, so a failing round
        // trip can be tied to the state it happened in rather than averaged
        // with every other sample.
        {
            SessionProtocol::StateSnapshot s;
            BuildSnapshot(rSystem::staticMethods.GetSingleton(), s);
            spdlog::info("=== idempotence sample: flow={} P1 super={} revenge={} status={} | P2 super={} revenge={} status={} ===",
                (int)*rSystem::staticVars.CurrentBattleFlow,
                (int)s.chara[0].super.integral, (int)s.chara[0].revenge.integral, (int)s.chara[0].status,
                (int)s.chara[1].super.integral, (int)s.chara[1].revenge.integral, (int)s.chara[1].status);
        }
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


// Ends the match cleanly instead of freezing on a modal box. A GGPO start
// failure used to pop a message box, which blocks the game loop; the peer then
// sees it go silent and desyncs too. Leaving returns both sides to the lobby.
static void AbortMatchStart(const char* why) {
    spdlog::error("Match start aborted: {}", why);

    // Drop the session BEFORE asking the game to leave. It is half-built by
    // definition here -- we are aborting precisely because it never reached
    // RUNNING -- and every rollback path keys off "ggpo != nullptr", so
    // leaving it live means the teardown can still be called back into a
    // session that never had two players. Clearing it first makes all of
    // those paths no-ops on the way out.
    if (fSystem::ggpo) {
        ggpo_close_session(fSystem::ggpo);
        fSystem::ggpo = nullptr;
    }
    fSystem::nFramesToSkip = 0;
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (fSystem::saveStates[i].used) {
            fSystem::SaveState::Free(&fSystem::saveStates[i]);
        }
    }

    rSystem* sys = rSystem::staticMethods.GetSingleton();
    if (sys) {
        *rSystem::GetReadyState(sys) = rSystem::RS_ISLEAVING;
    }
}

// GGPO tripped one of its own internal invariants.
//
// This used to be invisible: GGPO popped a modal dialog (freezing whichever
// thread hit it -- the game thread) and then called exit(0). A clean exit
// raises no exception, so the crash handler never ran, no dump was written,
// nothing reached the log, and Windows recorded a SUCCESSFUL exit. Players
// reported "it crashed" and we had no way to see why; one whole session ended
// at frame 1800 with a healthy 50 ms ping and not one line explaining it.
//
// Now the assertion text and its source location reach the log, and the MATCH
// ends instead of the process. GGPO's own state is inconsistent once this
// fires, so the session has to go -- but the player keeps their game, their
// lobby and their logs.
static void __cdecl OnGgpoAssertFailed(const char* msg) {
    spdlog::critical("GGPO internal assertion: {}", msg ? msg : "(no message)");
    spdlog::critical("This is a bug in the netcode, not in your connection. "
        "Ending the match; please send this log.");

    // Do not call back into GGPO from here -- it is mid-invariant-break. Just
    // ask the battle system to leave, the way a desync abort does.
    fSystem::bGgpoAssertAbort = true;
    rSystem* system = rSystem::staticMethods.GetSingleton();
    if (system) {
        *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
    }
}

void fSystem::StartGGPO(GGPOPlayer* inPlayers, int numPlayers, int port, int frameDelay, DWORD rngSeed) {
    ResetPacing(nullptr);
    {
        char pacingEnv[8] = { 0 };
        pacer.enabled = !(GetEnvironmentVariableA("SF4E_PACING", pacingEnv, sizeof(pacingEnv)) > 0 && pacingEnv[0] == '0');
        spdlog::info("Pacing: {}", pacer.enabled ? "on (frames stretch or shrink by up to 3 ms to stay level with the other PC)" : "off (SF4E_PACING=0)");
    }
    g_rollbackCost = RollbackCost();
    // Floor the delay here as well as in the menu. The lobby is one way in; the
    // debug overlay is another, and a stale settings file is a third. This is
    // the single point every path passes through, so it is the one place the
    // floor cannot be missed.
    if (frameDelay < 1) {
        spdlog::info("Input delay raised from {} to 1: at zero, every remote frame is a "
            "prediction and the game rolls back on all of them.", frameDelay);
        frameDelay = 1;
    }
    // A rematch must not build on a pool the previous teardown left behind.
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        SaveState::Reclaim(&saveStates[i], "start_ggpo", i);
    }
    // Re-capture the simulation FP mode fresh for this match.
    g_fpLogged = false;   // log the machine's FP word once per match

    // Soak test: nobody is sitting at either PC, so drive the local side with
    // random inputs. Standing still would only ever produce time-overs; we need
    // real damage and KOs, because the desync we are hunting happens at the
    // round/match-ending knockout.
    if (sf4e::bSoakTest) {
        nRandomizeLocalInputsEveryXFramesInGGPO = 8;
        spdlog::info("SOAK TEST: random local inputs every {} frames", nRandomizeLocalInputsEveryXFramesInGGPO);
    }

    // A match needs exactly two players in slots 1 and 2. On a poor connection
    // the lobby data can be momentarily incomplete when the match fires, and a
    // half-built player list makes ggpo_add_player fail ("could not add
    // player"). Refuse to start rather than freeze on that box.
    if (numPlayers < 2
        || inPlayers[0].player_num != 1 || inPlayers[1].player_num != 2
        || (inPlayers[0].type != GGPO_PLAYERTYPE_LOCAL && inPlayers[0].type != GGPO_PLAYERTYPE_REMOTE)
        || (inPlayers[1].type != GGPO_PLAYERTYPE_LOCAL && inPlayers[1].type != GGPO_PLAYERTYPE_REMOTE)) {
        AbortMatchStart("player list not ready (both players not yet in the lobby)");
        return;
    }

    // Installed before any session exists so an assertion during startup is
    // logged too. Setting it repeatedly is harmless.
    ggpo_set_assert_handler(OnGgpoAssertFailed);

    // Arm the stalled-start watchdog for EVERY match. It only ran during soak
    // tests, so a real player whose opponent vanished between the lobby and the
    // first frame sat on a black screen forever with nothing in the log --
    // which is exactly what the comment on the watchdog already described.
    g_ggpoStartTick = GetTickCount();
    g_ggpoReachedRunning = false;

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
        AbortMatchStart("ggpo_start_session failed");
        return;
    }
    // A 1s timeout dropped a real match on the first WiFi hiccup. Ten seconds
    // is what rollback games actually ship with; notify at two so the UI can
    // warn before it gives up.
    ggpo_set_disconnect_timeout(ggpo, 10000);
    ggpo_set_disconnect_notify_start(ggpo, 2000);

    int localPlayerIdx = -1;
    for (int i = 0; i < 2; i++) {
        players[i].type = inPlayers[i].type;
        // Fresh window each match: these are static and would otherwise carry
        // the previous match's spikes into this one's summary.
        players[i].ResetStatsWindow();
        result = ggpo_add_player(ggpo, inPlayers + i, &players[i].handle);
        if (!GGPO_SUCCEEDED(result)) {
            ggpo_close_session(ggpo);
            ggpo = nullptr;
            AbortMatchStart("ggpo_add_player failed");
            return;
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
    ResetPacing(nullptr);
    pacer.enabled = false;
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        SaveState::Reclaim(&saveStates[i], "start_spectating", i);
    }
    localPlayerHandle = GGPO_INVALID_HANDLE;
    for (int i = 0; i < MAX_SF4E_PROTOCOL_USERS; i++) {
        players[i].type = GGPO_PLAYERTYPE_SPECTATOR;
        players[i].handle = GGPO_INVALID_HANDLE;
    }
    spdlog::info("GGPO: spectating {}:{} from local port {}", host_ip, host_port, localport);
    g_spectatorStarvedSince = 0;
    g_spectatorLoggedFirstFrame = false;
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

    // When the host closes its session at the end of the match, the last
    // input packets can be lost and nothing retransmits them. Silence is
    // the signal then: three seconds without any packet and we leave, and
    // a player's game still gets ten before its own peer gives up on it.
    ggpo_set_disconnect_timeout(ggpo, 3000);
    ggpo_set_disconnect_notify_start(ggpo, 1000);

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
    // This is a rollback re-simulation. It must run under the same FP mode as
    // the original simulation of these frames did.
    EnforceSimFpControl();
    const LONGLONG startedAt = QpcNow();

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
    if (system == nullptr) {
        // The battle system is gone: the peer dropped and the match is tearing
        // down, or it never finished loading. GGPO still calls this back, and
        // calling BattleUpdate through a null system makes the GAME dereference
        // address 0 -- the "access violation ... reading address 0x00000000 at
        // SSFIV.exe+0x1633b0" crash seen whenever a new match was started after
        // a peer vanished. Keep GGPO's frame count consistent and get out
        // instead; the session is already on its way down.
        static bool warned = false;
        if (!warned) {
            warned = true;
            spdlog::error("GGPO advance callback with no battle system; skipping frame (match is tearing down)");
        }
        ggpo_advance_frame(ggpo);
        fPadSystem::playbackFrame = -1;
        return true;
    }
    // This callback only ever runs while GGPO re-simulates rolled-back frames,
    // so it is exactly the window the soak-test flow logger wants to flag.
    bInRollback = true;
    (system->*rSystem::publicMethods.BattleUpdate)();
    // Log while still flagged, so a transition decided during a rollback
    // re-simulation is reported as such.
    LogFlowTransition(system);
    bInRollback = false;

    result = ggpo_advance_frame(ggpo);
    if (!GGPO_SUCCEEDED(result)) {
        MessageBoxA(NULL, "sf4e system could not advance frame after callback! Will likely crash!", NULL, MB_OK);
    }
    else {
        CaptureSnapshot(system);
    }

    fPadSystem::playbackFrame = -1;
    g_rollbackCost.OnResimFrame(QpcNow() - startedAt);
    return true;
}

bool fSystem::ggpo_load_game_state_callback(unsigned char* buffer, int len)
{
    // Same guard as the advance callback: restoring walks the battle system and
    // its GameManager, so with the system gone this dereferences null and takes
    // the process down mid-teardown.
    if (buffer == nullptr || rSystem::staticMethods.GetSingleton() == nullptr) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            spdlog::error("GGPO load-state callback with no battle system; skipping restore (match is tearing down)");
        }
        return true;
    }
    SaveState* state = (SaveState*)buffer;
    const LONGLONG startedAt = QpcNow();
    SaveState::Load(state);
    g_rollbackCost.OnLoad(QpcNow() - startedAt);
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

    // Saving reads the battle system and its GameManager, so with the system
    // gone (peer dropped, match tearing down) this would dereference null the
    // same way the advance callback did.
    if (rSystem::staticMethods.GetSingleton() == nullptr) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            spdlog::error("GGPO save-state callback with no battle system; skipping save (match is tearing down)");
        }
        *buffer = nullptr;
        return false;
    }

    // Find an empty position in our array, and store if we can
    // find one.
    for (int i = 0; i < NUM_SAVE_STATES; i++) {
        if (saveStates[i].used) {
            continue;
        }

        const LONGLONG startedAt = QpcNow();
        SaveState::Save(&saveStates[i]);
        // Only the sync test compares checksums; a match never reads them.
        if (syncTest.bActive) {
            SaveState::ComputeChecksum(&saveStates[i]);
        }
        g_rollbackCost.OnSave(QpcNow() - startedAt);
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
    // The session destructor frees every slot, used or not; unused ones are null.
    if (!buffer) {
        return;
    }
    SaveState* victim = (SaveState*)buffer;
    SaveState::Free(victim);
}

// Once per outer tick, never inside a GGPO callback: account for the shift the
// limiter applied, sample how far ahead we run, and ask for the next shift.
void fSystem::StepPacing() {
    if (!ggpo || !pacer.enabled || localPlayerHandle == GGPO_INVALID_HANDLE || syncTest.bActive) {
        return;
    }
    pacer.OnShiftApplied(sf4e::Platform::D3D::TakeAppliedShift());
    if (bPredictionStalled) {
        pacer.OnPredictionStall();
    }
    else {
        for (int i = 0; i < MAX_SF4E_PROTOCOL_USERS; i++) {
            if (players[i].type != GGPO_PLAYERTYPE_REMOTE) {
                continue;
            }
            GGPONetworkStats stats;
            if (GGPO_SUCCEEDED(ggpo_get_network_stats(ggpo, players[i].handle, &stats))) {
                pacer.OnRiftSample(stats.timesync.local_frames_behind, stats.timesync.remote_frames_behind);
            }
            break;
        }
    }
    // The limiter only runs under the game's fixed frame-rate setting. Without
    // it there is nothing to shift, and GGPO's own frame skips stay in charge.
    static int ticksWithoutLimiter = 0;
    if (!sf4e::Platform::D3D::LimiterActive()) {
        if (++ticksWithoutLimiter == 300) {
            bFrameRateSettingWrong = true;
            spdlog::warn("Pacing: the game's frame limiter has not run for 5 s (frame rate setting is not Fixed?); "
                "time-sync corrections cannot be applied on this PC");
        }
        sf4e::Platform::D3D::RequestFrameShift(0.0);
        return;
    }
    ticksWithoutLimiter = 0;
    sf4e::Platform::D3D::RequestFrameShift(pacer.NextShiftMs());
    if (pacer.samples > 0 && pacer.samples % 600 == 0) {
        spdlog::info("Pacing: rift {:.2f} frames (peak {:.2f}), slowed {:.0f} ms, sped up {:.0f} ms, largest step {:.2f} ms, {} stalled ticks",
            pacer.riftFrames, pacer.maxAbsRiftFrames, pacer.slowedMs, pacer.spedUpMs, pacer.maxShiftMs, pacer.stallTicks);
    }
}

void fSystem::ResetPacing(const char* label) {
    if (label && pacer.samples > 0) {
        spdlog::info("Pacing ({}): slowed {:.0f} ms, sped up {:.0f} ms, largest step {:.2f} ms, rift peak {:.2f} frames, {} stalled ticks{}",
            label, pacer.slowedMs, pacer.spedUpMs, pacer.maxShiftMs, pacer.maxAbsRiftFrames, pacer.stallTicks,
            sf4e::Platform::D3D::LimiterActive() ? "" : " - the frame limiter never ran, so no correction could apply");
    }
    pacer.Reset();
    bPredictionStalled = false;
    sf4e::Platform::D3D::CancelFrameShift();
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
        g_ggpoReachedRunning = true;   // soak watchdog: the match really started
        bUpdateAllowed = true;
        spdlog::info("GGPO: Running");
        break;
    case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
        // The freeze a player actually feels. Packets from the other machine
        // have stopped, so the simulation stalls at the prediction limit until
        // they come back. Worth stating plainly: two of these read as "the game
        // froze", and the connection stats showed nothing at all, because an
        // outage is not high ping -- it is no packets to measure.
        g_connInterruptedTick = GetTickCount();
        spdlog::warn("Connection interrupted: no packets from the other player. "
            "The game will stall until they come back.");
        break;
    case GGPO_EVENTCODE_CONNECTION_RESUMED:
        if (g_connInterruptedTick != 0) {
            DWORD outageMs = GetTickCount() - g_connInterruptedTick;
            // Add the notify threshold: the link was already silent for that
            // long before GGPO told us.
            outageMs += 2000;
            g_connInterruptedTick = 0;
            for (int i = 0; i < MAX_SF4E_PROTOCOL_USERS; i++) {
                if (players[i].type == GGPO_PLAYERTYPE_REMOTE) {
                    players[i].outages++;
                    players[i].outageMsTotal += outageMs;
                }
            }
            spdlog::warn("Connection resumed after about {:.1f}s without packets "
                "(that is the freeze you just felt)", outageMs / 1000.0);
        }
        else {
            spdlog::info("GGPO: GGPO_EVENTCODE_CONNECTION_RESUMED");
        }
        break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
        if (info->u.disconnected.player >= 1000) {
            spdlog::warn("GGPO: spectator {} dropped; the match goes on", info->u.disconnected.player - 1000);
            break;
        }
        spdlog::error("GGPO: disconnected from peer (no packets for the timeout); leaving the match");
        *rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
        break;
    case GGPO_EVENTCODE_TIMESYNC:
        if (pacer.enabled && localPlayerHandle != GGPO_INVALID_HANDLE && sf4e::Platform::D3D::LimiterActive()) {
            // Pacing repays the gap a few milliseconds per frame; a whole-frame
            // skip on top of that would overshoot.
            spdlog::info("GGPO: timesync recommends {} frames; pacing is on, no frames skipped (rift {:.2f} frames, outstanding {:.1f} ms)",
                info->u.timesync.frames_ahead, pacer.riftFrames, pacer.outstandingMs);
            break;
        }
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
    // Field logs show 89 to 91 keys in every save state; 96 covers that
    // without reallocating on the first save.
    // reserving the lower bound.
    keys.reserve(96);
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
    snapshot.battleFlow = (int)*rSystem::staticVars.CurrentBattleFlow;
    snapshot.battleFlowSubstate = (int)*rSystem::staticVars.CurrentBattleFlowSubstate;

    // Soak-test diagnostic: chunk-checksum the GameManager so the two machines
    // compare their round/match bookkeeping every snapshot. We deliberately
    // sample PAST the 0x49c the save state copies: a divergence at an offset
    // >= that is proof the round state we fail to save is what forks a
    // round-end. Bounded by VirtualQuery so reading past the object can never
    // fault.
    if (sf4e::bDiagLogging) {
        const uint8_t* gm = (const uint8_t*)(src->*rSystem::publicMethods.GetGameManager)();
        MEMORY_BASIC_INFORMATION mbi = { 0 };
        size_t readable = 0;
        if (gm != nullptr &&
            VirtualQuery(gm, &mbi, sizeof(mbi)) == sizeof(mbi) &&
            mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            readable = (size_t)(((const uint8_t*)mbi.BaseAddress + mbi.RegionSize) - gm);
        }
        // All-or-nothing: a partial fill would leave trailing zeros here and
        // real checksums on the other machine, which would read as a fake
        // divergence. Either both sides probe the same span or we probe none.
        if (readable >= SessionProtocol::GM_PROBE_BYTES) {
            // Mask anything that looks like an address before hashing. The
            // GameManager is full of heap pointers, which differ between two
            // machines by nature and would report a divergence every single
            // snapshot, burying the signal we actually want. Masking by VALUE
            // RANGE (not by querying this machine's heap) is what keeps the two
            // machines consistent: the same field is a pointer at the same
            // offset on both, so both mask the same words. Small integers --
            // round counters, timers, the state we are actually hunting -- fall
            // below the range and are still compared exactly.
            const size_t words = SessionProtocol::GM_CHUNK_BYTES / sizeof(uint32_t);
            for (size_t c = 0; c < SessionProtocol::GM_MAX_CHUNKS; c++) {
                const uint32_t* src32 =
                    (const uint32_t*)(gm + c * SessionProtocol::GM_CHUNK_BYTES);
                uint32_t masked[SessionProtocol::GM_CHUNK_BYTES / sizeof(uint32_t)];
                for (size_t w = 0; w < words; w++) {
                    uint32_t v = src32[w];
                    masked[w] = (v >= 0x00010000u && v <= 0x7fffffffu) ? 0xaaaaaaaau : v;
                }
                snapshot.gmChunks[c] = sf4e::Game::Hash::Bytes(
                    masked, sizeof(masked), 0x474d0001 /* 'GM' */
                );
            }
        }
        else {
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("SOAK TEST: GameManager probe skipped, only {} bytes readable", readable);
            }
        }
    }

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
        snapshot.chara[i].action = (a->*methods.GetActionID)();
        snapshot.chara[i].posture = (a->*methods.GetActionPosture)();
        // These two hand the value back through a pointer. Start from zero
        // and take the returned pointer when there is one, so a getter that
        // does not fill the argument still yields the same bytes on both PCs.
        FixedPoint frame = { 0, 0 };
        FixedPoint* frameOut = (a->*methods.GetActionFrame)(&frame);
        snapshot.chara[i].actionFrame = frameOut ? *frameOut : frame;
        FixedPoint scale = { 0, 0 };
        (src->*rSystem::publicMethods.GetUnitTimeScale_Fixed)(&scale, i);
        snapshot.chara[i].timeScale = scale;
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
        // Restore only what the state recorded. An adapter or manager that did
        // not exist at save time is left alone instead of being zeroed.
        for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
            auto record = src->criPlayerState.find(&adapters[i]);
            if (record != src->criPlayerState.end()) {
                fSoundPlayerManager::adapterToCurrentSound[&adapters[i]] = record->second;
            }
        }
        auto poolRecord = src->managerState.find(stubManager);
        if (poolRecord != src->managerState.end()) {
            sf4e::Platform::SoundObjectPool<4>::Load(
                rSoundPlayerManager::GetAdapterPool(stubManager),
                &poolRecord->second
            );
        }
    }

    // Place each memento key back into its position.
    for (auto iter = src->keys.begin(); iter != src->keys.end(); iter++) {
        *iter->first = iter->second;
    }

    // Force the system to reload from the replaced mementos.
    fSystem::RestoreAllFromInternalMementos(system, &GGPO_MEMENTO_ID);

    // NOTE: do NOT try to re-stamp a Chara::Actor's saved memento image over the
    // live object by raw byte offset. The memento is the game's STRUCTURED
    // serialization, not a flat copy of the actor: geometry logging showed two
    // actors whose live bases are only 0x7080 (28KB) apart while each memento is
    // ~330KB, so buffer offset +N does NOT map to actorBase+N - the trailing
    // float region lives in separately-allocated child objects. Writing by
    // offset walks off the actor into unmapped memory (two hardware-fault
    // crashes, 2026-09-14). Making the actor restore lossless needs the real
    // sub-object addresses (disassembly of the game's record/restore), not a
    // byte overlay. See sf4e-rollback-findings.
}

void Clear(fSystem::SaveState* victim) {
    for (auto iter = victim->keys.begin(); iter != victim->keys.end(); iter++) {
        if (iter->first && victim->ownsKeys) {
            (iter->first->*rKey::publicMethods.ClearKey)();
            memset(iter->first, 0, sizeof(rKey));
        }
    }
    victim->keys.clear();
    victim->ownsKeys = true;

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
    if (!victim) {
        return;
    }
    // GGPO releases its oldest state before every save, so this runs once per
    // simulated and once per re-simulated frame. It used to save the live game,
    // restore the victim, clear it and restore the live game again: a full
    // record and two full restores per frame. ClearKey only needs the victim's
    // key installed, not its game state, so install each key just long enough
    // to release it and put the live key back.
    if (victim->ownsKeys) {
        for (auto& entry : victim->keys) {
            if (!entry.first) {
                continue;
            }
            const rKey live = *entry.first;
            *entry.first = entry.second;
            (entry.first->*rKey::publicMethods.ClearKey)();
            *entry.first = live;
        }
    }
    victim->ownsKeys = false;
    Clear(victim);
}

// Drop a slot's records without touching the engine. At the points that call
// this (session start, post-teardown sweep) the objects the keys point at are
// gone or belong to a fresh battle, so ClearKey through them is exactly what
// must not happen.
void fSystem::SaveState::Reclaim(SaveState* victim, const char* reason, int slotIndex) {
    if (!victim->used && victim->keys.empty()) {
        return;
    }
    spdlog::warn("SaveState: reclaiming leaked slot {} ({}) used={} keys={}",
        slotIndex, reason ? reason : "?", victim->used, victim->keys.size());
    victim->ownsKeys = false;
    Clear(victim);
}

void fSystem::SaveState::Load(SaveState* src) {
    std::vector<std::pair<rKey*, rKey>> tmpVec;

    // A load abandons the current timeline, and with it the stops queued by
    // frames about to be re-simulated. Re-simulation queues again whatever
    // survives; a stale entry would cut a sound that should keep playing.
    if (fSoundPlayerManager::bUsePureSounds) {
        for (auto& queue : fSoundPlayerManager::queuedStops) {
            queue.second.clear();
        }
    }

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
}

void fSystem::SaveState::SaveWithChecksum(SaveState* dst) {
    Save(dst);
    ComputeChecksum(dst);
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

// Writes a random pairing into the confirmed conditions, the same way the
// netplay path forces the characters both players chose. Done here because
// this runs on the VsPreBattle hook, which is the last moment the game will
// accept them.
using rVsModeSoak = Dimps::GameEvents::VsMode;
void fSystem::SyncTestPickRandomCharacters() {
    if (bSoakCharasPreset) {
        // The soak restart already chose these at pre-battle and the game has
        // loaded them. Re-rolling now would change the IDs out from under the
        // loaded assets.
        bSoakCharasPreset = false;
        return;
    }
    char* vsModeQuery[] = { "VSMode" };
    rVsModeSoak* mode = (rVsModeSoak*)Dimps::Event::EventBaseWithEC::FindForegroundEvent(
        Dimps::App::GetRootEvent(), vsModeQuery, 1);
    if (!mode) {
        return;
    }
    rVsModeSoak::ConfirmedPlayerConditions* conditions = rVsModeSoak::GetConfirmedPlayerConditions(mode);
    int picked[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        int id = PickSoakChara();
        picked[i] = id;
        *(rVsModeSoak::ConfirmedPlayerConditions::GetCharaID(&conditions[i])) = (BYTE)id;
        *(rVsModeSoak::ConfirmedPlayerConditions::GetSideActive(&conditions[i])) = 1;
    }
    // Stages differ in geometry, lighting and background objects, all of which
    // the camera and effect containers touch -- and those are among the keys
    // the first run reported as failing to restore.
    int stage = sf4e::localRand() % 30;
    Dimps::Platform::dString* stageName = rVsModeSoak::GetStageName(mode);
    (stageName->*Dimps::Platform::dString::publicMethods.assign)(Dimps::stageCodes[stage], 4);
    *(rVsModeSoak::GetStageCode(mode)) = stage;

    spdlog::info("Sync test soak: match {} - characters {} vs {}, stage {}",
        syncTest.nMatchesRun + 1, picked[0], picked[1], stage);
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

    if (syncTest.bSoak) {
        // Both sides mash. Random button presses produce specials, supers and
        // ultras often enough across thousands of frames, and they exercise
        // state a scripted routine never would.
        nRandomizeLocalInputsEveryXFramesInGGPO = 8;
        SyncTestPickRandomCharacters();
    }

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

    fSystem::SaveState::SaveWithChecksum(a);
    fSystem::bSkipResetAfterMemento = skipResetAfterMemento;
    fSystem::bRestoreGfxLast = restoreGfxLast;
    fSystem::SaveState::Load(a);
    fSystem::bSkipResetAfterMemento = previousSkipReset;
    fSystem::bRestoreGfxLast = previousGfxLast;
    fSystem::SaveState::SaveWithChecksum(b);

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

            // Dump EVERY differing value word, not a sample of four.
            //
            // The sample was how this stayed vague for months: it showed that
            // "runs of consecutive floats near the end" differed, which is a
            // description of a diff rather than an identification of one. The
            // complete offset set IS the bug -- any byte restore cannot
            // reproduce is a byte rollback cannot preserve -- so print all of
            // it, grouped into consecutive runs so the structure is visible,
            // and interpreted as floats because that is what these words are.
            size_t w = 0;
            int runs = 0;
            while (w < words && runs < 64) {
                bool differs = wa[w] != wb[w] &&
                    !(sf4e::Game::Hash::PointerNormalizer::IsHeapPointer(wa[w]) &&
                      sf4e::Game::Hash::PointerNormalizer::IsHeapPointer(wb[w]));
                if (!differs) {
                    w++;
                    continue;
                }
                size_t runStart = w;
                while (w < words && wa[w] != wb[w] &&
                       !(sf4e::Game::Hash::PointerNormalizer::IsHeapPointer(wa[w]) &&
                         sf4e::Game::Hash::PointerNormalizer::IsHeapPointer(wb[w]))) {
                    w++;
                }
                size_t runWords = w - runStart;
                spdlog::error("      +{}..+{} ({} word{}, {:.1f}% into the memento)",
                    runStart * 4, (w - 1) * 4, runWords, runWords == 1 ? "" : "s",
                    words ? (100.0 * runStart / words) : 0.0);
                for (size_t k = runStart; k < w && k < runStart + 12; k++) {
                    float fa, fb;
                    memcpy(&fa, &wa[k], 4);
                    memcpy(&fb, &wb[k], 4);
                    spdlog::error("        +{:<7} {:08x} -> {:08x}   {:>14.6f} -> {:<14.6f}",
                        k * 4, wa[k], wb[k], fa, fb);
                }
                if (runWords > 12) {
                    spdlog::error("        ... {} more words in this run", runWords - 12);
                }
                runs++;
            }
            if (runs >= 64) {
                spdlog::error("      (stopped after 64 runs)");
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
    SaveState::SaveWithChecksum(baseline);

    // Memento geometry for the actor keys. Each key's buffer holds numMementos
    // snapshots (a ring), so GetMementoDataSize spans ALL of them - not one
    // object image. Log the true per-snapshot size and each snapshot's offset
    // so a restore can target one snapshot and stay in the object's bounds.
    for (auto iter = baseline->keys.begin(); iter != baseline->keys.end(); iter++) {
        rKey& k = iter->second;
        if (k.mementoableObject == nullptr) {
            continue;
        }
        if (sf4e::Rtti::GetClassName(k.mementoableObject).find("Chara::Actor")
            == std::string::npos) {
            continue;
        }
        size_t dataSize = fKey::GetMementoDataSize(&k);
        spdlog::info(
            "Actor geometry: obj={} numMementos={} sizeAllocated={} nextIdx={} "
            "dataSize={} perMemento~={}",
            k.mementoableObject, k.numMementos, k.sizeAllocated, k.nextMementoIndex,
            dataSize, k.numMementos ? dataSize / k.numMementos : 0);
        for (int m = 0; m < k.numMementos && m < 8; m++) {
            ptrdiff_t off = (const uint8_t*)k.metadata[m].memento - (const uint8_t*)k.mementos;
            spdlog::info("   memento[{}] at +{} (id {:08x}:{:08x})",
                m, off, k.metadata[m].id.hi, k.metadata[m].id.lo);
        }

        // Name what lives at a failing offset instead of inferring it from the
        // float values. Two things have to be established, in order.
        //
        // First: does a memento offset even correspond to an object offset?
        // The diff reports offsets into the SERIALISED buffer, and reading
        // those as object offsets is an assumption -- if the memento is packed
        // or reordered, every conclusion drawn from it is wrong. Compare the
        // buffer against the live object and report how far they agree.
        {
            const uint8_t* live = (const uint8_t*)k.mementoableObject;
            const uint8_t* img = (const uint8_t*)k.metadata[0].memento;
            // Count TOTAL agreement, not the matching prefix. Byte 0 of any
            // object is its vtable pointer, which a serialiser would replace
            // or omit -- so a prefix test reports 0% for a memento that is
            // otherwise a faithful image, which is exactly what it did. Also
            // try small shifts, in case the image sits behind a header.
            if (live && img) {
                size_t bestShift = 0;
                size_t bestMatch = 0;
                for (size_t shift = 0; shift <= 64; shift += 4) {
                    size_t n = dataSize - shift;
                    size_t match = 0;
                    for (size_t j = 0; j < n; j += 4) {
                        if (*(const uint32_t*)(live + j) == *(const uint32_t*)(img + shift + j)) {
                            match++;
                        }
                    }
                    if (match > bestMatch) {
                        bestMatch = match;
                        bestShift = shift;
                    }
                }
                size_t words = dataSize / 4;
                spdlog::info("  memento-vs-object: best agreement {} of {} words ({:.1f}%) at shift +{}{}",
                    bestMatch, words, words ? (100.0 * bestMatch / words) : 0.0, bestShift,
                    bestMatch > words / 2 ? "  -> essentially a flat image; buffer offsets map to object offsets"
                                          : "  -> NOT a flat image; buffer offsets mean nothing in the object");
            }
        }

        // Second: walk the object for embedded polymorphic sub-objects. Every
        // Dimps class keeps its MSVC RTTI, so a vtable at offset N names the
        // sub-object starting there, and a failing offset belongs to whichever
        // named sub-object most recently precedes it. Only the tail is walked:
        // that is where all 128 differing words live.
        {
            const uint8_t* live = (const uint8_t*)k.mementoableObject;
            size_t from = dataSize > 40000 ? dataSize - 40000 : 0;
            spdlog::info("  embedded sub-objects from +{} to +{}:", from, dataSize);
            int named = 0;
            std::string last;
            for (size_t off = from; off + 4 <= dataSize && named < 64; off += 4) {
                const std::string& cn = sf4e::Rtti::GetClassName(live + off);
                if (cn.empty() || cn == last) {
                    continue;
                }
                last = cn;
                spdlog::info("    +{:<8} {}", off, cn);
                named++;
            }
            if (named == 0) {
                spdlog::info("    (none found - the tail is plain data, not objects)");
            }
        }
    }

    // The decisive pass: does a restore change anything the GAME can see?
    //
    // Everything above measures memento bytes, which conflates two very
    // different failures. Hair physics drifting and super meter drifting both
    // show up as "value words differ", but only one of them can change who
    // wins. BuildSnapshot reads the fields through the game's own accessors --
    // health, meter, revenge, status, position -- so comparing it across a
    // round trip asks the question directly, with no dependence on how the
    // memento is laid out.
    //
    // If this passes while the byte count stays at 128, the lossy words are
    // not gameplay state, and the netplay meter desync has a different cause.
    // If it fails, this is the bug.
    {
        spdlog::info("--- pass: gameplay state across a round trip ---");
        SaveState::Load(baseline);

        SessionProtocol::StateSnapshot before;
        BuildSnapshot(rSystem::staticMethods.GetSingleton(), before);

        int slot = -1;
        for (int i = 0; i < NUM_SAVE_STATES; i++) {
            if (!saveStates[i].used) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            spdlog::error("  no free slot for the gameplay pass");
        }
        else {
            SaveState* s = &saveStates[slot];
            SaveState::SaveWithChecksum(s);
            SaveState::Load(s);

            SessionProtocol::StateSnapshot after;
            BuildSnapshot(rSystem::staticMethods.GetSingleton(), after);
            SaveState::Free(s);

            bool gameplay = SessionProtocol::SnapshotGameplayDiffers(before, after);
            bool flow = SessionProtocol::SnapshotFlowDiffers(before, after);
            if (!gameplay && !flow) {
                spdlog::info("  PASS: every gameplay field survived the round trip unchanged");
                spdlog::info("  -> whatever the lossy words are, they are not state the game reads");
            }
            else {
                spdlog::error("  FAIL: gameplay state changed across a pure save/restore");
                spdlog::error("    {}", SessionProtocol::DescribeSnapshotDiff(before, after));
            }
        }
    }

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
            SaveState::SaveWithChecksum(x);
            SaveState::Load(x);
            SaveState::SaveWithChecksum(y);
            SaveState::Load(y);
            SaveState::SaveWithChecksum(z);
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
                spdlog::error(
                    "Sync test GAMEPLAY divergence #{} @ frame {}: {}",
                    syncTest.nGameplayMismatches,
                    frame,
                    sf4e::SessionProtocol::DescribeSnapshotDiff(original.snapshot, rec.snapshot)
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
