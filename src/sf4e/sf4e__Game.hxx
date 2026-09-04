#pragma once

#include <cstdint>
#include <set>
#include <unordered_map>

#include "../Dimps/Dimps__Game.hxx"

#include "sf4e.hxx"

namespace sf4e {
	namespace Game {
		void Install();

		// Small, fast, non-cryptographic hashing used for save-state checksums.
		// MurmurHash3 x86_32. Deterministic across processes for identical bytes.
		namespace Hash {
			uint32_t Bytes(const void* data, size_t len, uint32_t seed);
			uint32_t Mix(uint32_t h, uint32_t v);

			// Mementos embed raw heap addresses: the objects they describe are
			// reallocated on every save, so two byte-identical game states hash
			// differently. Worse, addresses never agree between two machines, so
			// a raw byte checksum can't detect desyncs online at all.
			//
			// The normalizer replaces each heap address with the ordinal of its
			// first appearance within the save. That is address-independent while
			// still capturing aliasing (two fields pointing at the same object,
			// or a field becoming null). Pointers into the executable image are
			// left alone: SF4 has no ASLR, so they are stable both across saves
			// and across machines, and hashing them raw keeps more signal.
			//
			// Misclassifying a value as a pointer only costs sensitivity; it can
			// never invent a mismatch, because both sides normalize identically.
			struct PointerNormalizer {
				std::unordered_map<uint32_t, uint32_t> ordinals;

				// Forget ordinals; call once per save, before hashing anything.
				void Reset();

				// Hash `len` bytes, normalizing any 4-byte aligned word that
				// points into committed private memory.
				uint32_t Hash(const void* data, size_t len, uint32_t seed);

				// True if `v` addresses committed, private (non-image) memory.
				// Results are cached per 64KB block, so repeated lookups are a
				// single array index.
				static bool IsHeapPointer(uint32_t v);

				// Drop the block cache. Call when starting a session, so that
				// memory freed by a previous battle isn't remembered.
				static void ResetBlockCache();
			};
		}

		struct GameMementoKey : Dimps::Game::GameMementoKey
		{
			void Initialize(void* mementoable, int numMementos);
			void ClearKey();
			static void Install();

			// Size in bytes of the key's whole allocation (`sizeAllocated`),
			// which holds the memento data followed by the metadata table.
			static size_t GetBufferSize(const Dimps::Game::GameMementoKey* key);

			// Size in bytes of just the memento data: mementoSize * numMementos,
			// the region between `mementos` and `metadata`.
			static size_t GetMementoDataSize(const Dimps::Game::GameMementoKey* key);

			// Checksum of the key's memento data. The game zeroes the buffer
			// when a key is initialized, so padding hashes deterministically;
			// the metadata table is excluded because it holds pointers.
			//
			// The raw variant hashes bytes as they are, and so is only
			// comparable within a single save. The normalized variant is the
			// one to compare across saves, rollbacks and machines.
			static uint32_t Checksum(const Dimps::Game::GameMementoKey* key);
			static uint32_t ChecksumNormalized(
				const Dimps::Game::GameMementoKey* key,
				Hash::PointerNormalizer& normalizer
			);

			static std::set<Dimps::Game::GameMementoKey*> trackedKeys;
		};

		namespace Sprite {
			struct Control : Dimps::Game::Sprite::Control {
				// These methods cannot be safely implemented! The ::Control class
				// has several unimplemented virtual methods, and all subclasses
				// of Control implement Enable() and Disable(), which would need
				// to be called as part of these methods.
				//
				// static void RecordToAdditionalMemento(Dimps::Game::Sprite::SingleNodeControl* n, AdditionalMemento& m);
				// static void RestoreFromAdditionalMemento(Dimps::Game::Sprite::SingleNodeControl* n, const AdditionalMemento& m);
			};

			struct SingleNodeControl : Dimps::Game::Sprite::SingleNodeControl {
				struct AdditionalMemento {
					bool enabled;
					int currentFrame;
				};

				static void RecordToAdditionalMemento(Dimps::Game::Sprite::SingleNodeControl* n, AdditionalMemento& m);
				static void RestoreFromAdditionalMemento(Dimps::Game::Sprite::SingleNodeControl* n, const AdditionalMemento& m);
			};
		}
	}
}