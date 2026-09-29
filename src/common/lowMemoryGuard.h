#ifndef KYTY_COMMON_LOW_MEMORY_GUARD_H_
#define KYTY_COMMON_LOW_MEMORY_GUARD_H_

#include <algorithm>
#include <cstdint>
#include <optional>

namespace Common {

// Decisions of the low-memory guard (main.cpp StartLowMemoryGuard), one call per 500 ms sample of
// available physical memory. Kept free of platform calls so the rules are unit-tested
// (tests/LowMemoryGuardTests.cpp).
class LowMemoryGuard {
public:
	enum class Action { None, Warn, ClearWarning, Stop };

	struct Settings {
		uint64_t stop_mib = 400;
		// Opt-in (low_memory_fast_stop / KYTY_LOW_MEMORY_FAST_STOP): stop without waiting for
		// four low samples when memory is nearly gone or falling fast. Demon's Souls area loads
		// took free RAM from 1.8 GB to 1 MiB in about 5 s (S11), faster than 2 s below the stop
		// threshold allows for.
		bool fast_stop = false;
	};

	static constexpr uint32_t StopSamples = 4; // 2 s at one sample per 500 ms

	explicit LowMemoryGuard(Settings settings): m_settings(settings) {}

	[[nodiscard]] uint64_t WarnMib() const { return std::max<uint64_t>(1024, m_settings.stop_mib + 512); }

	// `available_mib` is empty when the platform could not report it; such a sample changes
	// nothing. A reported 0 (less than 1 MiB free) is a real reading: the old guard skipped it as
	// "unknown", at exactly the moment it exists for.
	Action Sample(std::optional<uint64_t> available_mib) {
		if (m_settings.stop_mib == 0 || !available_mib.has_value()) {
			return Action::None;
		}
		const uint64_t available = *available_mib;
		const uint64_t previous  = m_previous.value_or(available);
		m_previous               = available;

		m_low_count = available < m_settings.stop_mib ? m_low_count + 1 : 0;
		if (m_low_count >= StopSamples) {
			return Action::Stop;
		}
		if (m_settings.fast_stop && available < m_settings.stop_mib) {
			const uint64_t drop = previous > available ? previous - available : 0;
			// Nearly gone, or at the last sample's rate gone within about one more second.
			if (available < m_settings.stop_mib / 4 || drop * 2 >= available) {
				return Action::Stop;
			}
		}
		if (available < WarnMib()) {
			m_warned = true;
			return Action::Warn;
		}
		if (m_warned && available > WarnMib() + 256) {
			m_warned = false;
			return Action::ClearWarning;
		}
		return Action::None;
	}

private:
	Settings                m_settings;
	std::optional<uint64_t> m_previous;
	uint32_t                m_low_count = 0;
	bool                    m_warned    = false;
};

} // namespace Common

#endif /* KYTY_COMMON_LOW_MEMORY_GUARD_H_ */
