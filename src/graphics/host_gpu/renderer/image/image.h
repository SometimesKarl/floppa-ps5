#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_

#include "common/alignment.h"
#include "common/assert.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <algorithm>
#include <array>
#include <compare>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class Buffer;
class CommandScheduler;
struct ImageTestAccess;

using ImageId = Common::SlotId;

struct CachedImageView {
	ImageViewInfo info;
	vk::ImageView view = nullptr;
};

struct ImageUsage {
	bool texture       = false;
	bool storage       = false;
	bool render_target = false;
	bool depth_target  = false;
	bool video_out     = false;
};

struct ImageBinding {
	vk::ImageLayout  attachment_layout = vk::ImageLayout::eUndefined;
	vk::AccessFlags2 attachment_access;
	bool             is_bound      = false;
	bool             is_target     = false;
	bool             needs_rebind  = false;
	bool             force_general = false;
	bool             shader_write  = false;
};

class Image final {
public:
	Image(GraphicContext& graphics, CommandScheduler& scheduler, const ImageInfo& info);
	~Image();
	KYTY_CLASS_NO_COPY(Image);

	// False when video and system memory both refused the image; the texture cache then frees
	// idle images and creates it again.
	[[nodiscard]] bool Allocated() const noexcept {
		return info.pixel_format == vk::Format::eUndefined || backing.image != nullptr;
	}
	[[nodiscard]] vk::ImageView FindView(const ImageViewInfo& view_info);
	// Levels of the host image (fewer than the guest's with reduced texture quality).
	[[nodiscard]] uint32_t HostLevels() const noexcept {
		return info.resources.levels - info.host_mip_skip;
	}
	// Whether a view may use this format. Images without a view-format list allow every
	// compatible format.
	[[nodiscard]] bool AllowsViewFormat(vk::Format format) const noexcept;
	using Barriers = std::vector<vk::ImageMemoryBarrier2>;
	[[nodiscard]] Barriers GetBarriers(vk::ImageLayout                      destination_layout,
	                                   vk::AccessFlags2                     destination_access,
	                                   vk::PipelineStageFlags2              destination_stage,
	                                   std::optional<ImageSubresourceRange> range);
	void AppendBarriers(Barriers& barriers, vk::ImageLayout destination_layout,
	                    vk::AccessFlags2 destination_access, vk::PipelineStageFlags2 destination_stage,
	                    std::optional<ImageSubresourceRange> range);
	void Transit(vk::ImageLayout destination_layout, vk::AccessFlags2 destination_access,
	             std::optional<ImageSubresourceRange> range, vk::CommandBuffer command_buffer);
	void Upload(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
	            uint64_t size);
	void Download(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
	              uint64_t size);
	void CopyImage(Image& source);
	void Resolve(Image& source, const ImageSubresourceRange& source_range,
	             const ImageSubresourceRange& destination_range);
	void CopyImageWithBuffer(Image& source, Buffer& buffer);
	void CopyMip(Image& source, uint32_t mip, uint32_t layer);

	void InvalidateCpuWrite(uint64_t vaddr, uint64_t size) {
		if (ImageRangeOverlaps(info.data.address, info.data.size, vaddr, size)) {
			m_cpu_dirty        = true;
			m_maybe_cpu_dirty  = false;
			m_maybe_hash_valid = false;
		} else if (ImagePageRangesOverlap(info.data.address, info.data.size, vaddr, size)) {
			m_maybe_cpu_dirty = true;
		}
	}

	[[nodiscard]] bool IsCpuDirty() const { return m_cpu_dirty || m_maybe_cpu_dirty; }
	[[nodiscard]] bool IsDefinitelyCpuDirty() const { return m_cpu_dirty; }
	[[nodiscard]] bool IsMaybeCpuDirty() const { return m_maybe_cpu_dirty; }
	void               MarkMaybeCpuDirty() {
		if (!m_cpu_dirty) {
			m_maybe_cpu_dirty = true;
		}
	}
	[[nodiscard]] bool NeedsMaybeCpuHash() const {
		return m_maybe_cpu_dirty && !m_maybe_hash_valid;
	}
	void SetMaybeCpuHash(uint64_t hash) {
		if (!NeedsMaybeCpuHash()) {
			EXIT("image cannot initialize maybe-dirty hash\n");
		}
		m_maybe_cpu_hash   = hash;
		m_maybe_hash_valid = true;
	}
	[[nodiscard]] bool ResolveMaybeCpuHash(uint64_t hash) {
		if (!m_maybe_cpu_dirty || !m_maybe_hash_valid || m_cpu_dirty) {
			EXIT("image cannot resolve maybe-dirty hash\n");
		}
		m_maybe_cpu_dirty  = false;
		m_maybe_hash_valid = false;
		m_cpu_dirty |= hash != m_maybe_cpu_hash;
		return m_cpu_dirty;
	}

	void RefreshComplete() {
		if (!IsCpuDirty()) {
			EXIT("clean image cannot complete a refresh\n");
		}
		m_cpu_dirty        = false;
		m_maybe_cpu_dirty  = false;
		m_maybe_hash_valid = false;
	}

