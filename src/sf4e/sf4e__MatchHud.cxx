#include <cstdio>
#include <cstring>
#include <mutex>

#include <windows.h>
#include <imgui.h>

#include "sf4e__Lobby.hxx"
#include "sf4e__MatchHud.hxx"
#include "sf4e__UiStyle.hxx"

using namespace sf4e::Ui;

namespace {
	const size_t NAME_CAPACITY = 32;
	const int PING_REFRESH_TICKS = 30;
	const ULONGLONG WAITING_NOTICE_AFTER_MS = 400;
	const int PING_GOOD_MS = 80;
	const int PING_FAIR_MS = 140;

	struct MatchState {
		bool active = false;
		bool fighting = false;
		char names[2][NAME_CAPACITY] = {};
		int inputDelay = 0;
		bool direct = false;
		int pingMs = 0;
		int ticksUntilPingRefresh = 0;
		ULONGLONG waitingSince = 0;
	};

	std::mutex g_mutex;
	MatchState g_match;

	// Positions as fractions of the 16:9 frame the game lays its HUD out in.
	struct Layout {
		float nameInsetX = 0.082f;
		float nameY = 0.135f;
		float nameSize = 0.036f;
		float connectionY = 0.168f;
		float connectionSize = 0.026f;
		float waitingY = 0.30f;
	};
	Layout g_layout;
	bool g_preview = false;

	struct Frame {
		ImVec2 origin;
		ImVec2 size;
		ImVec2 At(float x, float y) const { return ImVec2(origin.x + size.x * x, origin.y + size.y * y); }
	};

	Frame HudFrame(ImVec2 display) {
		Frame frame;
		const float aspect = 16.0f / 9.0f;
		if (display.x / display.y > aspect) {
			frame.size = ImVec2(display.y * aspect, display.y);
		}
		else {
			frame.size = ImVec2(display.x, display.x / aspect);
		}
		frame.origin = ImVec2((display.x - frame.size.x) * 0.5f, (display.y - frame.size.y) * 0.5f);
		return frame;
	}

	MatchState PreviewState() {
		MatchState preview;
		preview.active = true;
		preview.fighting = true;
		strcpy_s(preview.names[0], "PLAYER ONE");
		strcpy_s(preview.names[1], "PLAYER TWO");
		preview.inputDelay = 2;
		preview.direct = true;
		preview.pingMs = 48;
		return preview;
	}

	void DrawName(ImDrawList* dl, const Frame& frame, const char* name, bool rightSide) {
		ImFont* font = sf4e::Lobby::HeadFont();
		float size = frame.size.y * g_layout.nameSize;
		ImVec2 textSize = TextSize(font, size, name);
		ImVec2 anchor = frame.At(rightSide ? 1.0f - g_layout.nameInsetX : g_layout.nameInsetX, g_layout.nameY);
		float left = rightSide ? anchor.x - textSize.x : anchor.x;
		float padX = size * 0.45f;
		float accent = size * 0.18f;

		ImVec2 plateMin(left - padX, anchor.y - size * 0.10f);
		ImVec2 plateMax(left + textSize.x + padX, anchor.y + size * 1.12f);
		Slant(dl, plateMin, plateMax, SHADE, size * 0.3f);
		if (rightSide) {
			Slant(dl, ImVec2(plateMax.x - accent, plateMin.y), plateMax, BLUE, size * 0.3f);
		}
		else {
			Slant(dl, plateMin, ImVec2(plateMin.x + accent, plateMax.y), RED, size * 0.3f);
		}
		TextOutlined(dl, font, size, ImVec2(left, anchor.y), name, PAPER, size * 0.06f);
	}

	ImU32 PingColor(int pingMs) {
		if (pingMs <= PING_GOOD_MS) return GREEN;
		if (pingMs <= PING_FAIR_MS) return GOLD;
		return RED;
	}

