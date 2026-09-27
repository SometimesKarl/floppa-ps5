#ifndef EMULATOR_SRC_COMMON_TICKHISTORY_H_
#define EMULATOR_SRC_COMMON_TICKHISTORY_H_

#include <chrono>
#include <cstdint>
#include <deque>

namespace Common {

// Maps a monotonically increasing work counter (a cache's collection tick) to wall time, so
// "unused for 5 seconds" can be expressed as a tick bound whatever the tick rate is.
class TickHistory {
public:
	void Record(uint64_t tick) {
		const auto now = std::chrono::steady_clock::now();
		if (!m_samples.empty() && now - m_samples.back().time < std::chrono::milliseconds(250)) {
			return;
		}
		m_samples.push_back({tick, now});
		if (m_samples.size() > 512) {
			m_samples.pop_front();
		}
	}

	// The newest recorded tick that is at least `seconds` old; 0 while the history is shorter.
	[[nodiscard]] uint64_t TickSecondsAgo(double seconds) const {
		const auto limit = std::chrono::steady_clock::now() -
		                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
		                       std::chrono::duration<double>(seconds));
		for (auto it = m_samples.rbegin(); it != m_samples.rend(); ++it) {
			if (it->time <= limit) {
				return it->tick;
			}
		}
		return 0;
	}

private:
	struct Sample {
		uint64_t                              tick;
		std::chrono::steady_clock::time_point time;
	};
	std::deque<Sample> m_samples;
};

} // namespace Common

#endif // EMULATOR_SRC_COMMON_TICKHISTORY_H_
