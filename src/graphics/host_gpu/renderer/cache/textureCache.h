#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/tickHistory.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionManager.h"
#include "graphics/host_gpu/renderer/cache/dccClearResolver.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/image/blitHelper.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/tiler.h"

#include <array>
#include <map>
#include <source_location>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class Buffer;
class BufferCache;
class CommandBuffer;
class CommandScheduler;
class RenderExecutor;
struct TextureCacheTestAccess;

class TextureCache {
public:
	enum class BindingType : uint8_t { Texture, Storage, RenderTarget, DepthTarget, VideoOut };

	struct ImageDesc {
		ImageInfo     info;
		ImageViewInfo view_info;
		BindingType   type = BindingType::Texture;
		// The shader only filter-samples or gathers the image: it may use a reduced-quality host
		// image (TextureQuality). Every other lookup gets, or promotes to, the full image.
		bool          allow_reduced = false;
	};

	TextureCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	             BufferCache& buffer_cache);
	~TextureCache();
	KYTY_CLASS_NO_COPY(TextureCache);

	[[nodiscard]] ImageId       FindImage(ImageDesc& desc, bool exact_format = false);
	void                        UpdateImage(ImageId id);
	[[nodiscard]] ImageId       FindImageFromRange(uint64_t address, uint64_t size,
	                                               bool ensure_valid = true);
	[[nodiscard]] vk::ImageView FindTexture(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindRenderTarget(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindDepthTarget(ImageId id, const ImageDesc& desc);
	[[nodiscard]] Image&        GetImage(ImageId id) {
		auto& image = m_slot_images[id];
		TouchImage(image);
		return image;
	}
	void MarkGpuWritten(ImageId id);

	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
	                                        uint32_t packed_clear);
	void               InvalidateMemory(uint64_t address, uint64_t size);
	// `source` names the GPU write (for KYTY_UPLOAD_LOG): 0 fill, 1 copy, 2 shader storage buffer.
	void               InvalidateMemoryFromGPU(uint64_t address, uint64_t size, uint32_t source = 3);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t address, uint64_t size);

	[[nodiscard]] bool IsMeta(uint64_t address);
	[[nodiscard]] bool IsMetaCleared(uint64_t address, uint32_t slice);
	[[nodiscard]] bool ClearMeta(uint64_t address);
	[[nodiscard]] bool TouchMeta(uint64_t address, uint32_t slice, bool is_clear);

	void UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	void RunGarbageCollector();

