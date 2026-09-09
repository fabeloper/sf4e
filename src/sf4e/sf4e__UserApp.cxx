#include <chrono>
#include <memory>
#include <vector>

#include <windows.h>
#include <detours/detours.h>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__UserApp.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionProtocol.hxx"
#include "../session/sf4e__SessionServer.hxx"

#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__UserApp.hxx"

namespace SessionProtocol = sf4e::SessionProtocol;
using Dimps::App;
using Dimps::Event::EventBase;
using Dimps::Event::EventBaseWithEC;
using Dimps::Event::EventController;
using Dimps::Game::ProgressData;
using Dimps::GameEvents::RootEvent;
using Dimps::Math::FixedPoint;
using rMainMenu = Dimps::GameEvents::MainMenu;
using rVsMode = Dimps::GameEvents::VsMode;
using rUserApp = Dimps::UserApp;
using fSystem = sf4e::Game::Battle::System;
using fUserApp = sf4e::UserApp;
using fMainMenu = sf4e::GameEvents::MainMenu;
using fVsBattle = sf4e::GameEvents::VsBattle;
using fVsPreBattle = sf4e::GameEvents::VsPreBattle;
using sf4e::Game::Battle::Sound::SoundPlayerManager;
using sf4e::SessionClient;
using sf4e::SessionServer;

std::unique_ptr<fUserApp::Netplay> fUserApp::netplay;
std::unique_ptr<SessionServer> fUserApp::server;

sf4e::UserApp::Netplay::Netplay(
    const SessionClient::Callbacks& callbacks,
    std::string sidecarHash,
    uint16_t ggpoPort,
    std::string& name,
    uint8_t _deviceType,
    uint8_t _deviceIdx,
    uint8_t _delay,
    bool _spectator
):
    client(callbacks, sidecarHash, ggpoPort, name, _spectator),
    deviceType(_deviceType),
    deviceIdx(_deviceIdx),
    delay(_delay),
    spectator(_spectator)
{}

void fUserApp::_OnVsBattleTasksRegistered()
{
    std::vector<SessionProtocol::MemberData>& members = netplay->client._lobbyData.members;
    char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
    netplay->client._serverAddr.ToString(szAddr, sizeof(szAddr), false);

    // The server keeps the players in front of the spectators. An empty
    // address means "at the session server", which is where the relay is.
    std::vector<const SessionProtocol::MemberData*> playerMembers, spectatorMembers;
    const SessionProtocol::MemberData* me = nullptr;
    for (size_t i = 0; i < members.size(); i++) {
        if (members[i].connId == netplay->client._cid) {
            me = &members[i];
        }
        if (members[i].spectator) {
            spectatorMembers.push_back(&members[i]);
        }
        else if (playerMembers.size() < 2) {
            playerMembers.push_back(&members[i]);
        }
    }
    bool isPlayer = !netplay->spectator && me != nullptr && !me->spectator;

    if (isPlayer) {
        GGPOPlayer players[MAX_SF4E_PROTOCOL_USERS];
        int numPlayers = 0;
        int localIdx = -1;
        for (int i = 0; i < (int)playerMembers.size(); i++) {
            const SessionProtocol::MemberData& memberData = *playerMembers[i];
            GGPOPlayer& player = players[numPlayers++];
            player.size = sizeof(GGPOPlayer);
            player.player_num = i + 1;
            if (playerMembers[i] == me) {
                player.type = GGPO_PLAYERTYPE_LOCAL;
                localIdx = i;

                // Inject the chosen device into this player's side
                Dimps::Pad::System* padSys = Dimps::Pad::System::staticMethods.GetSingleton();
                Dimps::Pad::System::__publicMethods& padSysMethods = Dimps::Pad::System::publicMethods;
                (padSys->*padSysMethods.AssociatePlayerAndGamepad)(i, netplay->deviceIdx);
                (padSys->*padSysMethods.SetDeviceTypeForPlayer)(i, netplay->deviceType);
                (padSys->*padSysMethods.SetSideHasAssignedController)(i, 1);
                (padSys->*padSysMethods.SetActiveButtonMapping)(Dimps::Pad::System::BUTTON_MAPPING_FIGHT);
            }
            else {
                player.type = GGPO_PLAYERTYPE_REMOTE;
                strcpy_s(player.u.remote.ip_address, 32, memberData.ip.empty() ? szAddr : memberData.ip.c_str());
                player.u.remote.port = memberData.port;
            }
        }

        // P1 feeds the spectators, and only those the server marked as
        // watching this match; anyone who joined later waits for the next.
        if (localIdx == 0) {
            for (size_t i = 0; i < spectatorMembers.size() && numPlayers < MAX_SF4E_PROTOCOL_USERS; i++) {
                const SessionProtocol::MemberData& memberData = *spectatorMembers[i];
                if (!memberData.watching) {
                    continue;
                }
                GGPOPlayer& player = players[numPlayers++];
                player.size = sizeof(GGPOPlayer);
                player.type = GGPO_PLAYERTYPE_SPECTATOR;
                strcpy_s(player.u.remote.ip_address, 32, memberData.ip.empty() ? szAddr : memberData.ip.c_str());
                player.u.remote.port = memberData.port;
                spdlog::info("Netplay: feeding spectator {} at port {}", memberData.name, memberData.port);
            }
        }
        fSystem::StartGGPO(
            players,
            numPlayers,
            netplay->client._ggpoPort,
            netplay->delay,
            netplay->client._matchData.rngSeed
        );
    }
    else {
        if (playerMembers.empty()) {
            spdlog::error("Netplay: asked to spectate, but the lobby has no players");
            return;
        }
        // Spectate from P1. Through a relay pipe, our own member entry says
        // which port is ours to send to; without one, P1's own port.
        const SessionProtocol::MemberData& host = *playerMembers[0];
        uint16_t hostPort = (me != nullptr && me->hostPort != 0) ? me->hostPort : host.port;
        // Safe-_ish_ removal of const. This gets passed through to an
        // inet_pton() call and never modified.
        char* hostIP = (char*)(host.ip.empty() ? szAddr : host.ip.c_str());
        spdlog::info("Netplay: spectating {} at {}:{}", host.name, hostIP, hostPort);
        fSystem::StartSpectating(
            netplay->client._ggpoPort,
            2,
            hostIP,
            hostPort,
            netplay->client._matchData.rngSeed
        );
    }
}

