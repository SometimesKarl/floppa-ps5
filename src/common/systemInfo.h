#ifndef KYTY_COMMON_SYSTEM_INFO_H_
#define KYTY_COMMON_SYSTEM_INFO_H_

#include <cstdint>
#include <string>

namespace Common {

struct SystemInfo {
	std::string ProcessorName;
};

[[nodiscard]] SystemInfo GetSystemInfo();

// Physical memory still available to processes, in MiB (0: unknown on this platform).
[[nodiscard]] uint64_t AvailablePhysicalMemoryMib();

} // namespace Common

#endif /* KYTY_COMMON_SYSTEM_INFO_H_ */
