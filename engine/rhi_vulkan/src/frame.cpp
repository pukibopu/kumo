#include "internal.h"

namespace kumo::rhi::vulkan {

namespace {

struct BarrierMasks {
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

BarrierMasks masksFor(ImageLayoutState state) {
    switch (state) {
    case ImageLayoutState::Undefined:
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0};
    case ImageLayoutState::ColorAttachment:
        return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    case ImageLayoutState::DepthAttachment:
        return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case ImageLayoutState::ShaderReadOnly:
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT};
    case ImageLayoutState::TransferSrc:
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT};
    case ImageLayoutState::TransferDst:
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case ImageLayoutState::General:
        return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    case ImageLayoutState::PresentSrc:
        return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                0};
    }
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0};
}

// Attachment-write scope for a same-layout write-after-write barrier on a
// persistent attachment reused across frames (no layout change to react to).
BarrierMasks writeMasksFor(ImageLayoutState state) {
    if (state == ImageLayoutState::DepthAttachment) {
        return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    }
    return masksFor(state);
}

VkImageMemoryBarrier2 makeBarrier(VkImage image, VkImageAspectFlags aspect, const BarrierMasks& src,
                                  const BarrierMasks& dst) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src.stage;
    barrier.srcAccessMask = src.access;
    barrier.dstStageMask = dst.stage;
    barrier.dstAccessMask = dst.access;
    barrier.oldLayout = src.layout;
    barrier.newLayout = dst.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    // Layouts are tracked per-image, so barriers cover all mips/layers together.
    // Compute prefilter passes write mips sequentially, keeping this coherent.
    barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    return barrier;
}

void recordBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                   LayoutTransition transition) {
    const VkImageMemoryBarrier2 barrier =
        makeBarrier(image, aspect, masksFor(transition.oldLayout), masksFor(transition.newLayout));
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

class VulkanRenderPassEncoder final : public RenderPassEncoder {
public:
    void begin(VkCommandBuffer cmd, LayoutTracker& tracker, const RenderPassDesc& desc) {
        cmd_ = cmd;
        open_ = true;

        KUMO_ASSERT(desc.colorAttachments.size() <= kMaxColorAttachments);
        std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> colorInfos{};
        // Each color attachment may add its own barrier plus one for a resolve
        // target; the depth attachment adds one more.
        std::array<VkImageMemoryBarrier2, kMaxColorAttachments * 2 + 1> barriers{};
        std::uint32_t colorCount = 0;
        std::uint32_t barrierCount = 0;
        Extent2D renderExtent{};

        for (const RenderPassColorAttachment& attachment : desc.colorAttachments) {
            auto* target = static_cast<VulkanTexture*>(attachment.texture);
            KUMO_ASSERT(target != nullptr);
            renderExtent = target->extent();
            if (colorCount == 0) {
                passSampleCount_ = target->sampleCount();
            }

            const auto id = reinterpret_cast<std::uint64_t>(target->image());
            if (auto transition = tracker.request(id, ImageLayoutState::ColorAttachment)) {
                barriers[barrierCount++] =
                    makeBarrier(target->image(), VK_IMAGE_ASPECT_COLOR_BIT,
                                masksFor(transition->oldLayout), masksFor(transition->newLayout));
            } else {
                // No layout change, but a persistent attachment reused next frame
                // still needs a write-after-write barrier.
                const BarrierMasks waw = writeMasksFor(ImageLayoutState::ColorAttachment);
                barriers[barrierCount++] =
                    makeBarrier(target->image(), VK_IMAGE_ASPECT_COLOR_BIT, waw, waw);
            }

            VkRenderingAttachmentInfo& info = colorInfos[colorCount++];
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            info.imageView = target->view();
            info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            info.loadOp = toVk(attachment.loadOp);
            info.storeOp = toVk(attachment.storeOp);
            info.clearValue.color = {{attachment.clearColor.r, attachment.clearColor.g,
                                      attachment.clearColor.b, attachment.clearColor.a}};

            if (attachment.resolveTarget != nullptr) {
                auto* resolve = static_cast<VulkanTexture*>(attachment.resolveTarget);
                const auto resolveId = reinterpret_cast<std::uint64_t>(resolve->image());
                if (auto transition =
                        tracker.request(resolveId, ImageLayoutState::ColorAttachment)) {
                    barriers[barrierCount++] = makeBarrier(
                        resolve->image(), VK_IMAGE_ASPECT_COLOR_BIT,
                        masksFor(transition->oldLayout), masksFor(transition->newLayout));
                } else {
                    const BarrierMasks waw = writeMasksFor(ImageLayoutState::ColorAttachment);
                    barriers[barrierCount++] =
                        makeBarrier(resolve->image(), VK_IMAGE_ASPECT_COLOR_BIT, waw, waw);
                }
                info.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                info.resolveImageView = resolve->view();
                info.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }

        VkRenderingAttachmentInfo depthInfo{};
        bool hasDepth = false;
        if (desc.depthAttachment.texture != nullptr) {
            auto* target = static_cast<VulkanTexture*>(desc.depthAttachment.texture);
            renderExtent = target->extent();
            if (colorCount == 0) {
                passSampleCount_ = target->sampleCount();
            }
            const auto id = reinterpret_cast<std::uint64_t>(target->image());
            if (auto transition = tracker.request(id, ImageLayoutState::DepthAttachment)) {
                barriers[barrierCount++] =
                    makeBarrier(target->image(), VK_IMAGE_ASPECT_DEPTH_BIT,
                                masksFor(transition->oldLayout), masksFor(transition->newLayout));
            } else {
                const BarrierMasks waw = writeMasksFor(ImageLayoutState::DepthAttachment);
                barriers[barrierCount++] =
                    makeBarrier(target->image(), VK_IMAGE_ASPECT_DEPTH_BIT, waw, waw);
            }
            depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthInfo.imageView = target->view();
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthInfo.loadOp = toVk(desc.depthAttachment.loadOp);
            depthInfo.storeOp = toVk(desc.depthAttachment.storeOp);
            depthInfo.clearValue.depthStencil = {desc.depthAttachment.clearDepth, 0};
            hasDepth = true;
        }

        if (barrierCount > 0) {
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = barrierCount;
            dependency.pImageMemoryBarriers = barriers.data();
            vkCmdPipelineBarrier2(cmd_, &dependency);
        }

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {renderExtent.width, renderExtent.height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = colorCount;
        rendering.pColorAttachments = colorInfos.data();
        rendering.pDepthAttachment = hasDepth ? &depthInfo : nullptr;
        vkCmdBeginRendering(cmd_, &rendering);

        // Negative-height viewport flips Y so the same GLSL clip space renders
        // identically to Metal.
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = static_cast<float>(renderExtent.height);
        viewport.width = static_cast<float>(renderExtent.width);
        viewport.height = -static_cast<float>(renderExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd_, 0, 1, &viewport);

        VkRect2D scissor{{0, 0}, {renderExtent.width, renderExtent.height}};
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
    }

    void setPipeline(RenderPipeline& pipeline) override {
        auto& vkPipeline = static_cast<VulkanRenderPipeline&>(pipeline);
        KUMO_ASSERT(vkPipeline.sampleCount() == passSampleCount_);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeline.handle());
        currentLayout_ = vkPipeline.layout();
    }

    void setBindGroup(std::uint32_t set, BindGroup& group) override {
        VkDescriptorSet descriptorSet = static_cast<VulkanBindGroup&>(group).handle();
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, currentLayout_, set, 1,
                                &descriptorSet, 0, nullptr);
    }

    void setVertexBuffer(std::uint32_t slot, Buffer& buffer, std::uint64_t offset) override {
        VkBuffer handle = static_cast<VulkanBuffer&>(buffer).handle();
        VkDeviceSize vkOffset = offset;
        vkCmdBindVertexBuffers(cmd_, slot, 1, &handle, &vkOffset);
    }

    void setIndexBuffer(Buffer& buffer, IndexFormat format, std::uint64_t offset) override {
        vkCmdBindIndexBuffer(cmd_, static_cast<VulkanBuffer&>(buffer).handle(), offset,
                             format == IndexFormat::Uint16 ? VK_INDEX_TYPE_UINT16
                                                           : VK_INDEX_TYPE_UINT32);
    }

    void setPushConstants(ShaderStage stages, const void* data, std::uint32_t size) override {
        vkCmdPushConstants(cmd_, currentLayout_, toVkStages(stages), 0, size, data);
    }

    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount,
              std::uint32_t firstVertex) override {
        vkCmdDraw(cmd_, vertexCount, instanceCount, firstVertex, 0);
    }

    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount,
                     std::uint32_t firstIndex) override {
        vkCmdDrawIndexed(cmd_, indexCount, instanceCount, firstIndex, 0, 0);
    }

    void end() override {
        if (cmd_ != VK_NULL_HANDLE && open_) {
            vkCmdEndRendering(cmd_);
        }
        open_ = false;
    }

    void* nativeEncoderHandle() override { return cmd_; }
    void* nativePassDescriptorHandle() override { return nullptr; }

    void reset() {
        cmd_ = VK_NULL_HANDLE;
        open_ = false;
    }
    bool open() const { return open_; }

