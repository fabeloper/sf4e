#pragma once

#include <cstdint>
#include <set>

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
			static uint32_t Checksum(const Dimps::Game::GameMementoKey* key);

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