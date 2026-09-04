#include <string>

#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <detours/detours.h>
#include <spdlog/spdlog.h>

#include "sf4e__Crash.hxx"

namespace {
	// Only one report per process. A crash handler that crashes, or a fault
	// on every thread at once, must not recurse into itself.
	volatile LONG g_reporting = 0;

	std::string ModuleAndOffset(const void* address) {
		HMODULE module = nullptr;
		if (GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)address,
			&module
		) && module != nullptr) {
			char path[MAX_PATH] = { 0 };
			GetModuleFileNameA(module, path, MAX_PATH);
			const char* base = strrchr(path, '\\');
			base = base ? base + 1 : path;
			char out[MAX_PATH + 32];
			sprintf_s(out, "%s+0x%x", base, (unsigned)((const char*)address - (const char*)module));
			return out;
		}
		char out[32];
		sprintf_s(out, "0x%p", address);
		return out;
	}

	const char* ExceptionName(DWORD code) {
		switch (code) {
		case EXCEPTION_ACCESS_VIOLATION: return "access violation";
		case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
		case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
		case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
		case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
		case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
		case 0xE06D7363: return "unhandled C++ exception";
		default: return "exception";
		}
	}

	bool IsFatal(DWORD code) {
		switch (code) {
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_STACK_OVERFLOW:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_PRIV_INSTRUCTION:
		case EXCEPTION_IN_PAGE_ERROR:
			return true;
		default:
			return false;
		}
	}

	void LogStack(const CONTEXT* context) {
		void* frames[40];
		USHORT count = CaptureStackBackTrace(0, 40, frames, nullptr);
		// When we have the faulting context, the top frame that matters is
		// the faulting instruction itself, not our handler.
		if (context) {
			spdlog::critical("  at   {}", ModuleAndOffset((const void*)context->Eip));
		}
		for (USHORT i = 0; i < count; i++) {
			spdlog::critical("  [{:>2}] {}", i, ModuleAndOffset(frames[i]));
		}
	}

	void WriteMinidump(EXCEPTION_POINTERS* pointers) {
		PWSTR appdata = nullptr;
		if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &appdata) != S_OK) {
			return;
		}
		std::wstring dir(appdata);
		CoTaskMemFree(appdata);
		dir += L"\\sf4e";
		CreateDirectoryW(dir.c_str(), NULL);
		dir += L"\\crash";
		CreateDirectoryW(dir.c_str(), NULL);

		SYSTEMTIME t;
		GetLocalTime(&t);
		wchar_t path[MAX_PATH];
		swprintf_s(path, L"%s\\sf4e-%04d%02d%02d-%02d%02d%02d.dmp", dir.c_str(),
			t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

		HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE) {
			return;
		}
		MINIDUMP_EXCEPTION_INFORMATION info;
		info.ThreadId = GetCurrentThreadId();
		info.ExceptionPointers = pointers;
		info.ClientPointers = FALSE;
		BOOL ok = MiniDumpWriteDump(
			GetCurrentProcess(),
			GetCurrentProcessId(),
			file,
			(MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs),
			pointers ? &info : nullptr,
			nullptr,
			nullptr
		);
		CloseHandle(file);
		char narrow[MAX_PATH];
		WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, MAX_PATH, NULL, NULL);
		spdlog::critical("  minidump {}: {}", ok ? "written to" : "FAILED at", narrow);
	}

	void Report(const char* how, EXCEPTION_POINTERS* pointers) {
		if (InterlockedCompareExchange(&g_reporting, 1, 0) != 0) {
			return;
		}
		spdlog::critical("==== sf4e crash report ({}) ====", how);
		if (pointers && pointers->ExceptionRecord) {
			const EXCEPTION_RECORD* r = pointers->ExceptionRecord;
			spdlog::critical("  {} (0x{:08x}) at {}", ExceptionName(r->ExceptionCode), r->ExceptionCode, ModuleAndOffset(r->ExceptionAddress));
			if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
				spdlog::critical("  {} address 0x{:08x}", r->ExceptionInformation[0] == 0 ? "reading" : (r->ExceptionInformation[0] == 1 ? "writing" : "executing"), (unsigned)r->ExceptionInformation[1]);
			}
			LogStack(pointers->ContextRecord);
		}
		else {
			LogStack(nullptr);
		}
		WriteMinidump(pointers);
		spdlog::default_logger()->flush();
	}

	LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* pointers) {
		if (pointers && pointers->ExceptionRecord && IsFatal(pointers->ExceptionRecord->ExceptionCode)) {
			Report("hardware fault", pointers);
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* pointers) {
		Report("unhandled exception", pointers);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// ExitProcess leaves no trace of who called it. Log the caller so a
	// "the game just closed" report can be traced.
	void (WINAPI* RealExitProcess)(UINT) = ExitProcess;

	void WINAPI HookedExitProcess(UINT code) {
		if (InterlockedCompareExchange(&g_reporting, 1, 0) == 0) {
			spdlog::critical("==== ExitProcess({}) called ====", code);
			LogStack(nullptr);
			spdlog::default_logger()->flush();
		}
		RealExitProcess(code);
	}
}

void sf4e::Crash::Install() {
	AddVectoredExceptionHandler(1, VectoredHandler);
	SetUnhandledExceptionFilter(UnhandledFilter);
	DetourAttach((PVOID*)&RealExitProcess, HookedExitProcess);
}
