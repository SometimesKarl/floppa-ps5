#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREQUALITY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREQUALITY_H_

#include <atomic>

namespace Libs::Graphics::TextureQuality {

// emulator-settings.ini texture_quality=reduced (KYTY_TEXTURE_QUALITY): large read-only textures
// that shaders only filter-sample are kept on the GPU without their largest mip level, a quarter
// of their video memory. Anything that needs the exact texels promotes the texture back to full
// quality from guest memory, so no guest data is lost; close-up detail is softer.
inline std::atomic<bool> g_reduced {false};

inline void SetReduced(bool reduced) {
	g_reduced.store(reduced, std::memory_order_relaxed);
}
[[nodiscard]] inline bool Reduced() {
	return g_reduced.load(std::memory_order_relaxed);
}

} // namespace Libs::Graphics::TextureQuality

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTUREQUALITY_H_ */
