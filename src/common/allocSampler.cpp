// KYTY_ALLOC_SAMPLER=1: counts heap allocations made on threads that opted in
// (Common::AllocSamplerEnableThread) and records the call stacks of one in 8 of them. Every 20 s
// the busiest stacks are printed as module offsets, for tools/symbolize_allocs.py. Diagnostics
// for per-draw allocation churn on the GPU command thread; free when disabled (one TLS check).

#include "common/allocSampler.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

constexpr size_t   Depth = 6;
constexpr size_t   Slots = 8192;
thread_local bool  t_enabled   = false;
thread_local bool  t_recording = false;
thread_local uint32_t t_counter = 0;

struct Slot {
	std::atomic<uint64_t>             key {0};
	std::atomic<uint64_t>             count {0};
	std::atomic<uint64_t>             bytes {0};
	std::array<uint64_t, Depth>       frames {};
};

std::array<Slot, Slots>* g_slots = nullptr;
std::atomic<uint64_t>    g_total {0};
std::atomic<uint64_t>    g_total_bytes {0};
uint64_t                 g_image_base = 0;

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_ALLOC_SAMPLER");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

void Dump() {
	static auto last = std::chrono::steady_clock::now();
	const auto  now  = std::chrono::steady_clock::now();
	if (now - last < std::chrono::seconds(20)) {
		return;
	}
	last = now;
	std::array<size_t, 40> top {};
	size_t                 filled = 0;
	for (size_t i = 0; i < Slots; i++) {
		const auto count = (*g_slots)[i].count.load(std::memory_order_relaxed);
		if (count == 0) {
			continue;
		}
		size_t pos = filled < top.size() ? filled++ : top.size();
		while (pos > 0 && (*g_slots)[top[pos - 1]].count.load(std::memory_order_relaxed) < count) {
			if (pos < top.size()) {
				top[pos] = top[pos - 1];
			}
			pos--;
		}
		if (pos < top.size()) {
			top[pos] = i;
		}
	}
	std::printf("alloc sampler: %llu allocations, %llu bytes on sampled threads\n",
	            static_cast<unsigned long long>(g_total.load()),
	            static_cast<unsigned long long>(g_total_bytes.load()));
	for (size_t i = 0; i < filled; i++) {
		const auto& slot = (*g_slots)[top[i]];
		std::printf("alloc %llu %llu", static_cast<unsigned long long>(slot.count.load() * 8),
		            static_cast<unsigned long long>(slot.bytes.load() * 8));
		for (const auto frame: slot.frames) {
			std::printf(" 0x%llx", static_cast<unsigned long long>(frame >= g_image_base ? frame - g_image_base : 0));
		}
		std::printf("\n");
	}
	std::fflush(stdout);
}

void Record(size_t size) {
#if defined(_WIN32)
	g_total.fetch_add(1, std::memory_order_relaxed);
	g_total_bytes.fetch_add(size, std::memory_order_relaxed);
	if ((++t_counter & 7u) != 0 || t_recording) {
		return;
	}
	t_recording = true;
	void*          frames[Depth + 2] {};
	const auto     captured = RtlCaptureStackBackTrace(2, Depth, frames, nullptr);
	uint64_t       key      = 1469598103934665603ull;
	for (USHORT i = 0; i < captured; i++) {
		key = (key ^ reinterpret_cast<uint64_t>(frames[i])) * 1099511628211ull;
	}
	key |= 1;
	for (size_t probe = 0; probe < 64; probe++) {
		auto&    slot     = (*g_slots)[(key + probe) % Slots];
		uint64_t expected = 0;
		if (slot.key.load(std::memory_order_acquire) == key ||
		    slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
			if (expected == 0 && slot.frames[0] == 0) {
				for (USHORT i = 0; i < captured && i < Depth; i++) {
					slot.frames[i] = reinterpret_cast<uint64_t>(frames[i]);
				}
			}
			slot.count.fetch_add(1, std::memory_order_relaxed);
			slot.bytes.fetch_add(size, std::memory_order_relaxed);
			break;
		}
	}
	Dump();
	t_recording = false;
#else
	(void)size;
#endif
}

} // namespace

namespace Common {

void AllocSamplerEnableThread() {
	if (!Enabled()) {
		return;
	}
	static const bool initialized = [] {
		g_slots = new std::array<Slot, Slots>();
#if defined(_WIN32)
		g_image_base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
#endif
		return true;
	}();
	(void)initialized;
	t_enabled = true;
}

} // namespace Common

void* operator new(size_t size) {
	if (t_enabled) {
		Record(size);
	}
	void* p = std::malloc(size != 0 ? size : 1);
	if (p == nullptr) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		throw std::bad_alloc();
#else
		std::abort(); // Linux builds run without exceptions
#endif
	}
	return p;
}

void* operator new[](size_t size) {
	return operator new(size);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
	if (t_enabled) {
		Record(size);
	}
	return std::malloc(size != 0 ? size : 1);
}

void* operator new[](size_t size, const std::nothrow_t& tag) noexcept {
	return operator new(size, tag);
}

void operator delete(void* p) noexcept {
	std::free(p);
}

void operator delete[](void* p) noexcept {
	std::free(p);
}

void operator delete(void* p, size_t) noexcept {
	std::free(p);
}

void operator delete[](void* p, size_t) noexcept {
	std::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
	std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
	std::free(p);
}
