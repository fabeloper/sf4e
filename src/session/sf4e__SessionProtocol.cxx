#include <cstdio>
#include <string>

#include <cstdio>
#include <string>

#include "../Dimps/Dimps__GameEvents.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	namespace SessionProtocol {
		bool ConnectionID::operator==(const ConnectionID& rhs) {
			return this->host == rhs.host && this->user == rhs.user;
		}

		bool LobbyID::operator==(const LobbyID& rhs) {
			if (this->host == "" || this->key == "") {
				return rhs.host == "" || rhs.key == "";
			}

			return this->host == rhs.host && this->key == rhs.key;
		}

		const LobbyID LobbyID::NULL_LOBBY_ID = { "", "" };
		const LobbyData LobbyData::NULL_LOBBY = {
			LobbyID::NULL_LOBBY_ID,
			false,
			0,
			{0, 0},
			{}
		};

		MatchData::MatchData()
		{
			Clear();
		}

		void MatchData::Clear() {
			readyMessageNum[0] = -1;
			readyMessageNum[1] = -1;
			stageID = -1;
			rngSeed = 0xffffffff;
			memset(chara, 0, sizeof(Dimps::GameEvents::VsMode::ConfirmedCharaConditions) * 2);
		}

		bool MatchData::IsAllReady() {
			return (
				readyMessageNum[0] != -1 &&
				readyMessageNum[1] != -1
			);
		}
		static std::string FormatFP(const FixedPoint& v) {
			char b[24];
			snprintf(b, sizeof(b), "%d+%u/65536", (int)v.integral, (unsigned)v.fractional);
			return b;
		}

		std::string DescribeSnapshotDiff(const StateSnapshot& mine, const StateSnapshot& theirs) {
			std::string out;
			for (int i = 0; i < 2; i++) {
				const StateSnapshot::CharaStateSnapshot& m = mine.chara[i];
				const StateSnapshot::CharaStateSnapshot& r = theirs.chara[i];
				auto add = [&](const char* name, const std::string& mv, const std::string& rv) {
					if (mv == rv) return;
					if (!out.empty()) out += ", ";
					out += "P" + std::to_string(i + 1) + "." + name + " a=" + mv + " b=" + rv;
				};
				add("status", std::to_string(m.status), std::to_string(r.status));
				add("side", std::to_string(m.side), std::to_string(r.side));
				for (int k = 0; k < 4; k++) {
					if (m.rootPos[k] != r.rootPos[k]) {
						char nm[16], mv[32], rv[32];
						snprintf(nm, sizeof(nm), "rootPos[%d]", k);
						snprintf(mv, sizeof(mv), "%.6f", m.rootPos[k]);
						snprintf(rv, sizeof(rv), "%.6f", r.rootPos[k]);
						add(nm, mv, rv);
					}
				}
				#define FPF(x) add(#x, FormatFP(m.x), FormatFP(r.x))
				FPF(vit); FPF(vitmax); FPF(revenge); FPF(revengemax);
				FPF(recoverable); FPF(recoverablemax); FPF(super); FPF(supermax);
				FPF(sctimeamt); FPF(sctimemax); FPF(uctime); FPF(uctimemax);
				FPF(damage); FPF(combodamage);
				#undef FPF
			}
			if (out.empty()) {
				out = "(the compared fields all match; the drift is in state the snapshot does not cover)";
			}
			return out;
		}
	}
}
