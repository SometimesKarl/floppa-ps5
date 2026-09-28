#include "graphics/host_gpu/renderer/hitchStats.h"

#include "common/timer.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics::HitchStats {

namespace {

constexpr double HitchMs = 45.0;

thread_local bool g_bound = false;

// GPU command thread only.
struct Frame {
	std::array<uint64_t, static_cast<size_t>(Category::Count)> ticks {};
	uint64_t textures      = 0;
	uint64_t texture_bytes = 0;
	uint64_t begin         = 0;
	// Nested scopes (an upload inside a pipeline build) count once, in the outer category.
	uint32_t depth         = 0;
};
Frame g_frame;

struct Summary {
	uint64_t frames  = 0;
	uint64_t hitches = 0;
	std::array<uint64_t, static_cast<size_t>(Category::Count)> dominant {};
	uint64_t other   = 0;
	uint64_t since   = 0;
};
Summary g_summary;

constexpr std::array<const char*, static_cast<size_t>(Category::Count)> Names {
    "shader", "pipeline", "texture", "buffer", "gpu-wait", "guest-idle", "flip-wait", "ring-wait"};

// Time of the scopes nested in each open scope, so every scope is charged its own time only
// (a GPU wait inside a texture upload is a GPU wait).
thread_local std::array<uint64_t, 16> g_child_ticks {};

double Ms(uint64_t ticks) {
	return static_cast<double>(ticks) * 1000.0 /
	       static_cast<double>(Common::Timer::QueryPerformanceFrequency());
}

} // namespace

bool Enabled() {
	static const bool enabled = std::getenv("KYTY_HITCH_LOG") != nullptr;
	return enabled;
}

void BindThisThread() {
	g_bound = true;
}

void Add(Category category, uint64_t qpc_ticks) {
	if (g_bound) {
		g_frame.ticks[static_cast<size_t>(category)] += qpc_ticks;
	}
}

void CountTexture(uint64_t bytes) {
	if (g_bound && Enabled()) {
		g_frame.textures++;
		g_frame.texture_bytes += bytes;
	}
}

Scope::Scope(Category category): m_category(category) {
	if (g_bound && Enabled() && g_frame.depth < g_child_ticks.size()) {
		m_active                        = true;
		g_child_ticks[g_frame.depth++] = 0;
		m_begin                         = Common::Timer::QueryPerformanceCounter();
	}
}

Scope::~Scope() {
	if (!m_active) {
		return;
	}
	const auto elapsed = Common::Timer::QueryPerformanceCounter() - m_begin;
	const auto depth   = --g_frame.depth;
	const auto nested  = g_child_ticks[depth];
	Add(m_category, elapsed > nested ? elapsed - nested : 0);
	if (depth > 0) {
		g_child_ticks[depth - 1] += elapsed;
	}
}

void EndGuestFrame() {
	if (!g_bound || !Enabled()) {
		return;
	}
	const auto now = Common::Timer::QueryPerformanceCounter();
	auto&      f   = g_frame;
	auto&      s   = g_summary;
	if (s.since == 0) {
		s.since = now;
	}
	if (f.begin != 0) {
		const auto total = Ms(now - f.begin);
		s.frames++;
		if (total > HitchMs) {
			s.hitches++;
			double   accounted = 0;
			size_t   top       = 0;
			for (size_t i = 0; i < f.ticks.size(); i++) {
				accounted += Ms(f.ticks[i]);
				if (f.ticks[i] > f.ticks[top]) {
					top = i;
				}
			}
			const double other = total - accounted;
			if (other > Ms(f.ticks[top])) {
				s.other++;
			} else {
				s.dominant[top]++;
			}
			std::printf("hitch qpc=%llu frame %.0f ms:", static_cast<unsigned long long>(now), total);
			for (size_t i = 0; i < f.ticks.size(); i++) {
				if (Ms(f.ticks[i]) >= 1.0) {
					std::printf(" %s %.0f", Names[i], Ms(f.ticks[i]));
				}
			}
			std::printf(" other %.0f | %llu new textures %.1f MiB\n", other,
			            static_cast<unsigned long long>(f.textures),
			            static_cast<double>(f.texture_bytes) / 1048576.0);
		}
		if (Ms(now - s.since) >= 10000.0) {
			std::printf("hitch summary: %llu of %llu frames over %.0f ms; mostly:",
			            static_cast<unsigned long long>(s.hitches),
			            static_cast<unsigned long long>(s.frames), HitchMs);
			for (size_t i = 0; i < s.dominant.size(); i++) {
				if (s.dominant[i] != 0) {
					std::printf(" %s %llu", Names[i], static_cast<unsigned long long>(s.dominant[i]));
				}
			}
			std::printf(" other %llu\n", static_cast<unsigned long long>(s.other));
			std::fflush(stdout);
			s = {};
			s.since = now;
		}
	}
	const auto depth = f.depth;
	f        = {};
	f.depth  = depth;
	f.begin  = now;
}

} // namespace Libs::Graphics::HitchStats
