#include "common/assert.h"
#include "graphics/host_gpu/renderer/renderStats.h"
#include "common/common.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <bit>
#include <cstring>
namespace Libs::Graphics {

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_graphics(scheduler.Graphics()) {}

bool CommandBuffer::IsInvalid() const {
	return m_buffer == nullptr;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	EXIT_IF(IsInvalid());
	m_handle_uses++;
	return m_buffer;
}

void CommandBuffer::Begin() {
	EXIT_IF(m_rendering || IsInvalid());
	m_global_barrier_uses = UINT64_MAX;
	auto buffer = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::End() const {
	EndRendering();
	auto buffer = Handle();

	auto result = buffer.end();

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                                 uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = arg0;
	m_debug_arg1      = arg1;
	m_debug_arg2      = arg2;
	m_debug_arg3      = arg3;
	m_debug_arg4      = arg4;
}

void CommandBuffer::BeginRendering(const RenderState& state) const {
	if (m_rendering && m_render_state == state) {
		return;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	if (m_rendering && RenderStats::Enabled()) {
		// What the next draw needs differently from the rendering in progress.
		const auto& a = m_render_state;
		const auto& b = state;
		bool views = a.num_color_attachments != b.num_color_attachments ||
		             a.depth_stencil_attachment.image_view != b.depth_stencil_attachment.image_view;
		bool layouts = a.depth_stencil_attachment.image_layout != b.depth_stencil_attachment.image_layout;
		bool clears  = a.depth_stencil_attachment.depth_clear != b.depth_stencil_attachment.depth_clear ||
		              a.depth_stencil_attachment.stencil_clear !=
		                  b.depth_stencil_attachment.stencil_clear;
		for (uint32_t i = 0; i < std::min(a.num_color_attachments, b.num_color_attachments); i++) {
			views |= a.color_attachments[i].image_view != b.color_attachments[i].image_view;
			layouts |= a.color_attachments[i].image_layout != b.color_attachments[i].image_layout;
			clears |= a.color_attachments[i].is_clear != b.color_attachments[i].is_clear;
		}
		const bool extent = a.width != b.width || a.height != b.height || a.num_layers != b.num_layers;
		RenderStats::CountBreak(views    ? "new: other attachments"
		                        : extent ? "new: same attachments, other extent"
		                        : layouts ? "new: same attachments, other layout"
		                        : clears  ? "new: same attachments, clear"
		                                  : "new: same attachments, other state",
		                        0);
		EndRendering(std::source_location {});
	} else {
		EndRendering();
	}
	RenderStats::Count(RenderStats::g_begin_rendering);

	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		colors[i].imageView    = attachment.image_view;
		colors[i].imageLayout  = attachment.image_layout;
		colors[i].loadOp =
		    attachment.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp                 = vk::AttachmentStoreOp::eStore;
		colors[i].clearValue.color.uint32 = attachment.clear_value;
	}

	const auto&                 depth_stencil = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {};
	depth.imageView   = depth_stencil.image_view;
	depth.imageLayout = depth_stencil.image_layout;
	depth.loadOp =
	    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	// A read-only aspect is not stored: the attachment is then only read (see
	// RenderExecutor::AcquireRenderTargets).
	const auto writable = DepthWritableAspects(depth_stencil.image_layout);
	depth.storeOp = (writable & vk::ImageAspectFlagBits::eDepth) ? vk::AttachmentStoreOp::eStore
	                                                             : vk::AttachmentStoreOp::eNone;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

	vk::RenderingAttachmentInfo stencil {};
	stencil.imageView   = depth_stencil.image_view;
	stencil.imageLayout = depth_stencil.image_layout;
	stencil.loadOp =
	    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.storeOp = (writable & vk::ImageAspectFlagBits::eStencil) ? vk::AttachmentStoreOp::eStore
	                                                                 : vk::AttachmentStoreOp::eNone;
	stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = {state.width, state.height};
	rendering.layerCount           = state.num_layers;
	rendering.colorAttachmentCount = state.num_color_attachments;
	rendering.pColorAttachments    = colors.data();
	rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
	rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
	Handle().beginRendering(rendering);
	m_render_state = state;
	m_rendering    = true;
}

void CommandBuffer::EndRendering(std::source_location where) const {
	if (!m_rendering) {
		return;
	}
	Handle().endRendering();
	RenderStats::Count(RenderStats::g_end_rendering);
	if (where.line() != 0 && RenderStats::Enabled()) {
		RenderStats::CountBreak(where.file_name(), where.line());
	}
	m_rendering    = false;
	m_render_state = {};
}

} // namespace Libs::Graphics