	void DrawConnection(ImDrawList* dl, const Frame& frame, const MatchState& match) {
		ImFont* font = sf4e::Lobby::BodyFont();
		float size = frame.size.y * g_layout.connectionSize;
		char ping[24], rest[48];
		snprintf(ping, sizeof(ping), "%d ms", match.pingMs);
		snprintf(rest, sizeof(rest), "   DELAY %d   %s", match.inputDelay, match.direct ? "P2P" : "RELAY");

		ImVec2 pingSize = TextSize(font, size, ping);
		ImVec2 restSize = TextSize(font, size, rest);
		float dot = size * 0.5f;
		float width = dot * 1.6f + pingSize.x + restSize.x;
		ImVec2 at = frame.At(0.5f, g_layout.connectionY);
		float x = at.x - width * 0.5f;
		float padX = size * 0.6f;

		Slant(dl, ImVec2(x - padX, at.y - size * 0.12f), ImVec2(x + width + padX, at.y + size * 1.15f), SHADE, size * 0.3f);
		dl->AddCircleFilled(ImVec2(x + dot * 0.5f, at.y + size * 0.55f), dot * 0.5f, PingColor(match.pingMs));
		x += dot * 1.6f;
		dl->AddText(font, size, ImVec2(x, at.y), PingColor(match.pingMs), ping);
		dl->AddText(font, size, ImVec2(x + pingSize.x, at.y), PAPER_DIM, rest);
	}

	void DrawWaitingNotice(ImDrawList* dl, const Frame& frame) {
		ImFont* font = sf4e::Lobby::HeadFont();
		float size = frame.size.y * 0.045f;
		const char* text = "WAITING FOR OPPONENT";
		ImVec2 textSize = TextSize(font, size, text);
		ImVec2 at = frame.At(0.5f, g_layout.waitingY);
		Slant(dl, ImVec2(at.x - textSize.x * 0.5f - size, at.y - size * 0.2f), ImVec2(at.x + textSize.x * 0.5f + size, at.y + size * 1.3f), SHADE, size * 0.3f);
		TextCentered(dl, font, size, at.x, at.y, text, GOLD);
	}
}

void sf4e::MatchHud::BeginMatch(const std::string& p1Name, const std::string& p2Name, int inputDelay, bool direct) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_match = MatchState();
	g_match.active = true;
	strncpy_s(g_match.names[0], p1Name.c_str(), _TRUNCATE);
	strncpy_s(g_match.names[1], p2Name.c_str(), _TRUNCATE);
	g_match.inputDelay = inputDelay;
	g_match.direct = direct;
}

void sf4e::MatchHud::Tick(bool fighting, int pingMs, bool waitingForOpponent) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_match.active) {
		return;
	}
	g_match.fighting = fighting;
	if (--g_match.ticksUntilPingRefresh <= 0) {
		g_match.pingMs = pingMs;
		g_match.ticksUntilPingRefresh = PING_REFRESH_TICKS;
	}
	if (!waitingForOpponent) {
		g_match.waitingSince = 0;
	}
	else if (g_match.waitingSince == 0) {
		g_match.waitingSince = GetTickCount64();
	}
}

void sf4e::MatchHud::EndMatch() {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_match = MatchState();
}

void sf4e::MatchHud::Draw() {
	MatchState match;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		match = g_match;
	}
	if (g_preview) {
		match = PreviewState();
	}
	if (!match.active) {
		return;
	}

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	Frame frame = HudFrame(ImGui::GetIO().DisplaySize);
	if (match.fighting) {
		DrawName(dl, frame, match.names[0], false);
		DrawName(dl, frame, match.names[1], true);
		if (match.inputDelay > 0) {
			DrawConnection(dl, frame, match);
		}
	}
	if (match.waitingSince != 0 && GetTickCount64() - match.waitingSince >= WAITING_NOTICE_AFTER_MS) {
		DrawWaitingNotice(dl, frame);
	}
}

void sf4e::MatchHud::DrawTuningWindow(bool* open) {
	if (ImGui::Begin("Match HUD", open)) {
		ImGui::Checkbox("Preview with sample data", &g_preview);
		ImGui::SliderFloat("Name inset X", &g_layout.nameInsetX, 0.0f, 0.5f, "%.3f");
		ImGui::SliderFloat("Name Y", &g_layout.nameY, 0.0f, 0.5f, "%.3f");
		ImGui::SliderFloat("Name size", &g_layout.nameSize, 0.01f, 0.08f, "%.3f");
		ImGui::SliderFloat("Connection Y", &g_layout.connectionY, 0.0f, 0.5f, "%.3f");
		ImGui::SliderFloat("Connection size", &g_layout.connectionSize, 0.01f, 0.06f, "%.3f");
		ImGui::SliderFloat("Waiting notice Y", &g_layout.waitingY, 0.0f, 0.9f, "%.3f");
	}
	ImGui::End();
}