private:
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkPipelineLayout currentLayout_ = VK_NULL_HANDLE;
    std::uint32_t passSampleCount_ = 1;
    bool open_ = false;
};

class VulkanComputePassEncoder final : public ComputePassEncoder {
public:
    void begin(VkCommandBuffer cmd, LayoutTracker& tracker) {
        cmd_ = cmd;
        tracker_ = &tracker;
        open_ = true;
        dispatched_ = false;
        touched_.clear();
    }

    void setPipeline(ComputePipeline& pipeline) override {
        auto& vkPipeline = static_cast<VulkanComputePipeline&>(pipeline);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, vkPipeline.handle());
        currentLayout_ = vkPipeline.layout();
    }

    void setBindGroup(std::uint32_t set, BindGroup& group) override {
        auto& vkGroup = static_cast<VulkanBindGroup&>(group);
        for (VkImage image : vkGroup.storageImages()) {
            const auto id = reinterpret_cast<std::uint64_t>(image);
            if (auto transition = tracker_->request(id, ImageLayoutState::General)) {
                recordBarrier(cmd_, image, VK_IMAGE_ASPECT_COLOR_BIT, *transition);
            }
            if (std::find(touched_.begin(), touched_.end(), image) == touched_.end()) {
                touched_.push_back(image);
            }
        }
        VkDescriptorSet descriptorSet = vkGroup.handle();
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, currentLayout_, set, 1,
                                &descriptorSet, 0, nullptr);
    }

    void setPushConstants(const void* data, std::uint32_t size) override {
        vkCmdPushConstants(cmd_, currentLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, data);
    }

    void dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) override {
        if (dispatched_) {
            // Coarse inter-dispatch barrier, per the implicit sync model: successive
            // dispatches on the same storage image have no auto RAW/WAW sync here.
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd_, &dependency);
        }
        vkCmdDispatch(cmd_, groupsX, groupsY, groupsZ);
        dispatched_ = true;
    }

    void end() override {
        if (cmd_ != VK_NULL_HANDLE && open_) {
            // Storage writes are complete; make the images samplable by later passes.
            for (VkImage image : touched_) {
                const auto id = reinterpret_cast<std::uint64_t>(image);
                if (auto transition = tracker_->request(id, ImageLayoutState::ShaderReadOnly)) {
                    recordBarrier(cmd_, image, VK_IMAGE_ASPECT_COLOR_BIT, *transition);
                }
            }
        }
        open_ = false;
    }

    void reset() {
        cmd_ = VK_NULL_HANDLE;
        open_ = false;
        dispatched_ = false;
        touched_.clear();
    }
    bool open() const { return open_; }

