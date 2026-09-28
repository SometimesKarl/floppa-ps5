#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOLUTIONCONTROL_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOLUTIONCONTROL_H_

#include <cstdint>

// Steers a title's dynamic resolution to a chosen size.
//
// Titles with dynamic resolution (ASTRO BOT) pick their render size from the GPU time they
// measure with end-of-pipe timestamps. The emulator writes those from a clock (the GPU thread's
// recording time), so stretching that clock makes the GPU look slower and the title renders
// fewer pixels; shrinking it does the opposite. This watches the size of the render targets the
// title draws into most and adjusts the stretch until that size matches the configured one.
// With no resolution configured the clock is left alone.
namespace Libs::Graphics::ResolutionControl {

// Width the title's main passes should render at, 0 when not steering. Set once at boot from
// emulator-settings.ini (render_resolution=auto|1080p|1440p|2160p) or KYTY_RENDER_RESOLUTION.
void     Configure(uint32_t target_width);
[[nodiscard]] bool Active();

// GPU command thread: a color target bound for drawing, and the end of a guest frame.
void NoteColorTarget(uint32_t width, uint32_t height);
void EndGuestFrame();

// The reference clock as the title should see it (monotonic; stretched while steering).
[[nodiscard]] uint64_t Adjust(uint64_t clock);

} // namespace Libs::Graphics::ResolutionControl

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOLUTIONCONTROL_H_
