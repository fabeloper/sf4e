#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "sf4e__Rtti.hxx"

namespace {
	const std::string EMPTY;

	// Cache keyed by vtable pointer: every instance of a class shares one.
	std::unordered_map<uint32_t, std::string> g_namesByVtable;

	bool IsReadable(uint32_t address, size_t size) {
		if (address < 0x00010000 || address > 0x7ffeffff) {
			return false;
		}
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery((LPCVOID)address, &mbi, sizeof(mbi)) != sizeof(mbi)) {
			return false;
		}
		if (mbi.State != MEM_COMMIT) {
			return false;
		}
		if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
			return false;
		}
		// Reject reads that would run off the end of the region.
		uint32_t regionEnd = (uint32_t)mbi.BaseAddress + (uint32_t)mbi.RegionSize;
		return address + size <= regionEnd;
	}

	bool Read32(uint32_t address, uint32_t& out) {
		if (!IsReadable(address, sizeof(uint32_t))) {
			return false;
		}
		out = *(const uint32_t*)address;
		return true;
	}

	// Turns ".?AVActor@Chara@Battle@Game@Dimps@@" into
	// "Game::Battle::Chara::Actor". The mangled form lists the innermost name
	// first and each enclosing scope after it, so the parts are reversed.
	std::string Demangle(const char* mangled) {
		std::string name(mangled);
		if (name.compare(0, 4, ".?AV") != 0 && name.compare(0, 4, ".?AU") != 0) {
			return EMPTY;
		}
		name = name.substr(4);
		size_t tail = name.find("@@");
		if (tail != std::string::npos) {
			name = name.substr(0, tail);
		}

		std::vector<std::string> parts;
		size_t start = 0;
		while (start <= name.size()) {
			size_t at = name.find('@', start);
			if (at == std::string::npos) {
				parts.push_back(name.substr(start));
				break;
			}
			parts.push_back(name.substr(start, at - start));
			start = at + 1;
		}

		// Every game class sits under Dimps; repeating it adds nothing.
		if (!parts.empty() && parts.back() == "Dimps") {
			parts.pop_back();
		}

		std::string out;
		for (size_t i = parts.size(); i > 0; i--) {
			if (parts[i - 1].empty()) {
				continue;
			}
			if (!out.empty()) {
				out += "::";
			}
			out += parts[i - 1];
		}
		return out;
	}
}

const std::string& sf4e::Rtti::GetClassName(const void* obj) {
	uint32_t objAddress = (uint32_t)obj;
	uint32_t vtable;
	if (!Read32(objAddress, vtable)) {
		return EMPTY;
	}

	auto cached = g_namesByVtable.find(vtable);
	if (cached != g_namesByVtable.end()) {
		return cached->second;
	}

	// MSVC lays out 32-bit RTTI as:
	//   vtable[-1]                  -> RTTICompleteObjectLocator
	//   locator + 0x0c              -> TypeDescriptor
	//   descriptor + 0x08           -> the mangled name
	std::string name;
	uint32_t locator = 0;
	uint32_t descriptor = 0;
	if (Read32(vtable - 4, locator) && Read32(locator + 0x0c, descriptor)) {
		uint32_t nameAddress = descriptor + 0x08;
		if (IsReadable(nameAddress, 8)) {
			const char* mangled = (const char*)nameAddress;
			size_t maxLen = 0;
			while (maxLen < 512 && IsReadable(nameAddress + (uint32_t)maxLen, 1) && mangled[maxLen] != '\0') {
				maxLen++;
			}
			if (maxLen > 4 && maxLen < 512) {
				name = Demangle(mangled);
			}
		}
	}

	auto inserted = g_namesByVtable.emplace(vtable, std::move(name));
	return inserted.first->second;
}
