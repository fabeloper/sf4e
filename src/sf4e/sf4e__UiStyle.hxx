#pragma once

#include <cfloat>

#include <imgui.h>

namespace sf4e {
	// The look shared by the lobby and the in-match HUD: the game's own
	// palette and its slanted menu bars.
	namespace Ui {
		const ImU32 INK        = IM_COL32(8, 8, 10, 255);
		const ImU32 INK_SOFT   = IM_COL32(20, 20, 24, 255);
		const ImU32 RED        = IM_COL32(200, 16, 46, 255);
		const ImU32 RED_DEEP   = IM_COL32(120, 8, 26, 255);
		const ImU32 BLUE       = IM_COL32(40, 110, 220, 255);
		const ImU32 PAPER      = IM_COL32(240, 236, 226, 255);
		const ImU32 PAPER_DIM  = IM_COL32(200, 196, 186, 255);
		const ImU32 GOLD       = IM_COL32(242, 193, 78, 255);
		const ImU32 GREEN      = IM_COL32(88, 214, 120, 255);
		const ImU32 SHADE      = IM_COL32(0, 0, 0, 150);
		const ImU32 CARD       = IM_COL32(255, 255, 255, 18);
		const ImU32 CARD_EDGE  = IM_COL32(255, 255, 255, 70);

		inline void TextOutlined(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, const char* text, ImU32 col, float outline = 2.0f) {
			for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) {
				if (dx == 0 && dy == 0) continue;
				dl->AddText(font, size, ImVec2(pos.x + dx * outline, pos.y + dy * outline), INK, text);
			}
			dl->AddText(font, size, pos, col, text);
		}

		inline ImVec2 TextSize(ImFont* font, float size, const char* text) {
			return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
		}

		inline void TextCentered(ImDrawList* dl, ImFont* font, float size, float cx, float y, const char* text, ImU32 col, bool outline = true) {
			ImVec2 sz = TextSize(font, size, text);
			ImVec2 pos(cx - sz.x * 0.5f, y);
			if (outline) TextOutlined(dl, font, size, pos, text, col); else dl->AddText(font, size, pos, col, text);
		}

		inline void Slant(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float skew = 12.0f) {
			dl->AddQuadFilled(ImVec2(a.x + skew, a.y), ImVec2(b.x + skew, a.y), ImVec2(b.x, b.y), ImVec2(a.x, b.y), col);
		}
	}
}
