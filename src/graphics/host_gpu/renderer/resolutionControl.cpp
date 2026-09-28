#include "graphics/host_gpu/renderer/resolutionControl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Libs::Graphics::ResolutionControl {

namespace {

constexpr double MinScale = 0.1;
constexpr double MaxScale = 8.0;
// Frames between adjustments: the title's controller needs a few frames to answer.
constexpr uint32_t Period = 8;

std::atomic<uint32_t> g_target_width {0};
std::atomic<double>   g_scale {1.0};

// GPU command thread only.
struct Widths {
	std::array<uint32_t, 8> width {};
	std::array<uint32_t, 8> binds {};
	uint32_t                frames = 0;
	uint32_t                outside = 0; // consecutive periods outside the target band
	uint32_t                last_logged_width = 0;
} g_widths;

std::mutex g_clock_mutex;
uint64_t   g_last_real     = 0;
uint64_t   g_last_reported = 0;

} // namespace

void Configure(uint32_t target_width) {
	g_target_width.store(target_width, std::memory_order_relaxed);
	if (target_width != 0) {
		std::printf("Resolution control: steering the title's dynamic resolution to %u pixels wide\n",
		            target_width);
	}
}

bool Active() {
	return g_target_width.load(std::memory_order_relaxed) != 0;
}

void NoteColorTarget(uint32_t width, uint32_t height) {
	// Screen-sized 16:9 targets only (dynamic-resolution passes); shadow maps, cube faces and
	// small post-process chains do not count.
	if (width < 1280 || height == 0 || width * 9 < height * 16 * 97 / 100 ||
	    width * 9 > height * 16 * 103 / 100) {
		return;
	}
	auto& w = g_widths;
	for (size_t i = 0; i < w.width.size(); i++) {
		if (w.width[i] == width || w.width[i] == 0) {
			w.width[i] = width;
			w.binds[i]++;
			return;
		}
	}
}

void EndGuestFrame() {
	const auto target = g_target_width.load(std::memory_order_relaxed);
	if (target == 0) {
		return;
	}
	auto& w = g_widths;
	if (++w.frames < Period) {
		return;
	}
	w.frames = 0;
	// The size most draws render at is the title's current dynamic resolution.
	uint32_t width = 0, binds = 0;
	for (size_t i = 0; i < w.width.size(); i++) {
		if (w.binds[i] > binds || (w.binds[i] == binds && w.width[i] > width)) {
			width = w.width[i];
			binds = w.binds[i];
		}
	}
	w.width = {};
	w.binds = {};
	if (width == 0) {
		return;
	}
	// Titles resize in a few fixed steps (ASTRO BOT: 1920, 2432, 3328, 3840 wide), and every
	// switch recreates their render targets (a visible hitch), so a step near the target is
	// held rather than chased: the band spans from 7% under to 5% over the target. Only two
	// periods in a row outside it move the clock, and in small steps.
	auto       scale = g_scale.load(std::memory_order_relaxed);
	const bool high  = width > target * 105 / 100;
	const bool low   = width < target * 93 / 100;
	w.outside        = (high || low) ? w.outside + 1 : 0;
	if (w.outside >= 2) {
		w.outside = 0;
		scale     = high ? std::min(scale * 1.2, MaxScale) : std::max(scale / 1.2, MinScale);
	}
	g_scale.store(scale, std::memory_order_relaxed);
	if (width != w.last_logged_width) {
		w.last_logged_width = width;
		std::printf("Resolution control: title renders %u wide (target %u), clock scale %.2f\n", width,
		            target, scale);
	}
}

uint64_t Adjust(uint64_t clock) {
	if (!Active()) {
		return clock;
	}
	std::scoped_lock lock {g_clock_mutex};
	if (g_last_real == 0) {
		g_last_real     = clock;
		g_last_reported = clock;
		return clock;
	}
	const auto elapsed = clock > g_last_real ? clock - g_last_real : 0;
	g_last_real        = clock;
	g_last_reported += static_cast<uint64_t>(static_cast<double>(elapsed) *
	                                         g_scale.load(std::memory_order_relaxed));
	return g_last_reported;
}

} // namespace Libs::Graphics::ResolutionControl
