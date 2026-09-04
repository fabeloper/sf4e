#pragma once

namespace sf4e {
	// Records how the process died. Three things are caught:
	//
	// * hardware faults (access violation, illegal instruction, stack
	//   overflow, divide by zero), through a vectored handler that fires
	//   before anything else can swallow them;
	// * anything else unhandled, through the unhandled-exception filter;
	// * a silent ExitProcess, which produces no crash record at all and is
	//   otherwise indistinguishable from the game just closing.
	//
	// Each one logs the exception, the faulting module and offset, and a
	// stack of module+offset frames to sf4e.log, flushes it, and writes a
	// minidump next to the logs. Call from inside the Detours transaction.
	namespace Crash {
		void Install();
	}
}