private:
	enum class TransferDirection { Upload, Download };
	struct TextureTransfer;
	struct ImageDownload;

	// KYTY_UPLOAD_LOG: the operation that needed an image's guest contents (set around the
	// lookups that can upload).
	enum class UploadReason : uint8_t {
		Other,
		Texture,
		Storage,
		RenderTarget,
		DepthTarget,
		ConditionalClear,
		PartialClear,
		CopySource,
		Recreate,
		Count
	};
	UploadReason m_upload_reason = UploadReason::Other;
	class UploadReasonScope {
	public:
		UploadReasonScope(TextureCache& cache, UploadReason reason)
		    : m_cache(cache), m_previous(cache.m_upload_reason) {
			cache.m_upload_reason = reason;
		}
		~UploadReasonScope() { m_cache.m_upload_reason = m_previous; }
		UploadReasonScope(const UploadReasonScope&)            = delete;
		UploadReasonScope& operator=(const UploadReasonScope&) = delete;

	private:
		TextureCache& m_cache;
		UploadReason  m_previous;
	};

	struct MetaDataInfo {
		enum class Type : uint8_t { CMask, FMask, HTile };

		Type     type;
		uint32_t clear_mask = UINT32_MAX;
	};

	struct OverlapResult {
		ImageId image;
		int32_t mip   = -1;
		int32_t layer = -1;
	};

	using ImageIds       = InlinePageOwnerList<ImageId, 16>;
	using ImagePageTable = MultiLevelPageTable<ImageIds, 20, 44, 14>;
	// Query results: a texture lookup in ASTRO BOT often overlaps more than 16 images, and the
	// page-owner capacity made ~10k lookups a second spill to the heap.
	using ImageQueryIds = InlinePageOwnerList<ImageId, 128>;

	// Callers have validated the nonempty 44-bit range with TryGetPageRange.
	template <typename Func>
	static void ForEachPage(uint64_t address, size_t size, Func&& func) {
		using FuncReturn = typename std::invoke_result<Func, uint64_t>::type;
		static constexpr bool RETURNS_BOOL = std::is_same_v<FuncReturn, bool>;
		const uint64_t page_end = (address + size - 1) >> ImagePageTable::kPageBits;
		for (uint64_t page = address >> ImagePageTable::kPageBits; page <= page_end; ++page) {
			if constexpr (RETURNS_BOOL) {
				if (func(page)) {
					break;
				}
			} else {
				func(page);
			}
		}
	}

	// `protect` is an image the caller keeps using after the insertion; it is never reclaimed.
	[[nodiscard]] ImageId     InsertImage(const ImageInfo& info, ImageId protect = {},
	                                      std::source_location where = std::source_location::current());
	// Frees idle images, oldest first, and waits for the GPU so their memory is returned: used
	// when video and system memory both refuse a new image. Returns the bytes freed.
	uint64_t                  ReclaimForAllocation(uint64_t needed, ImageId protect, bool aggressive);
	// Largest GPU-written image an eviction writes back to guest memory (in one piece, through
	// the 64 MiB download ring); larger ones are kept.
	static constexpr uint64_t EvictionDownloadMax = 32ull * 1024 * 1024;
	// Whether evicting `image` loses nothing: not GPU-written, or writable back to guest memory.
	[[nodiscard]] bool        CanPreserveForEviction(const Image& image);
	// Writes a GPU-written image back before its eviction, within `budget` bytes (reduced by
	// what was written).
	enum class Preserve {
		Evictable, // nothing to write back, or written back
		Keep,      // cannot be written back: the image stays
		Later,     // over this call's budget
	};
	[[nodiscard]] Preserve    PreserveForEviction(ImageId id, uint64_t& budget);
	[[nodiscard]] ImageId     GetNullImage(const ImageDesc& desc);
	void                      RegisterImage(ImageId id);
	void                      UnregisterImage(ImageId id);
	// defer_erase=false leaves the slot for the caller to erase once the GPU is idle.
	void                      DeleteImage(ImageId id, bool defer_erase = true,
	                                      std::source_location where = std::source_location::current());
	void                      FreeImage(ImageId id,
	                                    std::source_location where = std::source_location::current());
	void                      TouchImage(Image& image);
	void                      TrackImage(ImageId id);
	void                      TrackImageHead(ImageId id);
	void                      TrackImageTail(ImageId id);
	void                      UntrackImage(ImageId id);
	void                      UntrackImageHead(ImageId id);
	void                      UntrackImageTail(ImageId id);
	void                      MarkAsMaybeDirty(ImageId id, Image& image);
	void                      TrackImageDownload(ImageId id, Image& image);
	[[nodiscard]] static bool SameBacking(const ImageInfo& cached, const ImageInfo& requested,
	                                      bool exact_format);
	[[nodiscard]] static BindingType UploadBinding(const Image& image);

	// Caller holds m_lock; it also serializes the per-image query epoch.
	[[nodiscard]] ImageQueryIds FindImagesInRegion(uint64_t address, uint64_t size,
	                                               bool page_overlap) const;
	[[nodiscard]] OverlapResult ResolveOverlap(const ImageInfo& requested, BindingType binding,
	                                           ImageId cached, ImageId merged);
	[[nodiscard]] ImageId       ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
	                                                ImageId cached);
	[[nodiscard]] ImageId       ExpandImage(const ImageInfo& info, ImageId source);
	[[nodiscard]] ImageId       RecreateWithUnrestrictedViews(ImageId source);
	void                        RefreshImage(ImageId id);
	void                        MaterializeColorClear(ImageId id, const ImageDesc& desc,
	                                                uint32_t metadata_base_layer);
	[[nodiscard]] bool          MaterializeDccClearOnGpu(ImageId id, const ImageDesc& desc,
	                                                     uint32_t first, uint32_t image_first,
	                                                     uint32_t count, uint32_t layers);
	void                        InitializeImage(ImageId id);
	[[nodiscard]] TextureTransfer
	BuildTextureTransfer(const Image& image, BindingType binding, TransferDirection direction) const;
	[[nodiscard]] ImageDownload BuildDownload(const Image& image) const;
	// changed_rows_only: the image already holds its guest contents except where GPU buffer writes
	// changed them (its buffer-dirty range); only the rows those bytes lie in are uploaded when the
	// layout allows it.
	void UploadImage(Image& image, Buffer& source, uint64_t source_offset,
	                 bool changed_rows_only = false);
	void DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
	                       uint64_t destination_size, ImageDownload transfer);
	void DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset);
	void CommitGpuWrite(Image& image);
	// Caller holds m_lock. Volume layer ranges select depth slices.
	void ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
	                const vk::ImageSubresourceRange& range, const vk::ClearValue& clear);
	// Caller holds m_lock. Clears one color level to clears[k] when predicate k is nonzero;
	// at most one predicate may be set.
	void ClearColorIfPredicate(CommandBuffer& command, ImageId id, vk::Format format,
	                           const vk::ImageSubresourceRange&     range,
	                           std::span<const vk::ClearColorValue> clears,
	                           const DccClearResolver::Predicates&  predicates);
	void PrepareImageCopy(Image& image);
	void RefreshCopySource(ImageId id);
	[[nodiscard]] bool CopyD16(Image& destination, Image& source);
	void               CopyImage(ImageId destination, ImageId source);
	[[nodiscard]] ImageId AssociateStencil(ImageId depth, GuestRange stencil);
	void CopyImageMip(ImageId destination, ImageId source, uint32_t mip, uint32_t layer);
	void ValidateImageDesc(const ImageDesc& desc) const;

	void               InvalidateCpuAliases(uint64_t address, uint64_t size);
	[[nodiscard]] bool DownloadImageMemory(ImageId id);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	TrackingSpinLock                                  m_lock;
	PageManager&                                      m_page_manager;
	BlitHelper                                        m_blit_helper;
	TileManager                                       m_tiler;
	BufferCache&                                      m_buffer_cache;
	DccClearResolver                                  m_dcc_clear_resolver;
	Common::SlotVector<Image>                         m_slot_images;
	ImagePageTable                                    m_image_page_table;
	std::unordered_map<vk::Format, ImageId>           m_null_images;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::unordered_set<ImageId>                       m_download_images;
	std::map<uint64_t, MetaDataInfo>                  m_surface_metas;
	// Per DCC metadata slice: the watched write generation when last tested on the GPU and the
	// clear codes tested since then. Until a guest write, a tested slice either did not match
	// those codes or was already expanded, so testing it again cannot find a clear.
	struct DccCheck {
		uint64_t generation   = 0;
		uint8_t  tested_codes = 0;
	};
	std::unordered_map<uint64_t, DccCheck>            m_dcc_checks;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_gc_freed = 0;
	uint64_t m_gc_kept  = 0;
	uint64_t m_gc_emergency_freed = 0;
	uint64_t m_gc_preserved_bytes = 0;
	// KYTY_UPLOAD_LOG: bytes partial uploads (UploadImage changed_rows_only) did not transfer.
	uint64_t m_partial_upload_bytes_saved = 0;
	// Reduced texture quality (TextureQuality): whether a new image for `desc` may leave out its
	// top level, and the guest addresses a lookup needed at full quality (never reduced again).
	[[nodiscard]] bool MayReduce(const ImageDesc& desc);
	std::unordered_set<uint64_t> m_full_quality_addresses;
	uint64_t                     m_reduced_images  = 0;
	uint64_t                     m_promoted_images = 0;
	uint64_t                     m_trimmed_bytes   = 0;
	uint64_t                     m_trim_skipped_bytes = 0;
	// KYTY_UPLOAD_LOG: why buffer-written images were uploaded whole (PartialMiss order).
	std::array<uint64_t, 8> m_partial_misses {};
	uint64_t m_overlap_freed = 0;
	Common::TickHistory m_tick_history;
	// Near the device budget (a level load outrunning the regular passes): frees images unused
	// for 5 s, writing GPU-written ones back to guest memory first (a bounded amount per call);
	// those that cannot be written back stay. Caller holds m_lock.
	// `critical`: video memory is almost full; images unused for a second go instead of five.
	void EmergencyCollect(uint64_t tick, bool critical);
	uint64_t                                          m_trigger_gc_memory  = 0;
	uint64_t                                          m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t         m_critical_gc_memory     = 3ull * 1024 * 1024 * 1024;
	uint64_t         m_gc_tick                = 0;
	mutable uint32_t m_image_query_epoch      = 0;
	bool             m_readback_linear_images = false;

	friend struct TextureCacheTestAccess;
	friend class BufferCache;
	friend class RenderExecutor;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
