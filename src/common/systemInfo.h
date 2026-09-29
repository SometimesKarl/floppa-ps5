#ifndef KYTY_COMMON_SYSTEM_INFO_H_
#define KYTY_COMMON_SYSTEM_INFO_H_

#include <cstdint>
#include <optional>
#include <string>

namespace Common {

struct SystemInfo {
	std::string ProcessorName;
};

[[nodiscard]] SystemInfo GetSystemInfo();

// Physical memory still available to processes, in MiB (0: unknown on this platform).
[[nodiscard]] uint64_t AvailablePhysicalMemoryMib();
// The same, empty when the platform cannot report it; 0 means less than 1 MiB is available.
[[nodiscard]] std::optional<uint64_t> AvailablePhysicalMemoryMibIfKnown();

} // namespace Common

#endif /* KYTY_COMMON_SYSTEM_INFO_H_ */
