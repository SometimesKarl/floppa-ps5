#ifndef EMULATOR_SRC_COMMON_AVSYNC_H_
#define EMULATOR_SRC_COMMON_AVSYNC_H_

#include <atomic>
#include <cstdint>

// Presentation latency shared between video out (which measures it) and audio out (which
// delays its host queue to match), so sound does not run ahead of the picture when the
// emulated frame pipeline is slower than the console's.
namespace Common::AvSync {

// Smoothed time from the guest submitting a frame's flip to the host present returning, and
// the smoothed interval between presented frames; 0 until the first GPU flip is presented.
inline std::atomic<uint32_t> g_video_latency_us {0};
inline std::atomic<uint32_t> g_frame_interval_us {0};

inline void ReportPresentedFrame(uint64_t latency_us, uint64_t interval_us) {
	// Per-sample clamps keep a loading stall from inflating the estimate; ~50-frame smoothing.
	constexpr uint64_t MaxSample = 200000;
	const auto         update    = [](std::atomic<uint32_t>& value, uint64_t sample) {
		sample         = sample < MaxSample ? sample : MaxSample;
		const auto old = value.load(std::memory_order_relaxed);
		const auto next =
		    old == 0 ? sample : (static_cast<uint64_t>(old) * 49 + sample) / 50;
		value.store(static_cast<uint32_t>(next), std::memory_order_relaxed);
	};
	update(g_video_latency_us, latency_us);
	if (interval_us != 0) {
		update(g_frame_interval_us, interval_us);
	}
}

[[nodiscard]] inline uint32_t VideoLatencyUs() {
	return g_video_latency_us.load(std::memory_order_relaxed);
}

[[nodiscard]] inline uint32_t FrameIntervalUs() {
	return g_frame_interval_us.load(std::memory_order_relaxed);
}

} // namespace Common::AvSync

#endif // EMULATOR_SRC_COMMON_AVSYNC_H_
