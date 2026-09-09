#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <Xinput.h>
#include <shellapi.h>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../session/sf4e__SessionClient.hxx"

#include "sf4e.hxx"
#include "sf4e__Lobby.hxx"
#include "sf4e__Matchmaker.hxx"
#include "sf4e__Pad.hxx"
#include "sf4e__UserApp.hxx"

using Dimps::App;
using Dimps::Event::EventBaseWithEC;
namespace rBattle = Dimps::Game::Battle;
using rVsMode = Dimps::GameEvents::VsMode;
using rPad = Dimps::Pad::System;
using fUserApp = sf4e::UserApp;

namespace {
	// ---------------------------------------------------------------- look
	// The game's own palette: ink black, poster red, paper white, and the
	// gold it reserves for what matters.
	const ImU32 INK        = IM_COL32(8, 8, 10, 255);
	const ImU32 INK_SOFT   = IM_COL32(20, 20, 24, 255);
	const ImU32 RED        = IM_COL32(200, 16, 46, 255);
	const ImU32 RED_DEEP   = IM_COL32(120, 8, 26, 255);
	const ImU32 PAPER      = IM_COL32(240, 236, 226, 255);
	const ImU32 PAPER_DIM  = IM_COL32(200, 196, 186, 255);
	const ImU32 GOLD       = IM_COL32(242, 193, 78, 255);
	const ImU32 GREEN      = IM_COL32(88, 214, 120, 255);
	const ImU32 SHADE      = IM_COL32(0, 0, 0, 150);
	const ImU32 CARD       = IM_COL32(255, 255, 255, 18);
	const ImU32 CARD_EDGE  = IM_COL32(255, 255, 255, 70);

	ImFont* g_fontTitle = nullptr;   // Impact, huge
	ImFont* g_fontHead = nullptr;    // Impact, headings and menu
	ImFont* g_fontBody = nullptr;    // Segoe UI Semibold
	ImFont* g_fontSmall = nullptr;   // Segoe UI

	// ---------------------------------------------------------------- state
	enum Screen {
		SC_CAPTURE,     // press Start on the controller you want to use
		SC_HOME,
		SC_JOIN,
		SC_CONNECTING,  // matchmaker + session connect in flight
		SC_LOBBY,
		SC_RESULT,      // after a match: rematch / change character / leave
	};

	// Last match result, set from the battle system, consumed when the lobby
	// comes back after the match.
	bool g_hasResult = false;
	int g_resultWinner = -1;     // side that won, or -1 for a draw
	int g_resultChara[2] = { -1, -1 };
	int g_mySideAtResult = -1;   // sides can rotate after reporting; keep ours
	int g_resultCursor = 0;
	int g_wins = 0, g_losses = 0;

	bool g_open = false;
	bool g_hiddenForBattle = false;
	Screen g_screen = SC_CAPTURE;
	sf4e::Matchmaker g_mm;
	bool g_mmConfigured = false;
	std::string g_error;
	unsigned long long g_errorUntil = 0;

	char g_name[32] = { 0 };
	uint8_t g_deviceIdx = 0xff;
	uint8_t g_deviceType = 0xff;
	int g_delay = 2;
	uint16_t g_localGgpoPort = 23457;

	int g_homeCursor = 0;
	const int HOME_ITEMS = 4;

