#ifndef EMULATOR_SRC_COMMON_ALLOCSAMPLER_H_
#define EMULATOR_SRC_COMMON_ALLOCSAMPLER_H_

namespace Common {

// With KYTY_ALLOC_SAMPLER=1, heap allocations made by the calling thread are counted and their
// call stacks sampled (see allocSampler.cpp). No effect otherwise.
void AllocSamplerEnableThread();

} // namespace Common

#endif // EMULATOR_SRC_COMMON_ALLOCSAMPLER_H_