private:
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    LayoutTracker* tracker_ = nullptr;
    VkPipelineLayout currentLayout_ = VK_NULL_HANDLE;
    std::vector<VkImage> touched_;
    bool dispatched_ = false;
    bool open_ = false;
};

class VulkanCommandEncoder final : public CommandEncoder {
public:
    VulkanCommandEncoder(VkPhysicalDevice physical, VkQueue queue, FrameSlot* slot,
                         LayoutTracker* tracker)
        : physical_(physical), queue_(queue), slot_(slot), tracker_(tracker) {}

    ~VulkanCommandEncoder() override {
        if (submitted_) {
            return;
        }
        // Drain the frame slot so its fence is signaled for the next reuse.
        if (passEncoder_.open()) {
            passEncoder_.end();
        }
        if (computePassEncoder_.open()) {
            computePassEncoder_.end();
        }
        vkEndCommandBuffer(slot_->cmd);

        VkCommandBufferSubmitInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = slot_->cmd;

        VkSemaphoreSubmitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitInfo.semaphore = slot_->imageAvailable;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        if (slot_->pendingAcquire) {
            // acquireNextTexture signaled this slot's semaphore but nothing
            // consumed it; the drain must wait so the signal is not left dangling.
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &waitInfo;
            slot_->pendingAcquire = false;
        }
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        vkQueueSubmit2(queue_, 1, &submit, slot_->inFlight);
    }

    RenderPassEncoder& beginRenderPass(const RenderPassDesc& desc) override {
        passEncoder_.reset();
        passEncoder_.begin(slot_->cmd, *tracker_, desc);
        return passEncoder_;
    }

    ComputePassEncoder& beginComputePass() override {
        computePassEncoder_.reset();
        computePassEncoder_.begin(slot_->cmd, *tracker_);
        return computePassEncoder_;
    }

