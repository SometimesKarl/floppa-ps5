#include "common/lowMemoryGuard.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>

namespace {

using Common::LowMemoryGuard;
using Action = LowMemoryGuard::Action;

int g_checks = 0;

void Check(bool value, const char* message) {
	g_checks++;
	if (!value) {
		std::fprintf(stderr, "LowMemoryGuardTests: failed: %s\n", message);
		std::abort();
	}
}

// Feeds samples (MiB; -1 = unknown) and returns the index of the first Stop, or -1.
int FirstStop(LowMemoryGuard::Settings settings, std::initializer_list<long long> samples) {
	LowMemoryGuard guard(settings);
	int            index = 0;
	for (const auto sample: samples) {
		const auto value = sample < 0 ? std::nullopt : std::optional<uint64_t>(sample);
		if (guard.Sample(value) == Action::Stop) {
			return index;
		}
		index++;
	}
	return -1;
}

void TestDefaultRulesMatchTheOldGuard() {
	const LowMemoryGuard::Settings settings {.stop_mib = 400};
	Check(FirstStop(settings, {5000, 399, 399, 399, 399}) == 4, "four low samples did not stop");
	Check(FirstStop(settings, {399, 399, 399, 401, 399, 399, 399}) == -1,
	      "a recovered sample did not restart the count");
	Check(FirstStop(settings, {400, 400, 400, 400, 400}) == -1, "the threshold itself stopped");
	// Default: no stop on a single deep sample (the old dwell of 2 s).
	Check(FirstStop(settings, {3000, 10, 3000}) == -1, "default guard stopped on one sample");
	Check(FirstStop({.stop_mib = 0}, {0, 0, 0, 0, 0, 0}) == -1, "stop_mib = 0 did not disable");
}

void TestZeroIsAReadingAndUnknownIsNot() {
	const LowMemoryGuard::Settings settings {.stop_mib = 400};
	// The old guard skipped 0 as "unknown": less than 1 MiB free never counted.
	Check(FirstStop(settings, {0, 0, 0, 0}) == 3, "0 MiB readings did not count as low");
	// Unknown samples neither count nor reset the run of low samples.
	Check(FirstStop(settings, {300, -1, 300, -1, 300, 300}) == 5,
	      "unknown samples changed the low count");
	Check(FirstStop(settings, {-1, -1, -1, -1, -1, -1}) == -1, "unknown samples stopped");
}

void TestWarnings() {
	LowMemoryGuard guard({.stop_mib = 400});
	Check(guard.WarnMib() == 1024, "warn threshold for 400 is not 1024");
	Check(LowMemoryGuard({.stop_mib = 800}).WarnMib() == 1312, "warn threshold not stop + 512");
	Check(guard.Sample(5000) == Action::None, "plenty of RAM warned");
	Check(guard.Sample(1000) == Action::Warn, "below the warn threshold did not warn");
	Check(guard.Sample(900) == Action::Warn, "warning not refreshed");
	Check(guard.Sample(1200) == Action::None, "cleared inside the hysteresis band");
	Check(guard.Sample(1300) == Action::ClearWarning, "warning not cleared above warn + 256");
	Check(guard.Sample(1300) == Action::None, "cleared twice");
}

void TestFastStop() {
	const LowMemoryGuard::Settings fast {.stop_mib = 400, .fast_stop = true};
	Check(FirstStop(fast, {3000, 90}) == 1, "fast stop missed a reading below a quarter");
	// 700 -> 350 in one sample: at that rate nothing is left within a second.
	Check(FirstStop(fast, {700, 350}) == 1, "fast stop missed a steep fall");
	// Below the threshold but falling slowly: wait for the normal count.
	Check(FirstStop(fast, {420, 390, 385, 380, 375}) == 4, "fast stop fired on a slow decline");
	// Above the threshold nothing fast-stops, however steep the fall.
	Check(FirstStop(fast, {9000, 500}) == -1, "fast stop fired above the threshold");
	// The first sample has no previous reading: no fall, only the quarter rule.
	Check(FirstStop(fast, {350}) == -1, "first sample treated as a fall");
	Check(FirstStop(fast, {-1, 700, -1, 350}) == 3, "unknown sample broke the fall rate");
}

} // namespace

int main() {
	TestDefaultRulesMatchTheOldGuard();
	TestZeroIsAReadingAndUnknownIsNot();
	TestWarnings();
	TestFastStop();
	std::printf("LowMemoryGuardTests: %d checks passed\n", g_checks);
	return 0;
}
