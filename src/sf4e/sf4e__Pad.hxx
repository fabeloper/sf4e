#pragma once

#include "../Dimps/Dimps__Pad.hxx"

namespace sf4e {
	namespace Pad {
		void Install();

		struct System : Dimps::Pad::System
		{
			struct Inputs {
				unsigned int mappedOn;
				unsigned int rawOn;
			};

			static const int PLAYBACK_MAX = 60;
			static Inputs playbackData[PLAYBACK_MAX][2];
			static int playbackFrame;
			// While the lobby scene is open the game must not see the pad, or
			// the main menu behind it reacts to every press.
			static bool bSuppressGameInput;
			static void Install();

			unsigned int GetButtons_RawOn(int pindex);
			unsigned int GetButtons_MappedOn(int pindex);
			// Edge reads. The battle derives what it needs from the two above,
			// but the menus navigate on these, so they must be gated too.
			unsigned int GetButtons_RawRising(int pindex);
			unsigned int GetButtons_RawFalling(int pindex);
			unsigned int GetButtons_RawRisingWithRepeat(int pindex);
		};
	}
}