    void generateMipmaps(Texture& texture) override {
        KUMO_ASSERT(!passEncoder_.open() && !computePassEncoder_.open());
        auto& tex = static_cast<VulkanTexture&>(texture);
        const std::uint32_t mipCount = tex.mipLevels();
        if (mipCount <= 1) {
            return;
        }
        // Linear-filter blits need a filterable format; HDR mip chains should use
        // RGBA16Float (RGBA32Float is not filterable on most GPUs).
        VkFormatProperties formatProps{};
        vkGetPhysicalDeviceFormatProperties(physical_, toVk(tex.format()), &formatProps);
        if ((formatProps.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
            logError("generateMipmaps: format is not linear-filterable "
                     "(HDR mip chains should use RGBA16Float)");
            return;
        }
        const std::uint32_t layerCount = tex.arrayLayers();
        const VkImage image = tex.image();
        VkCommandBuffer cmd = slot_->cmd;
        const auto id = reinterpret_cast<std::uint64_t>(image);
        const VkImageLayout srcLayout = masksFor(tracker_->current(id)).layout;

        auto emit = [&](VkImageMemoryBarrier2& b) {
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;

        VkImageMemoryBarrier2 toSrc{};
        toSrc.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toSrc.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        toSrc.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toSrc.oldLayout = srcLayout;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.subresourceRange = {aspect, 0, 1, 0, layerCount};
        emit(toSrc);

        std::int32_t mipW = static_cast<std::int32_t>(tex.extent().width);
        std::int32_t mipH = static_cast<std::int32_t>(tex.extent().height);
        for (std::uint32_t i = 1; i < mipCount; ++i) {
            const std::int32_t dstW = std::max(1, mipW / 2);
            const std::int32_t dstH = std::max(1, mipH / 2);

            VkImageMemoryBarrier2 toDst{};
            toDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            toDst.srcAccessMask = 0;
            toDst.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.subresourceRange = {aspect, i, 1, 0, layerCount};
            emit(toDst);

            VkImageBlit2 blit{};
            blit.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
            blit.srcSubresource = {aspect, i - 1, 0, layerCount};
            blit.srcOffsets[1] = {mipW, mipH, 1};
            blit.dstSubresource = {aspect, i, 0, layerCount};
            blit.dstOffsets[1] = {dstW, dstH, 1};
            VkBlitImageInfo2 blitInfo{};
            blitInfo.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
            blitInfo.srcImage = image;
            blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            blitInfo.dstImage = image;
            blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blitInfo.regionCount = 1;
            blitInfo.pRegions = &blit;
            blitInfo.filter = VK_FILTER_LINEAR;
            vkCmdBlitImage2(cmd, &blitInfo);

            VkImageMemoryBarrier2 dstToSrc{};
            dstToSrc.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            dstToSrc.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            dstToSrc.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            dstToSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            dstToSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            dstToSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            dstToSrc.subresourceRange = {aspect, i, 1, 0, layerCount};
            emit(dstToSrc);

            mipW = dstW;
            mipH = dstH;
        }

        VkImageMemoryBarrier2 toRead{};
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toRead.dstStageMask =
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.subresourceRange = {aspect, 0, mipCount, 0, layerCount};
        emit(toRead);
        tracker_->set(id, ImageLayoutState::ShaderReadOnly);
    }

    void finishAndSubmit(Surface* presentTo) override {
        KUMO_ASSERT(!submitted_);
        auto* surface = static_cast<VulkanSurface*>(presentTo);
        const bool present = surface != nullptr && surface->acquired();

        VkSemaphore renderFinished = VK_NULL_HANDLE;
        if (present) {
            // The wait below consumes the acquire semaphore signaled for this slot.
            slot_->pendingAcquire = false;
            VulkanTexture* target = surface->currentTexture();
            const auto id = reinterpret_cast<std::uint64_t>(target->image());
            if (auto transition = tracker_->request(id, ImageLayoutState::PresentSrc)) {
                recordBarrier(slot_->cmd, target->image(), VK_IMAGE_ASPECT_COLOR_BIT, *transition);
            }
            renderFinished = surface->currentRenderFinishedSemaphore();
        }

        vkEndCommandBuffer(slot_->cmd);

        VkCommandBufferSubmitInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = slot_->cmd;

        VkSemaphoreSubmitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitInfo.semaphore = slot_->imageAvailable;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalInfo{};
        signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalInfo.semaphore = renderFinished;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = present ? 1u : 0u;
        submit.pWaitSemaphoreInfos = present ? &waitInfo : nullptr;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = present ? 1u : 0u;
        submit.pSignalSemaphoreInfos = present ? &signalInfo : nullptr;
        vkQueueSubmit2(queue_, 1, &submit, slot_->inFlight);
        submitted_ = true;

        if (present) {
            const std::uint32_t imageIndex = surface->currentImageIndex();
            const VkSwapchainKHR swapchain = surface->swapchain();
            VkPresentInfoKHR presentInfo{};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = &renderFinished;
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = &swapchain;
            presentInfo.pImageIndices = &imageIndex;
            const VkResult result = vkQueuePresentKHR(queue_, &presentInfo);
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
                surface->markNeedsRecreate();
            } else if (result != VK_SUCCESS) {
                logError("vkQueuePresentKHR failed ({})", static_cast<int>(result));
            }
            surface->clearAcquired();
        }

        passEncoder_.reset();
        computePassEncoder_.reset();
    }

    void* nativeCommandBufferHandle() override { return slot_->cmd; }

private:
    VkPhysicalDevice physical_;
    VkQueue queue_;
    FrameSlot* slot_;
    LayoutTracker* tracker_;
    VulkanRenderPassEncoder passEncoder_;
    VulkanComputePassEncoder computePassEncoder_;
    bool submitted_ = false;
};

} // namespace

