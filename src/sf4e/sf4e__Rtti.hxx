#pragma once

#include <string>

namespace sf4e {
	// SF4 ships with MSVC RTTI intact, including the original Dimps class
	// hierarchy, so any polymorphic object can name itself at runtime. That
	// turns opaque diagnostics ("key #6 at 0x0FC2FD40 differs") into readable
	// ones ("Chara::Actor differs"), which matters a great deal when chasing
	// state that the save/restore path misses.
	namespace Rtti {
		// Best-effort class name for a polymorphic object, e.g.
		// "Game::Battle::Chara::Actor". Returns an empty string when the
		// pointer isn't a readable polymorphic object, so it is safe to call
		// on anything. Results are cached per vtable.
		//
		// The leading "Dimps::" is dropped: every class in the game carries it.
		const std::string& GetClassName(const void* obj);
	}
}
