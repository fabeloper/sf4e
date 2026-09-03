#include <set>

#include <windows.h>
#include <detours/detours.h>
#include "spdlog/spdlog.h"

#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "sf4e__Game.hxx"
#include "sf4e__Game__Battle.hxx"

namespace rGame = Dimps::Game;
using rSpriteNode = Dimps::Eva::IEmSpriteNode;
using rKey = rGame::GameMementoKey;
using rControl = rGame::Sprite::Control;
using rSingleNodeControl = rGame::Sprite::SingleNodeControl;

namespace fGame = sf4e::Game;
using fKey = fGame::GameMementoKey;
using fSingleNodeControl = fGame::Sprite::SingleNodeControl;

std::set<rKey*> fKey::trackedKeys;

void fGame::Install() {
    Battle::Install();
    GameMementoKey::Install();
}

void fKey::Install() {
    void (fKey::* _fInitialize)(void*, int) = &Initialize;
    void (fKey:: * _fClearKey)() = &ClearKey;
    DetourAttach((PVOID*)&rKey::publicMethods.Initialize, *(PVOID*)&_fInitialize);
    DetourAttach((PVOID*)&rKey::publicMethods.ClearKey, *(PVOID*)&_fClearKey);
}

void fKey::Initialize(void* mementoable, int numMementos) {
    (this->*rKey::publicMethods.Initialize)(mementoable, numMementos);
    trackedKeys.insert(this);
}

// Layout of the key's allocation, from GameMementoKey::Initialize (0x52fd40):
//
//   mementoSize   = mementoable->GetMementoSize()
//   sizeAllocated = (mementoSize + sizeof(Metadata)) * numMementos
//   mementos      = 16-byte aligned allocation of sizeAllocated bytes, zeroed
//   metadata      = mementos + mementoSize * numMementos
//
// i.e. the per-memento data comes first, followed by the Metadata table
// ({id, pointer-to-memento} per slot) at the tail of the same buffer.
// The buffer is zeroed by the game, so struct padding inside mementos is
// deterministic. The metadata table holds pointers into a fresh allocation
// every time the key is initialized, so it must stay out of any checksum.

size_t fKey::GetBufferSize(const rKey* key) {
    if (key->sizeAllocated <= 0) {
        return 0;
    }
    return (size_t)key->sizeAllocated;
}

size_t fKey::GetMementoDataSize(const rKey* key) {
    if (key->mementos == nullptr || key->metadata == nullptr) {
        return 0;
    }
    const uint8_t* begin = (const uint8_t*)key->mementos;
    const uint8_t* end = (const uint8_t*)key->metadata;
    if (end <= begin || (size_t)(end - begin) > GetBufferSize(key)) {
        return 0;
    }
    return (size_t)(end - begin);
}

uint32_t fKey::Checksum(const rKey* key) {
    size_t dataSize = GetMementoDataSize(key);
    if (dataSize == 0) {
        return 0;
    }
    return sf4e::Game::Hash::Bytes(key->mementos, dataSize, 0x5f4e0001);
}

uint32_t sf4e::Game::Hash::Bytes(const void* data, size_t len, uint32_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;
    uint32_t h = seed;

    size_t nblocks = len / 4;
    for (size_t i = 0; i < nblocks; i++) {
        uint32_t k;
        memcpy(&k, p + i * 4, 4);
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }

    const uint8_t* tail = p + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
    case 3: k1 ^= (uint32_t)tail[2] << 16;
    case 2: k1 ^= (uint32_t)tail[1] << 8;
    case 1: k1 ^= (uint32_t)tail[0];
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;
        h ^= k1;
    }

    h ^= (uint32_t)len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

uint32_t sf4e::Game::Hash::Mix(uint32_t h, uint32_t v) {
    return Bytes(&v, sizeof(v), h);
}

void fKey::ClearKey() {
    (this->*rKey::publicMethods.ClearKey)();
    trackedKeys.erase((rKey*)this);
}

void fSingleNodeControl::RecordToAdditionalMemento(rSingleNodeControl* c, AdditionalMemento& m) {
    Dimps::Eva::IEmSpriteNode* n = *rSingleNodeControl::GetSpriteNode(c);
    m.enabled = *(rControl::GetEnabled(c));
    m.currentFrame = *rSingleNodeControl::GetCurrentFrame(c);

    // DO NOT save or restore the sprite node- this will be handled by
    // the GFxApp's save state.
}

void fSingleNodeControl::RestoreFromAdditionalMemento(rSingleNodeControl* c, const AdditionalMemento& m) {
    Dimps::Eva::IEmSpriteNode* n = *rSingleNodeControl::GetSpriteNode(c);

    bool currentlyEnabled = *(rControl::GetEnabled(c));
    if (currentlyEnabled && !m.enabled) {
        (c->*rControl::publicMethods.Disable_0x57bd80)();
    } else if (!currentlyEnabled && m.enabled) {
        (c->*rControl::publicMethods.Enable_0x577910)();
    }
    *rSingleNodeControl::GetCurrentFrame(c) = m.currentFrame;

    // DO NOT save or restore the sprite node- this will be handled by
    // the GFxApp's save state.
}