bool VulkanQueue::init(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                       std::uint32_t family, VmaAllocator allocator, VkCommandPool uploadPool,
                       LayoutTracker* tracker) {
    physical_ = physical;
    device_ = device;
    queue_ = queue;
    allocator_ = allocator;
    uploadPool_ = uploadPool;
    tracker_ = tracker;

    for (FrameSlot& slot : frames_) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = family;
        if (vkCreateCommandPool(device_, &poolInfo, nullptr, &slot.pool) != VK_SUCCESS) {
            logError("vkCreateCommandPool failed");
            return false;
        }
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = slot.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &allocInfo, &slot.cmd) != VK_SUCCESS) {
            logError("vkAllocateCommandBuffers failed");
            return false;
        }
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device_, &fenceInfo, nullptr, &slot.inFlight) != VK_SUCCESS) {
            logError("vkCreateFence failed");
            return false;
        }
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &slot.imageAvailable) !=
            VK_SUCCESS) {
            logError("vkCreateSemaphore failed");
            return false;
        }
    }
    return true;
}

void VulkanQueue::shutdown() {
    for (FrameSlot& slot : frames_) {
        if (slot.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, slot.imageAvailable, nullptr);
        }
        if (slot.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(device_, slot.inFlight, nullptr);
        }
        if (slot.pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, slot.pool, nullptr);
        }
        slot = {};
    }
}

Ptr<CommandEncoder> VulkanQueue::createCommandEncoder() {
    FrameSlot& slot = frames_[frameIndex_];
    vkWaitForFences(device_, 1, &slot.inFlight, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &slot.inFlight);
    vkResetCommandPool(device_, slot.pool, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.cmd, &beginInfo);

    activeSlot_ = &slot;
    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
    return std::make_shared<VulkanCommandEncoder>(physical_, queue_, &slot, tracker_);
}

void VulkanQueue::waitIdle() {
    vkDeviceWaitIdle(device_);
}

void VulkanQueue::writeBuffer(Buffer& buffer, std::uint64_t offset, const void* data,
                              std::uint64_t size) {
    auto& vkBuffer = static_cast<VulkanBuffer&>(buffer);
    KUMO_ASSERT(offset + size <= vkBuffer.size());
    KUMO_ASSERT(vkBuffer.mapped() != nullptr);
    std::memcpy(static_cast<char*>(vkBuffer.mapped()) + offset, data, size);
}

void VulkanQueue::writeTexture(Texture& texture, const void* data, std::uint64_t bytesPerRow,
                               Extent2D size) {
    auto& vkTexture = static_cast<VulkanTexture&>(texture);
    const VkDeviceSize dataSize = static_cast<VkDeviceSize>(bytesPerRow) * size.height;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = dataSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo allocCreate{};
    allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreate.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};
    if (vmaCreateBuffer(allocator_, &bufferInfo, &allocCreate, &staging, &stagingAlloc,
                        &stagingInfo) != VK_SUCCESS) {
        logError("writeTexture staging allocation failed");
        return;
    }
    std::memcpy(stagingInfo.pMappedData, data, dataSize);

    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = uploadPool_;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cmdAlloc, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    const auto id = reinterpret_cast<std::uint64_t>(vkTexture.image());
    if (auto transition = tracker_->request(id, ImageLayoutState::TransferDst)) {
        recordBarrier(cmd, vkTexture.image(), VK_IMAGE_ASPECT_COLOR_BIT, *transition);
    }

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource = {static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
    copy.imageExtent = {size.width, size.height, 1};
    vkCmdCopyBufferToImage(cmd, staging, vkTexture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);

    if (auto transition = tracker_->request(id, ImageLayoutState::ShaderReadOnly)) {
        recordBarrier(cmd, vkTexture.image(), VK_IMAGE_ASPECT_COLOR_BIT, *transition);
    }

    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, uploadPool_, 1, &cmd);
    vmaDestroyBuffer(allocator_, staging, stagingAlloc);
}

