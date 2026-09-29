#include "common/systemInfo.h"

#include "common/assert.h"
#include "cpuinfo.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#include <unistd.h>
#endif

namespace Common {

SystemInfo GetSystemInfo() {
	static const bool initialized = cpuinfo_initialize();
	EXIT_IF(!initialized);

	const auto* package = cpuinfo_get_package(0);
	EXIT_IF(package == nullptr);

	return {package->name};
}

uint64_t AvailablePhysicalMemoryMib() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORYSTATUSEX status {};
	status.dwLength = sizeof(status);
	return GlobalMemoryStatusEx(&status) != 0 ? status.ullAvailPhys / (1024ull * 1024) : 0;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	const long pages = sysconf(_SC_AVPHYS_PAGES);
	const long size  = sysconf(_SC_PAGESIZE);
	return pages > 0 && size > 0
	           ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(size) / (1024ull * 1024)
	           : 0;
#else
	return 0;
#endif
}

} // namespace Common
