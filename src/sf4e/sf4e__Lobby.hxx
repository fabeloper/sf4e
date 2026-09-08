#pragma once

struct ImGuiIO;

namespace sf4e {
	// The player-facing lobby: a full-screen scene drawn over the game's main
	// menu, styled after the game itself and driven from the controller.
	//
	// It replaces the developer overlay's Network window for players. The
	// flow is Home (create / join / settings) -> Join (code entry) ->
	// Lobby (character, options, ready) -> the match, and back to the Lobby
	// for a rematch. While it is open the game's own menu stops receiving
	// pad input, so nothing behind it reacts.
	namespace Lobby {
		// Called once per ImGui context. Loads the display fonts from the
		// fonts every Windows install ships with; falls back to ImGui's own.
		void LoadFonts(ImGuiIO& io);

		// Show the scene. Only meaningful on the main menu.
		void Open();
		void Close();
		bool IsOpen();

		// Draw and process input for this frame. Safe to call every frame.
		void Draw();

		// Called by the battle system when an online match ends. `winnerSide`
		// is 0 or 1, or -1 for a draw. The lobby shows a results screen with
		// rematch / change character / leave when the game returns to the
		// main menu.
		void OnMatchResult(int winnerSide, int charaP1, int charaP2);
	}
}