VulkanSurface::~VulkanSurface() {
    vkDeviceWaitIdle(device_);
    destroySwapchain();
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
}

void VulkanSurface::configure(const SurfaceConfig& config) {
    vkDeviceWaitIdle(device_);
    format_ = config.format;
    extent_ = config.size;
    recreate();
}

Texture* VulkanSurface::acquireNextTexture() {
    if (needsRecreate_) {
        vkDeviceWaitIdle(device_);
        recreate();
    }
    if (swapchain_ == VK_NULL_HANDLE) {
        return nullptr;
    }
    const VkSemaphore imageAvailable = imageAvailableSemaphore();
    std::uint32_t index = 0;
    const VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable,
                                                  VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        needsRecreate_ = true;
        return nullptr;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        logError("vkAcquireNextImageKHR failed ({})", static_cast<int>(result));
        return nullptr;
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        needsRecreate_ = true;
    }
    currentImageIndex_ = index;
    acquired_ = true;
    markAcquireSignaled();
    texture_.reset(images_[index], imageViews_[index], extent_, format_);
    return &texture_;
}

VkSemaphore VulkanSurface::imageAvailableSemaphore() const {
    return queue_->currentImageAvailableSemaphore();
}

void VulkanSurface::markAcquireSignaled() {
    queue_->markAcquireSignaled();
}

void VulkanSurface::forgetSwapchainImages() {
    if (tracker_ != nullptr) {
        for (VkImage image : images_) {
            tracker_->forget(reinterpret_cast<std::uint64_t>(image));
        }
    }
}

void VulkanSurface::destroySwapchain() {
    forgetSwapchainImages();
    for (VkImageView view : imageViews_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    imageViews_.clear();
    for (VkSemaphore semaphore : renderFinished_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    renderFinished_.clear();
    for (VkSemaphore semaphore : retiredSemaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    retiredSemaphores_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    images_.clear();
}

void VulkanSurface::recreate() {
    if (extent_.width == 0 || extent_.height == 0) {
        destroySwapchain();
        needsRecreate_ = true;
        return;
    }
    const VkSwapchainKHR oldSwapchain = swapchain_;
    std::vector<VkImageView> oldViews = imageViews_;
    std::vector<VkSemaphore> oldRenderFinished = renderFinished_;

    vkb::SwapchainBuilder builder{physical_, device_, surface_,
                                  static_cast<std::uint32_t>(graphicsFamily_),
                                  static_cast<std::uint32_t>(graphicsFamily_)};
    auto swap_ret = builder
                        .set_desired_format(
                            VkSurfaceFormatKHR{toVk(format_), VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                        .set_desired_extent(extent_.width, extent_.height)
                        .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                        .set_old_swapchain(oldSwapchain)
                        .build();
    if (!swap_ret) {
        logError("swapchain build failed: {}", swap_ret.error().message());
        needsRecreate_ = true;
        return;
    }
    vkb::Swapchain vkbSwapchain = swap_ret.value();

    // Semaphores retired by the previous recreate are now idle; destroy them.
    for (VkSemaphore semaphore : retiredSemaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    retiredSemaphores_.clear();
    for (VkImageView view : oldViews) {
        vkDestroyImageView(device_, view, nullptr);
    }
    // The presentation engine may still hold renderFinished after SUBOPTIMAL;
    // waitIdle does not cover it, so retire rather than destroy immediately.
    retiredSemaphores_ = std::move(oldRenderFinished);
    forgetSwapchainImages();
    if (oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
    }

    swapchain_ = vkbSwapchain.swapchain;
    extent_ = {vkbSwapchain.extent.width, vkbSwapchain.extent.height};
    images_ = vkbSwapchain.get_images().value();
    imageViews_ = vkbSwapchain.get_image_views().value();

    renderFinished_.resize(images_.size());
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (VkSemaphore& semaphore : renderFinished_) {
        vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore);
    }
    needsRecreate_ = false;
}

} // namespace kumo::rhi::vulkan