void fUserApp::_OnVsPreBattleTasksRegistered()
{
    size_t charaConditionSize = sizeof(rVsMode::ConfirmedCharaConditions);

    // XXX (adanducci): this is a little fragile- it's technically possible
    // that the pre-battle event is constructed in another context, but
    // practically speaking the VsPreBattle event will always be used in
    // the context of VsMode.
    char* vsModeQuery[] = { "VSMode" };
    rVsMode* mode = (rVsMode*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), vsModeQuery, 1);
    if (!mode) {
        spdlog::error("VsPreBattle tasks registered, but the current foreground event isn't VSMode!");
        return;
    }

    Dimps::Platform::dString* stageName = rVsMode::GetStageName(mode);
    rVsMode::ConfirmedPlayerConditions* conditions = rVsMode::GetConfirmedPlayerConditions(mode);
    for (int i = 0; i < 2; i++) {
        *(rVsMode::ConfirmedPlayerConditions::GetCharaID(&conditions[i])) = netplay->client._matchData.chara[i].charaID;
        *(rVsMode::ConfirmedPlayerConditions::GetSideActive(&conditions[i])) = 1;
        rVsMode::ConfirmedCharaConditions* charaConditions = rVsMode::ConfirmedPlayerConditions::GetCharaConditions(&conditions[i]);
        memcpy_s(charaConditions, charaConditionSize, &netplay->client._matchData.chara[i], charaConditionSize);
    }

    (stageName->*Dimps::Platform::dString::publicMethods.assign)(Dimps::stageCodes[netplay->client._matchData.stageID], 4);
    *(rVsMode::GetStageCode(mode)) = netplay->client._matchData.stageID;
}

void OnReady(sf4e::SessionClient* const client, const sf4e::SessionClient::Callbacks& c) {
    // Since handling a request forces the process to load into a battle,
    // handling the request can only reasonably be done if the process is
    // currently on the main menu.
    RootEvent* root = App::GetRootEvent();
    char* mainMenuQuery[1] = { "MainMenu" };
    rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(
        root,
        mainMenuQuery,
        1
    );
    if (!mainMenu) {
        spdlog::info("Client: ignoring that both clients are ready because we're not on the main menu");
        return;
    }

    ProgressData* progressData = *RootEvent::GetProgressData(root);
    ProgressData::BattleTypeSettings* BattleTypeSettings = &(ProgressData::GetBattleTypeSettings(progressData)[ProgressData::NBT_PVP]);
    *ProgressData::GetNextBattleType(progressData) = ProgressData::NBT_PVP;
    BattleTypeSettings->editionSelect = client->_lobbyData.editionSelect;
    BattleTypeSettings->rounds = client->_lobbyData.roundCount;
    BattleTypeSettings->timeLimit = client->_lobbyData.roundTime;
    fVsPreBattle::bSkipToVersus = true;
    fVsPreBattle::OnTasksRegistered = fUserApp::_OnVsPreBattleTasksRegistered;
    fVsBattle::OnTasksRegistered = fUserApp::_OnVsBattleTasksRegistered;
    (rMainMenu::ToItemObserver(mainMenu)->*rMainMenu::itemObserverMethods.GoToVersusMode)();
}

void OnBattleSynced(SessionClient* const client, const sf4e::SessionClient::Callbacks& callbacks) {
    fVsBattle::bSessionSynced = true;
}

sf4e::SessionClient::Callbacks clientCallbacks = {
    nullptr,
    sf4e::Overlay::OnClientError,
    OnReady,
    OnBattleSynced,
};

void fUserApp::Install() {
    DetourAttach((PVOID*)&rUserApp::staticMethods.Steam_PostUpdate, Steam_PostUpdate);
}

void fUserApp::StartSession(char* joinAddr, uint16_t port, std::string& sidecarHash, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay, bool spectator) {
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.ParseString(joinAddr);
    netplay.reset(new Netplay(
        clientCallbacks,
        sidecarHash,
        port,
        name,
        deviceType,
        deviceIdx,
        delay,
        spectator
    ));
    netplay->client.Connect(addr);
}

void fUserApp::StartServer(uint16 hostPort, std::string& identity, std::string& sidecarHash, bool editionSelect, int roundCount, FixedPoint roundTime) {
    server.reset(new SessionServer(identity, sidecarHash, editionSelect, roundCount, roundTime));
    server->Listen(hostPort);
}

void fUserApp::Steam_PostUpdate() {
    if (netplay) {
        netplay->client.PrepareForCallbacks();
    }
    if (server) {
        server->PrepareForCallbacks();
    }
    SteamNetworkingSockets()->RunCallbacks();

    if (netplay) {
        if (netplay->client.Step()) {
            delete netplay.release();
        }
    }

    if (server) {
        if (server->Step()) {
            delete server.release();
        }
    }

    if (fSystem::ggpo) {
        ggpo_idle(fSystem::ggpo, 1);
    }

    rUserApp::staticMethods.Steam_PostUpdate();
}