	std::string g_code;
	int g_joinCursor = 0;
	const char CODE_CHARS[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
	const int CODE_CHAR_COUNT = 31;
	const int JOIN_COLS = 8;
	const int JOIN_EXTRA = 3;  // DEL, JOIN, WATCH

	// Lobby screen: a character grid, then an options row, then actions.
	const int CHARA_COUNT = 0x2c;
	const int CHARA_COLS = 8;
	int g_charaCursor = 0;
	int g_lobbyRow = 0;      // 0 = grid, 1 = options, 2 = actions
	int g_optionCursor = 0;
	int g_actionCursor = 0;
	rVsMode::ConfirmedCharaConditions g_cond = { 0, 0, 0, 0, 0, 0, 0, 0, (BYTE)rBattle::ED_USF4 };
	int g_stage = 0;
	bool g_sentReady = false;
	bool g_reportedLastMatch = false;
	bool g_isCreator = false;
	bool g_spectate = false;   // joined to watch, not to play

	// Ink strokes, generated once per open so they don't flicker.
	struct Stroke { std::vector<ImVec2> pts; float thickness; ImU32 col; };
	std::vector<Stroke> g_strokes;

	// ---------------------------------------------------------------- input
	struct Input {
		bool up, down, left, right, confirm, back, alt, lb, rb, start;
		bool copy, paste;   // Ctrl+C / Ctrl+V on the keyboard
		Input() { memset(this, 0, sizeof(*this)); }
	};
	WORD g_prevPad = 0;
	bool g_prevKeys[256] = { false };
	unsigned long long g_repeatAt = 0;
	int g_heldDir = -1;

	bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

	// XInput and GetAsyncKeyState report the devices regardless of which
	// window is active, so without this a message typed into Discord with the
	// game behind it walks the lobby. Only the game's own window counts.
	bool GameHasFocus() {
		HWND fg = GetForegroundWindow();
		if (!fg) return false;
		DWORD pid = 0;
		GetWindowThreadProcessId(fg, &pid);
		return pid == GetCurrentProcessId();
	}
	bool g_hadFocus = false;

	// Edge-triggered buttons; directions repeat while held.
	Input ReadInput() {
		Input in;
		XINPUT_STATE xs;
		WORD pad = 0;
		SHORT lx = 0, ly = 0;

		bool focus = GameHasFocus();
		if (!focus) {
			g_hadFocus = false;
			g_heldDir = -1;
			return in;
		}
		if (!g_hadFocus) {
			// Focus just came back: take the current state as the baseline so
			// whatever is held right now doesn't register as a fresh press.
			g_hadFocus = true;
			for (DWORD i = 0; i < 4; i++) {
				if (XInputGetState(i, &xs) == ERROR_SUCCESS) pad |= xs.Gamepad.wButtons;
			}
			g_prevPad = pad;
			for (int k = 8; k < 256; k++) g_prevKeys[k] = KeyDown(k);
			g_heldDir = -1;
			return in;
		}
		pad = 0;
		for (DWORD i = 0; i < 4; i++) {
			if (XInputGetState(i, &xs) == ERROR_SUCCESS) {
				pad |= xs.Gamepad.wButtons;
				if (std::abs(xs.Gamepad.sThumbLX) > std::abs(lx)) lx = xs.Gamepad.sThumbLX;
				if (std::abs(xs.Gamepad.sThumbLY) > std::abs(ly)) ly = xs.Gamepad.sThumbLY;
			}
		}
		const SHORT dead = 16000;
		bool dUp = (pad & XINPUT_GAMEPAD_DPAD_UP) || ly > dead || KeyDown(VK_UP);
		bool dDown = (pad & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -dead || KeyDown(VK_DOWN);
		bool dLeft = (pad & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -dead || KeyDown(VK_LEFT);
		bool dRight = (pad & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > dead || KeyDown(VK_RIGHT);

		int dir = dUp ? 0 : dDown ? 1 : dLeft ? 2 : dRight ? 3 : -1;
		unsigned long long now = GetTickCount64();
		bool fire = false;
		if (dir != g_heldDir) {
			g_heldDir = dir;
			fire = dir >= 0;
			g_repeatAt = now + 340;
		}
		else if (dir >= 0 && now >= g_repeatAt) {
			fire = true;
			g_repeatAt = now + 110;
		}
		if (fire) {
			in.up = dir == 0; in.down = dir == 1; in.left = dir == 2; in.right = dir == 3;
		}

		#define EDGE_PAD(mask) (((pad & (mask)) != 0) && ((g_prevPad & (mask)) == 0))
		#define EDGE_KEY(vk) (KeyDown(vk) && !g_prevKeys[vk])
		in.confirm = EDGE_PAD(XINPUT_GAMEPAD_A) || EDGE_KEY(VK_RETURN) || EDGE_KEY(VK_SPACE);
		in.back = EDGE_PAD(XINPUT_GAMEPAD_B) || EDGE_KEY(VK_ESCAPE) || EDGE_KEY(VK_BACK);
		in.alt = EDGE_PAD(XINPUT_GAMEPAD_Y) || EDGE_KEY(VK_TAB);
		in.lb = EDGE_PAD(XINPUT_GAMEPAD_LEFT_SHOULDER) || EDGE_KEY(VK_PRIOR);
		in.rb = EDGE_PAD(XINPUT_GAMEPAD_RIGHT_SHOULDER) || EDGE_KEY(VK_NEXT);
		in.start = EDGE_PAD(XINPUT_GAMEPAD_START) || EDGE_KEY(VK_F1);
		bool ctrl = KeyDown(VK_CONTROL);
		in.copy = ctrl && EDGE_KEY('C');
		in.paste = ctrl && EDGE_KEY('V');
		#undef EDGE_PAD
		#undef EDGE_KEY

		g_prevPad = pad;
		for (int k = 8; k < 256; k++) g_prevKeys[k] = KeyDown(k);
		return in;
	}

	// Typed characters for the code screen, so keyboard users just type.
	void Flash(const char* msg, bool good = false);

	// Appends valid code characters from `text`, uppercased, up to six.
	void AppendCodeText(const char* text) {
		for (const char* p = text; p && *p && g_code.size() < 6; p++) {
			char c = (char)toupper((unsigned char)*p);
			if (strchr(CODE_CHARS, c) && c != 0) g_code += c;
		}
	}

	void PasteCode() {
		const char* clip = ImGui::GetClipboardText();
		if (!clip || !*clip) { Flash("Nothing to paste"); return; }
		g_code.clear();
		AppendCodeText(clip);
		if (g_code.empty()) Flash("The clipboard doesn't hold a lobby code");
	}

	void ReadTypedCode() {
		if (!GameHasFocus() || !g_hadFocus) return;
		if (KeyDown(VK_CONTROL)) return;  // Ctrl+V is a paste, not a letter
		for (int i = 0; i < CODE_CHAR_COUNT; i++) {
			char c = CODE_CHARS[i];
			int vk = (c >= '0' && c <= '9') ? c : c;  // letters and digits map to their VK codes
			if ((GetAsyncKeyState(vk) & 0x8000) && !g_prevKeys[vk] && g_code.size() < 6) {
				g_code += c;
			}
		}
	}

	// ---------------------------------------------------------------- draw helpers
	void TextOutlined(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, const char* text, ImU32 col, float outline = 2.0f) {
		for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) {
			if (dx == 0 && dy == 0) continue;
			dl->AddText(font, size, ImVec2(pos.x + dx * outline, pos.y + dy * outline), INK, text);
		}
		dl->AddText(font, size, pos, col, text);
	}

	ImVec2 TextSize(ImFont* font, float size, const char* text) {
		return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
	}

	void TextCentered(ImDrawList* dl, ImFont* font, float size, float cx, float y, const char* text, ImU32 col, bool outline = true) {
		ImVec2 sz = TextSize(font, size, text);
		ImVec2 pos(cx - sz.x * 0.5f, y);
		if (outline) TextOutlined(dl, font, size, pos, text, col); else dl->AddText(font, size, pos, col, text);
	}

	// A slanted highlight bar, the game's own menu idiom.
	void Slant(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float skew = 12.0f) {
		dl->AddQuadFilled(ImVec2(a.x + skew, a.y), ImVec2(b.x + skew, a.y), ImVec2(b.x, b.y), ImVec2(a.x, b.y), col);
	}

	void GenerateStrokes(ImVec2 ds) {
		g_strokes.clear();
		unsigned s = 0x5F4E;
		#define RND() (s = s * 1103515245u + 12345u, (float)((s >> 8) & 0xffff) / 65535.0f)
		for (int i = 0; i < 7; i++) {
			Stroke st;
			float x0 = RND() * ds.x, y0 = RND() * ds.y;
			float ang = (RND() - 0.5f) * 1.2f + (i % 2 ? 0.6f : -0.5f);
			float len = ds.x * (0.35f + RND() * 0.5f);
			int n = 14;
			for (int k = 0; k < n; k++) {
				float t = (float)k / (n - 1);
				float jitter = (RND() - 0.5f) * 26.0f;
				st.pts.push_back(ImVec2(x0 + std::cos(ang) * len * t + jitter, y0 + std::sin(ang) * len * t + jitter * 0.6f));
			}
			st.thickness = 5.0f + RND() * 16.0f;
			st.col = (i % 3 == 0) ? IM_COL32(200, 16, 46, 200) : IM_COL32(0, 0, 0, 210);
			g_strokes.push_back(st);
		}
		#undef RND
	}

	void DrawBackdrop(ImDrawList* dl, ImVec2 ds) {
		dl->AddRectFilled(ImVec2(0, 0), ds, INK);
		// The red band: a diagonal sweep across the upper half.
		dl->AddQuadFilled(ImVec2(-40, ds.y * 0.10f), ImVec2(ds.x * 0.62f, ds.y * 0.02f), ImVec2(ds.x * 0.48f, ds.y * 0.42f), ImVec2(-40, ds.y * 0.50f), RED_DEEP);
		dl->AddQuadFilled(ImVec2(-40, ds.y * 0.12f), ImVec2(ds.x * 0.58f, ds.y * 0.05f), ImVec2(ds.x * 0.45f, ds.y * 0.38f), ImVec2(-40, ds.y * 0.45f), RED);
		for (size_t i = 0; i < g_strokes.size(); i++) {
			dl->AddPolyline(g_strokes[i].pts.data(), (int)g_strokes[i].pts.size(), g_strokes[i].col, ImDrawFlags_None, g_strokes[i].thickness);
		}
		// Paper grain: faint horizontal lines.
		for (float y = 0; y < ds.y; y += 6) {
			dl->AddLine(ImVec2(0, y), ImVec2(ds.x, y), IM_COL32(255, 255, 255, 6));
		}
	}

	void DrawHeader(ImDrawList* dl, ImVec2 ds, const char* title, const char* sub) {
		TextOutlined(dl, g_fontTitle, 84, ImVec2(60, 40), title, PAPER, 3.0f);
		if (sub) dl->AddText(g_fontBody, 26, ImVec2(64, 132), PAPER_DIM, sub);
	}

	void DrawHint(ImDrawList* dl, ImVec2 ds, const char* hint) {
		Slant(dl, ImVec2(0, ds.y - 52), ImVec2(ds.x, ds.y), SHADE, 0);
		dl->AddText(g_fontBody, 22, ImVec2(60, ds.y - 40), PAPER, hint);
	}

	bool g_flashGood = false;

	void Flash(const char* msg, bool good) {
		g_error = msg;
		g_flashGood = good;
		g_errorUntil = GetTickCount64() + (good ? 2500 : 5000);
	}

	void DrawError(ImDrawList* dl, ImVec2 ds) {
		if (g_error.empty() || GetTickCount64() > g_errorUntil) return;
		ImVec2 sz = TextSize(g_fontBody, 24, g_error.c_str());
		Slant(dl, ImVec2(ds.x * 0.5f - sz.x * 0.5f - 24, ds.y - 110), ImVec2(ds.x * 0.5f + sz.x * 0.5f + 24, ds.y - 70), g_flashGood ? GREEN : RED);
		dl->AddText(g_fontBody, 24, ImVec2(ds.x * 0.5f - sz.x * 0.5f, ds.y - 102), PAPER, g_error.c_str());
	}

	// ---------------------------------------------------------------- helpers
	bool OnMainMenu() {
		char* q[1] = { "MainMenu" };
		return EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), q, 1) != nullptr;
	}

	void EnsureMatchmaker() {
		if (g_mmConfigured) return;
		g_mmConfigured = true;
		if (g_name[0] == 0 && sf4e::args.szName[0] != 0) strncpy_s(g_name, sf4e::args.szName, _TRUNCATE);
		if (g_name[0] == 0) {
			DWORD len = sizeof(g_name);
			if (!GetUserNameA(g_name, &len)) strcpy_s(g_name, "Player");
		}
		if (sf4e::args.szServer[0] != 0) g_mm.Configure(sf4e::args.szServer);
		spdlog::info("Lobby: playing as {} ({})", g_name, sf4e::args.szName[0] ? "Steam persona" : "Windows user name");
	}

	// The server keeps the players in front of the spectators.
	int MySide() {
		if (!fUserApp::netplay || g_spectate) return -1;
		sf4e::SessionClient& c = fUserApp::netplay->client;
		int n = 0;
		for (size_t i = 0; i < c._lobbyData.members.size(); i++) {
			if (c._lobbyData.members[i].spectator) continue;
			if (c._lobbyData.members[i].connId == c._cid) return n;
			n++;
		}
		return -1;
	}

	const sf4e::SessionProtocol::MemberData* PlayerAt(int side) {
		if (!fUserApp::netplay) return nullptr;
		auto& m = fUserApp::netplay->client._lobbyData.members;
		int n = 0;
		for (size_t i = 0; i < m.size(); i++) {
			if (m[i].spectator) continue;
			if (n == side) return &m[i];
			n++;
		}
		return nullptr;
	}

	const sf4e::SessionProtocol::MemberData* Me() {
		if (!fUserApp::netplay) return nullptr;
		sf4e::SessionClient& c = fUserApp::netplay->client;
		for (size_t i = 0; i < c._lobbyData.members.size(); i++) {
			if (c._lobbyData.members[i].connId == c._cid) return &c._lobbyData.members[i];
		}
		return nullptr;
	}

	std::string SpectatorNames() {
		std::string s;
		if (!fUserApp::netplay) return s;
		auto& m = fUserApp::netplay->client._lobbyData.members;
		for (size_t i = 0; i < m.size(); i++) {
			if (!m[i].spectator) continue;
			if (!s.empty()) s += ", ";
			s += m[i].name;
		}
		return s;
	}

	void LeaveLobby() {
		fUserApp::netplay.reset();
		g_mm.Cancel();
		g_screen = SC_HOME;
	}

	void SetSuppress(bool on) {
		sf4e::Pad::System::bSuppressGameInput = on;
	}

	void ConnectFromMatchmaker() {
		std::string addr = g_mm.SessionAddress();
		std::vector<char> buf(addr.begin(), addr.end());
		buf.push_back(0);
		fUserApp::StartSession(buf.data(), g_localGgpoPort, sf4e::sidecarHash, std::string(g_name), g_deviceType, g_deviceIdx, (uint8_t)g_delay, g_spectate);
		g_sentReady = false;
		g_reportedLastMatch = false;
		g_lobbyRow = 0;
	}

	const char* EditionLabel(int e) {
		switch (e) {
		case rBattle::ED_SF4: return "SF4";
		case rBattle::ED_SSF4: return "SSF4";
		case rBattle::ED_AE2011: return "AE 2011";
		case rBattle::ED_AE2012: return "AE 2012";
		case rBattle::ED_USF4: return "ULTRA";
		case rBattle::ED_OMEGA: return "OMEGA";
		default: return "?";
		}
	}

	void CycleEdition(int dir) {
		int idx = -1, n = 0, list[NUM_VALID_EDITIONS];
		for (int i = 0; i < NUM_VALID_EDITIONS; i++) {
			int e = rBattle::orderedEditions[i];
			if (rBattle::validEditionsPerChara[g_cond.charaID].valid[i]) {
				if (e == g_cond.unc_edition) idx = n;
				list[n++] = e;
			}
		}
		if (n == 0) return;
		if (idx < 0) idx = n - 1;
		idx = (idx + dir + n) % n;
		g_cond.unc_edition = (BYTE)list[idx];
	}

	void SendReady() {
		if (!fUserApp::netplay || g_sentReady) return;
		sf4e::SessionClient& c = fUserApp::netplay->client;
		if (c.PreBattle_SetChara(g_cond) != k_EResultOK) { Flash("Could not send your character"); return; }
		if (MySide() == 0) {
			c.PreBattle_SetEnv(sf4e::localRand());
			c.PreBattle_SetStage(g_stage);
		}
		c.Lobby_Ready();
		g_sentReady = true;
	}

	// ---------------------------------------------------------------- screens
	void DrawCapture(ImDrawList* dl, ImVec2 ds) {
		DrawHeader(dl, ds, "ONLINE VERSUS", "sf4e rollback");
		TextCentered(dl, g_fontHead, 44, ds.x * 0.5f, ds.y * 0.48f, "PRESS START", PAPER);
		TextCentered(dl, g_fontBody, 24, ds.x * 0.5f, ds.y * 0.48f + 56, "on the controller you want to play with", PAPER_DIM, false);
		DrawHint(dl, ds, "Keyboard: press Enter");

		rPad* p = rPad::staticMethods.GetSingleton();
		rPad::__publicMethods& m = rPad::publicMethods;
		if ((p->*m.CaptureNextMatchingPadToSide)(0, 0x1040, 0xffffffff)) {
			g_deviceIdx = (p->*m.GetDeviceIndexForPlayer)(0);
			g_deviceType = (p->*m.GetDeviceTypeForPlayer)(0);
			(p->*m.SetSideHasAssignedController)(0, 0);
			switch (g_deviceType) {
			case Dimps::Pad::PADTYPE_RAWINPUT:
				(Dimps::Pad::System_RawInput::staticMethods.GetSingleton()->*Dimps::Pad::System_RawInput::publicMethods.SetDeviceInUse)(g_deviceIdx, 0);
				break;
			case Dimps::Pad::PADTYPE_XINPUT:
				(Dimps::Pad::System_XInput::staticMethods.GetSingleton()->*Dimps::Pad::System_XInput::publicMethods.SetDeviceInUse)(g_deviceIdx, 0);
				break;
			}
			g_screen = SC_HOME;
			g_prevPad = 0xffff;  // swallow the press that got us here
		}
	}

	// Server reachability shown on the home screen. The address itself is
	// never displayed anywhere; players only ever see whether it answers.
	// true if version string `a` ("x.y.z") is strictly newer than `b`.
	bool VersionNewer(const std::string& a, const char* b) {
		if (a.empty()) return false;
		int av[3] = { 0, 0, 0 }, bv[3] = { 0, 0, 0 };
		sscanf(a.c_str(), "%d.%d.%d", &av[0], &av[1], &av[2]);
		sscanf(b, "%d.%d.%d", &bv[0], &bv[1], &bv[2]);
		for (int i = 0; i < 3; i++) {
			if (av[i] != bv[i]) return av[i] > bv[i];
		}
		return false;
	}

	int g_serverStatus = -1;        // -1 checking, 0 unreachable, 1 online
	bool g_pingInFlight = false;

	void DrawHome(ImDrawList* dl, ImVec2 ds, const Input& in) {
		EnsureMatchmaker();
		if (g_serverStatus < 0 && !g_pingInFlight && g_mm.IsConfigured()) {
			g_mm.Ping();
			g_pingInFlight = true;
		}
		if (g_pingInFlight) {
			g_mm.Poll();
			if (g_mm.state == sf4e::Matchmaker::State::Done) { g_serverStatus = 1; g_pingInFlight = false; g_mm.Cancel(); }
			else if (g_mm.state == sf4e::Matchmaker::State::Failed) { g_serverStatus = 0; g_pingInFlight = false; g_mm.Cancel(); }
		}
		const char* sub = !g_mm.IsConfigured() ? "no server configured" :
			g_serverStatus == 1 ? "SERVER ONLINE" : g_serverStatus == 0 ? "SERVER UNREACHABLE" : "checking the server...";
		DrawHeader(dl, ds, "ONLINE VERSUS", nullptr);
		ImU32 subCol = g_serverStatus == 1 ? GREEN : g_serverStatus == 0 ? RED : PAPER_DIM;
		dl->AddText(g_fontBody, 26, ImVec2(64, 132), subCol, sub);
		if (g_serverStatus == 1 && g_mm.capacity > 0) {
			char cap[48];
			snprintf(cap, sizeof(cap), "   %d of %d lobbies in use", g_mm.lobbiesInUse, g_mm.capacity);
			dl->AddText(g_fontSmall, 20, ImVec2(64 + TextSize(g_fontBody, 26, sub).x, 136), PAPER_DIM, cap);
		}

		// Update notice: the server runs a newer build than this one, so the
		// player is behind and cannot join its lobbies until they update.
		// The address is never shown; only the version and a link.
		bool updateAvailable = g_serverStatus == 1 && VersionNewer(g_mm.serverVersion, SF4E_VERSION);
		if (updateAvailable) {
			char msg[112];
			snprintf(msg, sizeof(msg), "UPDATE AVAILABLE   v%s   (you have v%s)   -   press Y to download",
				g_mm.serverVersion.c_str(), SF4E_VERSION);
			Slant(dl, ImVec2(40, 168), ImVec2(ds.x - 40, 210), GOLD, 8);
			dl->AddText(g_fontBody, 24, ImVec2(66, 176), INK, msg);
			if (in.alt) {
				ShellExecuteA(NULL, "open", "https://github.com/fabeloper/sf4e/releases/latest", NULL, NULL, SW_SHOWNORMAL);
				Flash("Opening the download page in your browser", true);
			}
		}

		char delayLabel[32];
		snprintf(delayLabel, sizeof(delayLabel), "INPUT DELAY   <  %d  >", g_delay);
		const char* items[HOME_ITEMS] = { "CREATE LOBBY", "JOIN WITH CODE", delayLabel, "BACK TO GAME" };
		float y = ds.y * 0.36f;
		for (int i = 0; i < HOME_ITEMS; i++) {
			bool sel = i == g_homeCursor;
			if (sel) Slant(dl, ImVec2(40, y - 6), ImVec2(560, y + 52), RED);
			TextOutlined(dl, g_fontHead, 40, ImVec2(80, y), items[i], sel ? PAPER : PAPER_DIM);
			y += 74;
		}
		dl->AddText(g_fontSmall, 20, ImVec2(80, y + 10), PAPER_DIM, ("Playing as " + std::string(g_name)).c_str());
		DrawHint(dl, ds, "Up/Down: choose     A: confirm     Left/Right: change delay     B: back to game");

		if (in.up) g_homeCursor = (g_homeCursor + HOME_ITEMS - 1) % HOME_ITEMS;
		if (in.down) g_homeCursor = (g_homeCursor + 1) % HOME_ITEMS;
		if (g_homeCursor == 2) {
			if (in.left && g_delay > 0) g_delay--;
			if (in.right && g_delay < 8) g_delay++;
		}
		if (in.back) { sf4e::Lobby::Close(); return; }
		if (in.confirm) {
			switch (g_homeCursor) {
			case 0:
				if (!g_mm.IsConfigured()) { Flash("No lobby server configured"); break; }
				g_isCreator = true;
				g_spectate = false;
				g_mm.Create(sf4e::sidecarHash, g_name);
				g_screen = SC_CONNECTING;
				break;
			case 1:
				g_code.clear(); g_joinCursor = 0; g_isCreator = false;
				g_screen = SC_JOIN;
				break;
			case 3:
				sf4e::Lobby::Close();
				break;
			}
		}
	}

	void DrawJoin(ImDrawList* dl, ImVec2 ds, const Input& in) {
		DrawHeader(dl, ds, "JOIN", "type the code your opponent gave you");
		ReadTypedCode();

		// The code so far, as six slots.
		float slotW = 70, gap = 12, total = 6 * slotW + 5 * gap;
		float x0 = ds.x * 0.5f - total * 0.5f, y0 = ds.y * 0.30f;
		for (int i = 0; i < 6; i++) {
			ImVec2 a(x0 + i * (slotW + gap), y0), b(a.x + slotW, y0 + 84);
			dl->AddRectFilled(a, b, CARD); dl->AddRect(a, b, CARD_EDGE, 0, 0, 2);
			if (i < (int)g_code.size()) { char s[2] = { g_code[i], 0 }; TextCentered(dl, g_fontTitle, 64, (a.x + b.x) * 0.5f, y0 + 6, s, GOLD); }
		}

		// The grid: characters, then a row of DEL, JOIN (play) and WATCH.
		int total_items = CODE_CHAR_COUNT + JOIN_EXTRA;
		float cell = 62, cg = 8;
		float gridW = JOIN_COLS * cell + (JOIN_COLS - 1) * cg;
		float gx0 = ds.x * 0.5f - gridW * 0.5f, gy0 = ds.y * 0.46f;
		int charRows = (CODE_CHAR_COUNT + JOIN_COLS - 1) / JOIN_COLS;
		const char* extras[JOIN_EXTRA] = { "DEL", "JOIN", "WATCH" };
		for (int i = 0; i < total_items; i++) {
			bool sel = i == g_joinCursor;
			ImVec2 a, b;
			const char* label;
			char s[2] = { 0, 0 };
			if (i < CODE_CHAR_COUNT) {
				int r = i / JOIN_COLS, c = i % JOIN_COLS;
				a = ImVec2(gx0 + c * (cell + cg), gy0 + r * (cell + cg)); b = ImVec2(a.x + cell, a.y + cell);
				s[0] = CODE_CHARS[i]; label = s;
			}
			else {
				int k = i - CODE_CHAR_COUNT;
				float w = (gridW - (JOIN_EXTRA - 1) * cg) / JOIN_EXTRA;
				a = ImVec2(gx0 + k * (w + cg), gy0 + charRows * (cell + cg)); b = ImVec2(a.x + w, a.y + cell);
				label = extras[k];
			}
			dl->AddRectFilled(a, b, sel ? RED : CARD); dl->AddRect(a, b, sel ? PAPER : CARD_EDGE, 0, 0, 2);
			TextCentered(dl, g_fontHead, 34, (a.x + b.x) * 0.5f, a.y + 10, label, PAPER, false);
		}
		DrawHint(dl, ds, "Move: pick a letter     A: add     B / Backspace: delete     Y / Ctrl+V: paste     Start: join     WATCH: spectate the match");

		if (in.left) g_joinCursor = (g_joinCursor + total_items - 1) % total_items;
		if (in.right) g_joinCursor = (g_joinCursor + 1) % total_items;
		if (in.up) {
			if (g_joinCursor >= CODE_CHAR_COUNT) {
				int k = g_joinCursor - CODE_CHAR_COUNT;
				g_joinCursor = (std::min)(CODE_CHAR_COUNT - 1, (charRows - 1) * JOIN_COLS + k * 3);
			}
			else if (g_joinCursor >= JOIN_COLS) g_joinCursor -= JOIN_COLS;
		}
		if (in.down && g_joinCursor < CODE_CHAR_COUNT) {
			if (g_joinCursor + JOIN_COLS < CODE_CHAR_COUNT) g_joinCursor += JOIN_COLS;
			else g_joinCursor = CODE_CHAR_COUNT + (std::min)(JOIN_EXTRA - 1, (g_joinCursor % JOIN_COLS) / 3);
		}
		bool submit = in.start, watch = false;
		if (in.confirm) {
			if (g_joinCursor < CODE_CHAR_COUNT) { if (g_code.size() < 6) g_code += CODE_CHARS[g_joinCursor]; }
			else if (g_joinCursor == CODE_CHAR_COUNT) { if (!g_code.empty()) g_code.pop_back(); }
			else if (g_joinCursor == CODE_CHAR_COUNT + 1) submit = true;
			else watch = true;
		}
		if (in.back) { if (!g_code.empty()) g_code.pop_back(); else g_screen = SC_HOME; }
		if (in.alt || in.paste) PasteCode();
		if (submit || watch) {
			if (g_code.size() < 6) { Flash("The code has six characters"); }
			else { g_spectate = watch; g_mm.Join(g_code, sf4e::sidecarHash, g_name, watch); g_screen = SC_CONNECTING; }
		}
	}

	void DrawConnecting(ImDrawList* dl, ImVec2 ds, const Input& in) {
		DrawHeader(dl, ds, g_isCreator ? "CREATING LOBBY" : (g_spectate ? "JOINING AS SPECTATOR" : "JOINING"), nullptr);
		g_mm.Poll();
		if (g_mm.state == sf4e::Matchmaker::State::Waiting) {
			int dots = (int)(GetTickCount64() / 400 % 4);
			std::string t = "Contacting the server" + std::string(dots, '.');
			TextCentered(dl, g_fontHead, 36, ds.x * 0.5f, ds.y * 0.48f, t.c_str(), PAPER);
			if (in.back) { g_mm.Cancel(); g_screen = SC_HOME; }
		}
		else if (g_mm.state == sf4e::Matchmaker::State::Failed) {
			TextCentered(dl, g_fontHead, 36, ds.x * 0.5f, ds.y * 0.44f, "COULD NOT CONNECT", RED);
			TextCentered(dl, g_fontBody, 24, ds.x * 0.5f, ds.y * 0.44f + 54, g_mm.error.c_str(), PAPER_DIM, false);
			DrawHint(dl, ds, "B: back");
			if (in.back || in.confirm) g_screen = g_isCreator ? SC_HOME : SC_JOIN;
		}
		else if (g_mm.state == sf4e::Matchmaker::State::Done) {
			ConnectFromMatchmaker();
			g_screen = SC_LOBBY;
		}
	}

	void DrawResult(ImDrawList* dl, ImVec2 ds, const Input& in) {
		bool draw = g_resultWinner < 0;
		bool won = !draw && g_resultWinner == g_mySideAtResult;
		DrawHeader(dl, ds, "MATCH OVER", nullptr);

		char who[64];
		const char* verdict;
		if (g_spectate) {
			snprintf(who, sizeof(who), "%s WINS", g_resultWinner == 0 ? "P1" : "P2");
			verdict = draw ? "DRAW" : who;
		}
		else {
			verdict = draw ? "DRAW" : (won ? "YOU WIN" : "YOU LOSE");
		}
		TextCentered(dl, g_fontTitle, 120, ds.x * 0.5f, ds.y * 0.24f, verdict, draw ? PAPER : ((won || g_spectate) ? GOLD : RED));

		const char* c1 = (g_resultChara[0] >= 0 && g_resultChara[0] < CHARA_COUNT) ? Dimps::characterNames[g_resultChara[0]] : "?";
		const char* c2 = (g_resultChara[1] >= 0 && g_resultChara[1] < CHARA_COUNT) ? Dimps::characterNames[g_resultChara[1]] : "?";
		char line[96];
		snprintf(line, sizeof(line), "%s   vs   %s", c1, c2);
		TextCentered(dl, g_fontHead, 34, ds.x * 0.5f, ds.y * 0.24f + 130, line, PAPER_DIM, false);
		if (!g_spectate) {
			char tally[64];
			snprintf(tally, sizeof(tally), "this session   %d - %d", g_wins, g_losses);
			TextCentered(dl, g_fontBody, 22, ds.x * 0.5f, ds.y * 0.24f + 176, tally, PAPER_DIM, false);
		}

		const char* playerItems[3] = { "REMATCH", "CHANGE CHARACTER", "LEAVE LOBBY" };
		const char* watcherItems[2] = { "KEEP WATCHING", "LEAVE LOBBY" };
		int n = g_spectate ? 2 : 3;
		const char** items = g_spectate ? watcherItems : playerItems;
		float y = ds.y * 0.56f;
		for (int i = 0; i < n; i++) {
			bool sel = i == g_resultCursor;
			ImVec2 sz = TextSize(g_fontHead, 40, items[i]);
			float x = ds.x * 0.5f - sz.x * 0.5f;
			if (sel) Slant(dl, ImVec2(x - 40, y - 6), ImVec2(x + sz.x + 40, y + 52), i == 0 ? GREEN : RED);
			TextOutlined(dl, g_fontHead, 40, ImVec2(x, y), items[i], sel ? PAPER : PAPER_DIM);
			y += 72;
		}
		DrawHint(dl, ds, g_spectate ? "Up/Down: choose     A: confirm" : "Up/Down: choose     A: confirm     Start: rematch with the same character");

		if (!fUserApp::netplay) {
			TextCentered(dl, g_fontBody, 22, ds.x * 0.5f, ds.y * 0.50f, g_spectate ? "The lobby closed" : "Your opponent left", RED, false);
		}
		if (in.up) g_resultCursor = (g_resultCursor + n - 1) % n;
		if (in.down) g_resultCursor = (g_resultCursor + 1) % n;
		if (g_spectate) {
			if (in.start || (in.confirm && g_resultCursor == 0)) { g_screen = fUserApp::netplay ? SC_LOBBY : SC_HOME; }
			else if ((in.confirm && g_resultCursor == 1) || in.back) LeaveLobby();
			return;
		}
		bool rematch = in.start || (in.confirm && g_resultCursor == 0);
		if (rematch) {
			if (fUserApp::netplay) { SendReady(); g_screen = SC_LOBBY; g_lobbyRow = 2; g_actionCursor = 0; }
			else g_screen = SC_HOME;
		}
		else if (in.confirm && g_resultCursor == 1) {
			g_screen = fUserApp::netplay ? SC_LOBBY : SC_HOME;
			g_lobbyRow = 0;
		}
		else if ((in.confirm && g_resultCursor == 2) || in.back) {
			LeaveLobby();
		}
	}

	void DrawPlayerCard(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* label, const char* name, int charaId, bool ready, bool me) {
		dl->AddRectFilled(a, b, CARD); dl->AddRect(a, b, me ? GOLD : CARD_EDGE, 0, 0, me ? 3 : 2);
		Slant(dl, ImVec2(a.x, a.y), ImVec2(a.x + 70, a.y + 34), me ? GOLD : RED, 8);
		dl->AddText(g_fontHead, 26, ImVec2(a.x + 14, a.y + 3), INK, label);
		dl->AddText(g_fontBody, 24, ImVec2(a.x + 90, a.y + 6), PAPER, name);
		const char* cname = (charaId >= 0 && charaId < CHARA_COUNT && Dimps::characterNames[charaId]) ? Dimps::characterNames[charaId] : "-";
		TextOutlined(dl, g_fontHead, 40, ImVec2(a.x + 14, a.y + 48), cname, PAPER);
		if (ready) { Slant(dl, ImVec2(b.x - 130, b.y - 40), ImVec2(b.x - 8, b.y - 8), GREEN); dl->AddText(g_fontHead, 24, ImVec2(b.x - 112, b.y - 36), INK, "READY"); }
	}

	// The spectator's room: the two players, who is watching, and when the
	// next match starts. Nothing to pick; the players run the show.
	void DrawSpectatorLobby(ImDrawList* dl, ImVec2 ds, const Input& in) {
		sf4e::SessionClient& c = fUserApp::netplay->client;
		bool connected = !c._lobbyData.members.empty();
		std::string code = g_mm.code;

		DrawHeader(dl, ds, "SPECTATING", nullptr);
		TextOutlined(dl, g_fontTitle, 96, ImVec2(ds.x - 60 - TextSize(g_fontTitle, 96, code.c_str()).x, 30), code.c_str(), GOLD, 3.0f);
		dl->AddText(g_fontSmall, 20, ImVec2(ds.x - 60 - 300, 130), PAPER_DIM, "you are watching this lobby   (Y / Ctrl+C: copy)");
		if ((in.alt || in.copy) && !code.empty()) {
			ImGui::SetClipboardText(code.c_str());
			Flash("Code copied to the clipboard", true);
		}

		float cardW = (ds.x - 120 - 40) * 0.5f, cardH = 110, cy = 170;
		for (int i = 0; i < 2; i++) {
			const sf4e::SessionProtocol::MemberData* pm = PlayerAt(i);
			bool ready = pm && c._matchData.readyMessageNum[i] > -1;
			int charaId = ready ? c._matchData.chara[i].charaID : -1;
			ImVec2 a(60 + i * (cardW + 40), cy);
			DrawPlayerCard(dl, a, ImVec2(a.x + cardW, cy + cardH), i == 0 ? "P1" : "P2", pm ? pm->name.c_str() : "waiting for a player...", charaId, ready, false);
		}
		TextCentered(dl, g_fontTitle, 48, ds.x * 0.5f, cy + 24, "VS", RED);

		const sf4e::SessionProtocol::MemberData* me = Me();
		bool bothReady = c._matchData.readyMessageNum[0] > -1 && c._matchData.readyMessageNum[1] > -1;
		const char* status;
		if (!connected) status = "Connecting to the lobby...";
		else if (!PlayerAt(0) || !PlayerAt(1)) status = "Waiting for two players";
		else if (bothReady && me && me->watching) status = "The match is starting";
		else if (bothReady) status = "A match is in progress. You will watch the next one.";
		else status = "Waiting for the players to ready up";
		TextCentered(dl, g_fontHead, 36, ds.x * 0.5f, cy + cardH + 70, status, PAPER);
		std::string specs = SpectatorNames();
		TextCentered(dl, g_fontBody, 22, ds.x * 0.5f, cy + cardH + 130, ("Watching: " + specs).c_str(), PAPER_DIM, false);
		TextCentered(dl, g_fontSmall, 20, ds.x * 0.5f, cy + cardH + 170, "You see the match from P1's side, a moment behind the players.", PAPER_DIM, false);

		float ay = ds.y * 0.72f;
		const char* act = "LEAVE LOBBY";
		ImVec2 sz = TextSize(g_fontHead, 30, act);
		ImVec2 a(60, ay), b(60 + sz.x + 60, ay + 50);
		Slant(dl, a, b, RED, 10);
		TextOutlined(dl, g_fontHead, 30, ImVec2(a.x + 30, a.y + 8), act, PAPER);
		DrawHint(dl, ds, "A / B: leave the lobby     Y / Ctrl+C: copy the code");
		if (in.back || in.confirm) LeaveLobby();
	}

	void DrawLobby(ImDrawList* dl, ImVec2 ds, const Input& in) {
		if (!fUserApp::netplay) {
			DrawHeader(dl, ds, "LOBBY", nullptr);
			TextCentered(dl, g_fontHead, 34, ds.x * 0.5f, ds.y * 0.48f, "Connection lost", RED);
			DrawHint(dl, ds, "B: back");
			if (in.back || in.confirm) g_screen = SC_HOME;
			return;
		}
		if (g_spectate) { DrawSpectatorLobby(dl, ds, in); return; }
		sf4e::SessionClient& c = fUserApp::netplay->client;
		int side = MySide();
		bool connected = !c._lobbyData.members.empty();
		std::string code = g_mm.code;

		DrawHeader(dl, ds, "LOBBY", nullptr);
		// The code, large and gold, where the creator's eye lands first.
		TextOutlined(dl, g_fontTitle, 96, ImVec2(ds.x - 60 - TextSize(g_fontTitle, 96, code.c_str()).x, 30), code.c_str(), GOLD, 3.0f);
		dl->AddText(g_fontSmall, 20, ImVec2(ds.x - 60 - 300, 130), PAPER_DIM, g_isCreator ? "give this code to your opponent   (Y / Ctrl+C: copy)" : "you joined this lobby");
		if ((in.alt || in.copy) && !code.empty()) {
			ImGui::SetClipboardText(code.c_str());
			Flash("Code copied to the clipboard", true);
		}

		// Player cards.
		float cardW = (ds.x - 120 - 40) * 0.5f, cardH = 110, cy = 170;
		for (int i = 0; i < 2; i++) {
			const sf4e::SessionProtocol::MemberData* pm = PlayerAt(i);
			bool has = pm != nullptr;
			const char* name = has ? pm->name.c_str() : "waiting for opponent...";
			bool ready = has && c._matchData.readyMessageNum[i] > -1;
			int charaId = -1;
			if (has && i == side) charaId = g_cond.charaID;
			else if (has && ready) charaId = c._matchData.chara[i].charaID;
			ImVec2 a(60 + i * (cardW + 40), cy);
			DrawPlayerCard(dl, a, ImVec2(a.x + cardW, cy + cardH), i == 0 ? "P1" : "P2", name, charaId, ready, has && i == side);
		}
		TextCentered(dl, g_fontTitle, 48, ds.x * 0.5f, cy + 24, "VS", RED);
		std::string specs = SpectatorNames();
		if (!specs.empty()) dl->AddText(g_fontSmall, 20, ImVec2(60, cy + cardH + 4), PAPER_DIM, ("Watching: " + specs).c_str());

		// Character grid.
		float gy = cy + cardH + 26;
		float cellW = (ds.x - 120 - (CHARA_COLS - 1) * 6) / CHARA_COLS, cellH = 40;
		for (int i = 0; i < CHARA_COUNT; i++) {
			int r = i / CHARA_COLS, col = i % CHARA_COLS;
			ImVec2 a(60 + col * (cellW + 6), gy + r * (cellH + 6)), b(a.x + cellW, a.y + cellH);
			bool cursor = g_lobbyRow == 0 && i == g_charaCursor;
			bool chosen = i == g_cond.charaID;
			dl->AddRectFilled(a, b, cursor ? RED : (chosen ? IM_COL32(242, 193, 78, 60) : CARD));
			dl->AddRect(a, b, cursor ? PAPER : (chosen ? GOLD : CARD_EDGE), 0, 0, chosen || cursor ? 2 : 1);
			const char* nm = Dimps::characterNames[i] ? Dimps::characterNames[i] : "?";
			dl->AddText(g_fontBody, 20, ImVec2(a.x + 8, a.y + 8), cursor ? PAPER : (chosen ? GOLD : PAPER_DIM), nm);
		}
		int rows = (CHARA_COUNT + CHARA_COLS - 1) / CHARA_COLS;
		float oy = gy + rows * (cellH + 6) + 18;

		// Options row.
		char costume[24], color[24], ultra[24], edition[24], stage[40];
		snprintf(costume, sizeof(costume), "COSTUME %d", g_cond.costume + 1);
		snprintf(color, sizeof(color), "COLOR %d", g_cond.color + 1);
		snprintf(ultra, sizeof(ultra), "ULTRA %s", g_cond.ultraCombo == 0 ? "I" : g_cond.ultraCombo == 1 ? "II" : "W");
		snprintf(edition, sizeof(edition), "%s", EditionLabel(g_cond.unc_edition));
		snprintf(stage, sizeof(stage), "STAGE: %s", (side == 0 && Dimps::stageNames[g_stage]) ? Dimps::stageNames[g_stage] : "opponent picks");
		const char* opts[5] = { costume, color, ultra, edition, stage };
		int nOpts = 5;
		float ox = 60;
		for (int i = 0; i < nOpts; i++) {
			bool cursor = g_lobbyRow == 1 && i == g_optionCursor;
			// The focused option wears chevrons so it reads as "press to change".
			char shown[64];
			snprintf(shown, sizeof(shown), cursor ? "<  %s  >" : "%s", opts[i]);
			ImVec2 sz = TextSize(g_fontBody, 22, shown);
			ImVec2 a(ox, oy), b(ox + sz.x + 36, oy + 40);
			if (cursor) Slant(dl, a, b, RED, 8); else dl->AddRect(a, b, CARD_EDGE, 0, 0, 1);
			dl->AddText(g_fontBody, 22, ImVec2(a.x + 18, a.y + 8), cursor ? PAPER : PAPER_DIM, shown);
			ox = b.x + 14;
		}

		// Actions.
		float ay = oy + 60;
		const char* acts[2] = { g_sentReady ? "READY  -  waiting" : "READY", "LEAVE LOBBY" };
		float ax = 60;
		for (int i = 0; i < 2; i++) {
			bool cursor = g_lobbyRow == 2 && i == g_actionCursor;
			ImVec2 sz = TextSize(g_fontHead, 30, acts[i]);
			ImVec2 a(ax, ay), b(ax + sz.x + 60, ay + 50);
			Slant(dl, a, b, cursor ? (i == 0 ? GREEN : RED) : CARD, 10);
			if (!cursor) dl->AddRect(a, b, CARD_EDGE, 0, 0, 1);
			if (cursor) TextOutlined(dl, g_fontHead, 30, ImVec2(a.x + 30, a.y + 8), acts[i], PAPER);
			else dl->AddText(g_fontHead, 30, ImVec2(a.x + 30, a.y + 8), PAPER_DIM, acts[i]);
			ax = b.x + 20;
		}
		const char* hint = g_lobbyRow == 1
			? "Left/Right: next option     A / RB: change     LB: change back     Start: READY     B: leave"
			: "Move: choose     A: select     Start: READY     B: leave";
		DrawHint(dl, ds, connected ? hint : "Connecting to the lobby...");

		// Input.
		if (g_sentReady) {
			if (in.back) { fUserApp::netplay.reset(); g_mm.Cancel(); g_screen = SC_HOME; }
			return;
		}
		if (g_lobbyRow == 0) {
			if (in.left) g_charaCursor = (g_charaCursor + CHARA_COUNT - 1) % CHARA_COUNT;
			if (in.right) g_charaCursor = (g_charaCursor + 1) % CHARA_COUNT;
			if (in.up) { if (g_charaCursor >= CHARA_COLS) g_charaCursor -= CHARA_COLS; }
			if (in.down) { if (g_charaCursor + CHARA_COLS < CHARA_COUNT) g_charaCursor += CHARA_COLS; else g_lobbyRow = 1; }
			if (in.confirm) { g_cond.charaID = (BYTE)g_charaCursor; g_cond.costume = 0; g_cond.color = 0; CycleEdition(0); }
		}
		else if (g_lobbyRow == 1) {
			if (in.up) g_lobbyRow = 0;
			if (in.down) g_lobbyRow = 2;
			// Left/right walk along the row; A and the bumpers change the value.
			if (in.left) g_optionCursor = (g_optionCursor + nOpts - 1) % nOpts;
			if (in.right) g_optionCursor = (g_optionCursor + 1) % nOpts;
			int d = in.confirm ? 1 : 0;
			if (in.rb) d = 1; if (in.lb) d = -1;
			if (d != 0) {
				switch (g_optionCursor) {
				case 0: g_cond.costume = (BYTE)((g_cond.costume + 8 + d) % 8); break;
				case 1: g_cond.color = (BYTE)((g_cond.color + 10 + d) % 10); break;
				case 2: g_cond.ultraCombo = (BYTE)((g_cond.ultraCombo + 3 + d) % 3); break;
				case 3: CycleEdition(d); break;
				case 4: if (side == 0) g_stage = (g_stage + 30 + d) % 30; break;
				}
			}
		}
		else {
			if (in.up) g_lobbyRow = 1;
			if (in.left || in.right) g_actionCursor = 1 - g_actionCursor;
			if (in.confirm) {
				if (g_actionCursor == 0) SendReady();
				else { fUserApp::netplay.reset(); g_mm.Cancel(); g_screen = SC_HOME; return; }
			}
		}
		if (in.start && connected) SendReady();
		if (in.back && g_lobbyRow != 2) { g_lobbyRow = 2; g_actionCursor = 1; }
		else if (in.back) { fUserApp::netplay.reset(); g_mm.Cancel(); g_screen = SC_HOME; }
	}
}

// ---------------------------------------------------------------- public
void sf4e::Lobby::LoadFonts(ImGuiIO& io) {
	// ImGui's default font is whichever is added first, and the developer
	// overlay relies on it. Add it first, and pin it, so the display fonts
	// below never leak into the debug windows.
	ImFont* fallback = io.Fonts->AddFontDefault();
	io.FontDefault = fallback;

	char fonts[MAX_PATH];
	GetWindowsDirectoryA(fonts, MAX_PATH);
	std::string dir = std::string(fonts) + "\\Fonts\\";
	g_fontTitle = io.Fonts->AddFontFromFileTTF((dir + "impact.ttf").c_str(), 96.0f);
	g_fontHead = io.Fonts->AddFontFromFileTTF((dir + "impact.ttf").c_str(), 44.0f);
	g_fontBody = io.Fonts->AddFontFromFileTTF((dir + "segoeuib.ttf").c_str(), 26.0f);
	g_fontSmall = io.Fonts->AddFontFromFileTTF((dir + "segoeui.ttf").c_str(), 20.0f);
	if (!g_fontTitle) g_fontTitle = fallback;
	if (!g_fontHead) g_fontHead = fallback;
	if (!g_fontBody) g_fontBody = fallback;
	if (!g_fontSmall) g_fontSmall = fallback;
	if (g_fontTitle == fallback) spdlog::warn("Lobby: Impact/Segoe not found, using the default font");
}

void sf4e::Lobby::Open() {
	if (g_open) return;
	g_open = true;
	g_hiddenForBattle = false;
	g_screen = (g_deviceIdx == 0xff) ? SC_CAPTURE : SC_HOME;
	g_serverStatus = -1;
	g_pingInFlight = false;
	g_homeCursor = 0;
	g_error.clear();
	GenerateStrokes(ImGui::GetIO().DisplaySize);
	SetSuppress(true);
	g_prevPad = 0xffff;
	spdlog::info("Lobby: opened");
}

// After closing, keep the game's pad muted until every button is released.
// Otherwise the press that closed the lobby is still held when the game gets
// the pad back, its menu treats it as a fresh press on the still-highlighted
// "Multiplayer Battle", and the lobby reopens before the player sees it go.
static bool g_releaseLatch = false;

static bool AnyButtonHeld() {
	XINPUT_STATE xs;
	for (DWORD i = 0; i < 4; i++) {
		if (XInputGetState(i, &xs) == ERROR_SUCCESS && xs.Gamepad.wButtons != 0) return true;
	}
	const int keys[] = { VK_RETURN, VK_SPACE, VK_ESCAPE, VK_BACK, VK_F1, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (GetAsyncKeyState(keys[i]) & 0x8000) return true;
	}
	return false;
}

void sf4e::Lobby::Close() {
	if (!g_open) return;
	g_open = false;
	g_releaseLatch = true;
	spdlog::info("Lobby: closed");
}

bool sf4e::Lobby::IsOpen() { return g_open; }

void sf4e::Lobby::OnMatchResult(int winnerSide, int charaP1, int charaP2) {
	if (!g_open || !fUserApp::netplay) return;
	g_hasResult = true;
	g_resultWinner = winnerSide;
	g_resultChara[0] = charaP1;
	g_resultChara[1] = charaP2;
	// The match data knows what each side actually picked.
	sf4e::SessionClient& c = fUserApp::netplay->client;
	if (c._matchData.readyMessageNum[0] > -1) g_resultChara[0] = c._matchData.chara[0].charaID;
	if (c._matchData.readyMessageNum[1] > -1) g_resultChara[1] = c._matchData.chara[1].charaID;
	g_mySideAtResult = MySide();
	if (winnerSide >= 0 && !g_spectate) {
		if (winnerSide == g_mySideAtResult) g_wins++; else g_losses++;
	}
	spdlog::info("Lobby: match over, winner side {} (me: side {})", winnerSide, g_mySideAtResult);
}

void sf4e::Lobby::Draw() {
	if (!g_open) {
		if (g_releaseLatch) {
			if (AnyButtonHeld()) return;
			g_releaseLatch = false;
			SetSuppress(false);
		}
		return;
	}

	// Hide during a match, and come back for the rematch.
	bool menu = OnMainMenu();
	if (!menu) {
		if (!g_hiddenForBattle) { g_hiddenForBattle = true; SetSuppress(g_spectate); }
		return;
	}
	if (g_hiddenForBattle) {
		g_hiddenForBattle = false;
		SetSuppress(true);
		g_sentReady = false;
		g_lobbyRow = 0;
		g_prevPad = 0xffff;

		// Ask the server to reset readiness so both players can go again.
		// Only one side may report, or the winner-stays rotation runs twice
		// and undoes itself; P1 does it. A draw is reported as P2 losing,
		// which leaves the sides where they were.
		if (fUserApp::netplay && MySide() == 0) {
			int loser = (g_hasResult && g_resultWinner >= 0) ? (1 - g_resultWinner) : 1;
			fUserApp::netplay->client.Lobby_ReportResults(loser);
		}
		if (g_hasResult) {
			g_hasResult = false;
			g_resultCursor = 0;
			g_screen = SC_RESULT;
		}
	}

	ImGuiIO& io = ImGui::GetIO();
	ImVec2 ds = io.DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ds);
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::Begin("##sf4e-lobby", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	DrawBackdrop(dl, ds);

	Input in = ReadInput();
	switch (g_screen) {
	case SC_CAPTURE: DrawCapture(dl, ds); break;
	case SC_HOME: DrawHome(dl, ds, in); break;
	case SC_JOIN: DrawJoin(dl, ds, in); break;
	case SC_CONNECTING: DrawConnecting(dl, ds, in); break;
	case SC_LOBBY: DrawLobby(dl, ds, in); break;
	case SC_RESULT: DrawResult(dl, ds, in); break;
	}
	DrawError(dl, ds);
	dl->AddText(g_fontSmall, 16, ImVec2(ds.x - 200, 8), IM_COL32(255, 255, 255, 90), "sf4e rollback  test");
	ImGui::End();
}
