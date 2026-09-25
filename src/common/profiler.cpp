#include "common/profiler.h"

#include "common/emulatorConfig.h"

#include <algorithm>
#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <tracy/Tracy.hpp>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#endif

namespace {

thread_local std::vector<Profiler::ScopedBlock*> g_block_stack;

void RemoveBlock(Profiler::ScopedBlock* block) {
	auto block_it = std::find(g_block_stack.rbegin(), g_block_stack.rend(), block);
	if (block_it != g_block_stack.rend()) {
		g_block_stack.erase(std::next(block_it).base());
	}
}

} // namespace

namespace Profiler {

ScopedBlock::ScopedBlock(const tracy::SourceLocationData* source_location) {
	if (tracy::ProfilerAvailable()) {
		m_zone.emplace(source_location, TRACY_CALLSTACK, true);
		g_block_stack.push_back(this);
	}
}

ScopedBlock::~ScopedBlock() {
	End();
}

void ScopedBlock::End() {
	if (m_zone.has_value()) {
		m_zone.reset();
		RemoveBlock(this);
	}
}

void EndBlock() {
	if (!g_block_stack.empty()) {
		g_block_stack.back()->End();
	}
}

void SetThreadName(const char* name) {
	if (name == nullptr) {
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// The OS description lets debuggers and per-thread CPU sampling attribute time to
	// emulator and guest threads without a profiler connection.
	wchar_t wide[64] {};
	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, static_cast<int>(std::size(wide))) > 0) {
		SetThreadDescription(GetCurrentThread(), wide);
	}
#endif
	if (tracy::ProfilerAvailable()) {
		tracy::SetThreadName(name);
	}
}

void MarkFrame() {
	// With TRACY_MANUAL_LIFETIME the profiler instance exists only after StartupProfiler.
	if (tracy::ProfilerAvailable()) {
		FrameMark;
	}
}

void Initialize() {
	if (Config::ProfilerEnabled() && !tracy::ProfilerAvailable()) {
		tracy::StartupProfiler();
		TracySetProgramName("KytyPS5");
		::printf("Tracy profiler enabled: client %d.%d.%d, protocol %u, "
		         "broadcast %u, connect to 127.0.0.1:8086\n",
		         tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch,
		         tracy::ProtocolVersion, tracy::BroadcastVersion);
	}
}

void Shutdown() {
	if (tracy::ProfilerAvailable()) {
		tracy::ShutdownProfiler();
	}
}

} // namespace Profiler
