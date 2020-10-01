#pragma once
#include <avk/avk.hpp>

namespace avk
{
	/** Represents data for a vulkan graphics pipeline */
	class graphics_pipeline_t
	{
		friend class root;
		
	public:
		graphics_pipeline_t() = default;
		graphics_pipeline_t(graphics_pipeline_t&&) noexcept = default;
		graphics_pipeline_t(const graphics_pipeline_t&) = delete;
		graphics_pipeline_t& operator=(graphics_pipeline_t&&) noexcept = default;
		graphics_pipeline_t& operator=(const graphics_pipeline_t&) = delete;
		~graphics_pipeline_t() = default;

		const auto& layout_handle() const { return mPipelineLayout.get(); }
		std::tuple<const graphics_pipeline_t*, const vk::PipelineLayout, const std::vector<vk::PushConstantRange>*> layout() const { return std::make_tuple(this, layout_handle(), &mPushConstantRanges); }
		const auto& handle() const { return mPipeline.get(); }
		const renderpass_t& get_renderpass() const { return mRenderPass; }
		const auto& renderpass_handle() const { return mRenderPass->handle(); }
		auto subpass_id() const { return mSubpassIndex; }

		auto& viewports() { return mViewports; }
		auto& scissors() { return mScissors; }
		// TODO: Further getters
		
	private:
		renderpass mRenderPass;
		uint32_t mSubpassIndex;
		// The vertex input data:
		std::vector<vk::VertexInputBindingDescription> mOrderedVertexInputBindingDescriptions;
		std::vector<vk::VertexInputAttributeDescription> mVertexInputAttributeDescriptions;
		vk::PipelineVertexInputStateCreateInfo mPipelineVertexInputStateCreateInfo;
		// How to interpret the vertex input:
		vk::PipelineInputAssemblyStateCreateInfo mInputAssemblyStateCreateInfo;
		// Our precious GPU shader programs:
		std::vector<shader> mShaders;
		std::vector<vk::PipelineShaderStageCreateInfo> mShaderStageCreateInfos;
		std::vector<vk::SpecializationInfo> mSpecializationInfos;
		// Viewport, depth, and scissors configuration
		std::vector<vk::Viewport> mViewports;
		std::vector<vk::Rect2D> mScissors;
		vk::PipelineViewportStateCreateInfo mViewportStateCreateInfo;
		// Rasterization state:
		vk::PipelineRasterizationStateCreateInfo mRasterizationStateCreateInfo;
		// Depth stencil config:
		vk::PipelineDepthStencilStateCreateInfo mDepthStencilConfig;
		// Color blend attachments
		std::vector<vk::PipelineColorBlendAttachmentState> mBlendingConfigsForColorAttachments;
		vk::PipelineColorBlendStateCreateInfo mColorBlendStateCreateInfo;
		// Multisample state
		vk::PipelineMultisampleStateCreateInfo mMultisampleStateCreateInfo;
		// Dynamic state
		std::vector<vk::DynamicState> mDynamicStateEntries;
		vk::PipelineDynamicStateCreateInfo mDynamicStateCreateInfo;
		// Pipeline layout, i.e. resource bindings
		set_of_descriptor_set_layouts mAllDescriptorSetLayouts;
		std::vector<vk::PushConstantRange> mPushConstantRanges;
		vk::PipelineLayoutCreateInfo mPipelineLayoutCreateInfo;
		std::optional<vk::PipelineTessellationStateCreateInfo> mPipelineTessellationStateCreateInfo;
		
		// TODO: What to do with flags?
		vk::PipelineCreateFlags mPipelineCreateFlags;

		// Handles:
		vk::UniquePipelineLayout mPipelineLayout;
		vk::UniquePipeline mPipeline;
	};
	
	using graphics_pipeline = avk::owning_resource<graphics_pipeline_t>;
	
	template <>
	inline void command_buffer_t::bind_pipeline<graphics_pipeline_t>(const graphics_pipeline_t& aPipeline)
	{
		handle().bindPipeline(vk::PipelineBindPoint::eGraphics, aPipeline.handle());
	}

	template <>
	inline void command_buffer_t::bind_pipeline<graphics_pipeline>(const graphics_pipeline& aPipeline)
	{
		bind_pipeline<graphics_pipeline_t>(aPipeline);
	}

	template <>
	inline void command_buffer_t::bind_descriptors<std::tuple<const graphics_pipeline_t*, const vk::PipelineLayout, const std::vector<vk::PushConstantRange>*>>
		(std::tuple<const graphics_pipeline_t*, const vk::PipelineLayout, const std::vector<vk::PushConstantRange>*> aPipelineLayout, std::vector<descriptor_set> aDescriptorSets)
	{
		bind_descriptors(vk::PipelineBindPoint::eGraphics, std::get<const graphics_pipeline_t*>(aPipelineLayout)->layout_handle(), std::move(aDescriptorSets));
	}

}
