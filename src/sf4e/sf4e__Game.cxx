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

uint32_t fKey::ChecksumNormalized(const rKey* key, sf4e::Game::Hash::PointerNormalizer& normalizer) {
    size_t dataSize = GetMementoDataSize(key);
    if (dataSize == 0) {
        return 0;
    }
    return normalizer.Hash(key->mementos, dataSize, 0x5f4e0001);
}

namespace {
    // Classification of each 64KB block of the user address space.
    enum BlockKind : uint8_t {
        BK_UNKNOWN = 0,
        BK_HEAP = 1,     // committed, private: addresses here move between saves
        BK_OTHER = 2,    // free, reserved, image-backed or mapped
    };
    const uint32_t BLOCK_SHIFT = 16;
    const uint32_t NUM_BLOCKS = 1 << (32 - BLOCK_SHIFT);
    const uint32_t MIN_USER_ADDRESS = 0x00010000;
    const uint32_t MAX_USER_ADDRESS = 0x7ffeffff;

    // Marks a normalized pointer slot. Chosen to sit outside the user address
    // space so it can't collide with an ordinary pointer value.
    const uint32_t POINTER_TAG = 0xb01dfa00;

    uint8_t g_blockKinds[NUM_BLOCKS];

    // Fills the cache for the region containing `address`. VirtualQuery reports
    // a whole run of same-attribute pages at once, so one call typically
    // classifies many blocks.
    uint8_t ClassifyBlock(uint32_t address) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)address, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            g_blockKinds[address >> BLOCK_SHIFT] = BK_OTHER;
            return BK_OTHER;
        }

        const DWORD unreadable = PAGE_NOACCESS | PAGE_GUARD;
        bool isHeap =
            mbi.State == MEM_COMMIT &&
            mbi.Type == MEM_PRIVATE &&
            (mbi.Protect & unreadable) == 0;
        uint8_t kind = isHeap ? BK_HEAP : BK_OTHER;

        uint32_t base = (uint32_t)mbi.BaseAddress;
        uint32_t last = base + (uint32_t)mbi.RegionSize - 1;
        for (uint32_t block = base >> BLOCK_SHIFT; block <= (last >> BLOCK_SHIFT); block++) {
            g_blockKinds[block] = kind;
            if (block == NUM_BLOCKS - 1) {
                break;
            }
        }
        return kind;
    }
}

void sf4e::Game::Hash::PointerNormalizer::ResetBlockCache() {
    memset(g_blockKinds, BK_UNKNOWN, sizeof(g_blockKinds));
}

bool sf4e::Game::Hash::PointerNormalizer::IsHeapPointer(uint32_t v) {
    if (v < MIN_USER_ADDRESS || v > MAX_USER_ADDRESS) {
        return false;
    }
    uint8_t kind = g_blockKinds[v >> BLOCK_SHIFT];
    if (kind == BK_UNKNOWN) {
        kind = ClassifyBlock(v);
    }
    return kind == BK_HEAP;
}

void sf4e::Game::Hash::PointerNormalizer::Reset() {
    ordinals.clear();
}

uint32_t sf4e::Game::Hash::PointerNormalizer::Hash(const void* data, size_t len, uint32_t seed) {
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = seed;

    size_t nwords = len / 4;
    for (size_t i = 0; i < nwords; i++) {
        uint32_t k;
        memcpy(&k, p + i * 4, 4);
        if (IsHeapPointer(k)) {
            // Replace the address with a tagged ordinal, assigned in order of
            // first appearance. Identical states produce identical sequences
            // regardless of where the allocator happened to put the objects.
            auto found = ordinals.find(k);
            uint32_t ordinal;
            if (found == ordinals.end()) {
                ordinal = (uint32_t)ordinals.size();
                ordinals.emplace(k, ordinal);
            }
            else {
                ordinal = found->second;
            }
            k = POINTER_TAG ^ ordinal;
        }
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }

    const uint8_t* tail = p + nwords * 4;
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