	[[nodiscard]] bool IsGpuModified() const noexcept { return m_gpu_modified; }
	void               MarkGpuModified() noexcept { m_gpu_modified = true; }
	void               ClearGpuModified() noexcept { m_gpu_modified = false; }

	[[nodiscard]] bool IsBufferModified() const noexcept { return m_buffer_modified; }
	// GPU buffer writes changed guest bytes of the image, which ones unknown.
	void MarkBufferModified() noexcept {
		m_buffer_modified    = true;
		m_buffer_dirty_bands = ~uint64_t {0};
	}
	// GPU buffer writes changed the image's guest bytes [begin, end) (guest addresses). Marks
	// accumulate per 64th of the image (BufferDirtyBand) until the image is refreshed: two small
	// writes at both ends of an image do not mark all of it.
	void MarkBufferModified(uint64_t begin, uint64_t end) noexcept {
		m_buffer_modified = true;
		if (begin >= end || info.data.size == 0) {
			return;
		}
		const auto band  = BufferDirtyBand();
		const auto first = std::min<uint64_t>((begin - std::min(begin, info.data.address)) / band, 63);
		const auto last  = std::min<uint64_t>((end - 1 - std::min(end - 1, info.data.address)) / band, 63);
		m_buffer_dirty_bands |= (~uint64_t {0} >> (63 - last)) & (~uint64_t {0} << first);
	}
	void ClearBufferModified() noexcept {
		m_buffer_modified    = false;
		m_buffer_dirty_bands = 0;
	}
	// Bit i: guest bytes [address + i * BufferDirtyBand(), + BufferDirtyBand()) changed.
	[[nodiscard]] uint64_t BufferDirtyBands() const noexcept { return m_buffer_dirty_bands; }
	[[nodiscard]] uint64_t BufferDirtyBand() const noexcept {
		return std::max<uint64_t>((info.data.size + 63) / 64, 1);
	}

	[[nodiscard]] bool Overlaps(uint64_t address, uint64_t size,
	                            bool pages = false) const noexcept {
		return pages ? ImagePageRangesOverlap(info.data.address, info.data.size, address, size)
		             : ImageRangeOverlaps(info.data.address, info.data.size, address, size);
	}
	[[nodiscard]] bool SafeToDownload() const noexcept {
		return IsGpuModified() && !IsBufferModified() && !IsCpuDirty();
	}
	[[nodiscard]] bool IsTracked() const noexcept { return track_addr != 0 && track_addr_end != 0; }
	[[nodiscard]] uint64_t AccountedSize() const noexcept {
		return backing.image == nullptr ? 0 : Common::AlignUp(info.data.size, 1024);
	}
	[[nodiscard]] uint64_t HashGuestEdges() const;

	ImageInfo        info;
	VulkanImage      backing;
	std::vector<CachedImageView> views;
	ImageUsage       usage;
	ImageBinding     binding;
	bool             registered     = false;
	mutable uint32_t query_epoch    = 0;
	uint64_t         track_addr     = 0;
	uint64_t         track_addr_end = 0;
	ImageId          depth_id {};
	uint64_t         tick_accessed_last = 0;
	// Guest frame of the last lookup (RenderStats::g_guest_frames; UINT64_MAX: never).
	uint64_t         frame_accessed_last = UINT64_MAX;
	// Guest frames this image was last bound as a render target in (UINT64_MAX: never): a target
	// drawn every frame can miss one draw while its pipeline compiles; one drawn once cannot.
	uint64_t         target_frame      = UINT64_MAX;
	uint64_t         prev_target_frame = UINT64_MAX;
	// KYTY_UPLOAD_LOG: bytes of this image GPU buffer writes overlapped since its last upload.
	uint64_t         dirty_write_bytes = 0;
	uint64_t         lru_touch_tick     = UINT64_MAX;
	size_t           lru_id             = 0;
	// A view outside the view-format list was needed; the texture cache recreates the image
	// without the list on its next lookup.
	bool             needs_unrestricted_views = false;

private:
	friend struct ImageTestAccess;

	[[nodiscard]] static vk::ImageAspectFlags FullAspectMask(vk::Format format) noexcept;
	[[nodiscard]] static uint32_t             CopyRows(uint64_t row_size, uint32_t rows,
	                                                   uint64_t capacity) noexcept;
	[[nodiscard]] static std::pair<uint32_t, uint32_t>
	SanitizeCopyLayers(const Image& source, const Image& destination, uint32_t depth);

	GraphicContext&           m_graphics;
	CommandScheduler&         m_scheduler;
	std::array<vk::Format, 2> m_view_formats {};
	uint32_t                  m_view_format_count = 0;
	uint64_t                  m_maybe_cpu_hash   = 0;
	bool              m_cpu_dirty        = false;
	bool              m_maybe_cpu_dirty  = false;
	bool              m_maybe_hash_valid = false;
	bool              m_gpu_modified     = false;
	bool              m_buffer_modified  = false;
	uint64_t          m_buffer_dirty_bands = 0;
};

namespace ImageOps {

void                                 Validate(const ImageInfo& info);
[[nodiscard]] Prospero::BufferFormat RenderTargetTransferFormat(uint32_t bytes_per_element);

} // namespace ImageOps

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
