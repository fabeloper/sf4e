#pragma once

#include <random>
#include <string>
#include <windows.h>

#include "../Dimps/Dimps__Eva.hxx"

namespace sf4e {
	typedef struct Args {
		bool bShowConsole = false;
		// Lobby server as "host" or "host:port". Copied by value into the
		// game process with the rest of the payload, hence a fixed buffer.
		char szServer[96] = { 0 };
	} Args;

	typedef struct Payload {
		Args args;
		HANDLE hSyncEvent = NULL;
	} Payload;

	extern std::string sidecarHash;
	extern std::mt19937 localRand;
	extern Args args;
	extern HANDLE hSyncEvent;

	void Install(HINSTANCE hinstDll, const Payload* const payload);

	namespace Eva {
		struct IEmSpriteAction : Dimps::Eva::IEmSpriteAction {
			struct AdditionalMemento {
				Dimps::Eva::IEmSpriteAction action;
			};

			static void RecordToAdditionalMemento(Dimps::Eva::IEmSpriteAction* a, AdditionalMemento& m);
			static void RestoreFromAdditionalMemento(Dimps::Eva::IEmSpriteAction* a, const AdditionalMemento& m);
		};

		struct Task : Dimps::Eva::Task {
			struct TaskFunctorBuf {
				char pad[0x10];
			};

			struct AdditionalMemento {
				Dimps::Eva::Task rawTask;

				TaskFunctorBuf cancelFunctor;
				TaskFunctorBuf workFunctor;
				bool hasCancelFunctor;
				bool hasWorkFunctor;
			};

			static void RecordToAdditionalMemento(Dimps::Eva::Task* t, AdditionalMemento& m);
			static void RestoreFromAdditionalMemento(Dimps::Eva::Task* t, const AdditionalMemento& m);
		};

		struct TaskCore : Dimps::Eva::TaskCore {
			// This is wrong- this is variable length, but we only care about storing
			// the data of the System task core right now. It would make more sense
			// for this to be associated with the Task memento, but the core is the
			// object that knows how large the private per-task data is.
			struct TaskDataBuf {
				// Derived from 0x5da300
				char pad[0x20];
			};

			struct AdditionalMemento {
				int numUsed;
				Task::AdditionalMemento tasks[MAX_TASKS_PER_CORE];
				TaskDataBuf taskdata[MAX_TASKS_PER_CORE];
			};

			static void RecordToAdditionalMemento(Dimps::Eva::TaskCore* c, AdditionalMemento& m);
			static void RestoreFromAdditionalMemento(Dimps::Eva::TaskCore* c, const AdditionalMemento& m);
		};
	}
}