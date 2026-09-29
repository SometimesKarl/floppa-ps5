#include "common/profiler.h"

#include "common/emulatorConfig.h"

#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <tracy/Tracy.hpp>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#endif

namespace Profiler {

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
