// DarkHouse — GPU noise reduction node ("denoise").
//
// Implements the algorithm described in denoise.hpp as five compute passes
// recorded into the develop graph's command buffer:
//
//   down0   input -> G_1: loads the full-size input once into shared memory,
//           stabilises + opponent-transforms it there, filters (separable
//           5-tap), and bins Haar HH magnitudes for the noise estimate
//   down    G_k -> G_(k+1) for the remaining levels (shared-memory tiles)
//   sigma   histogram -> sigma -> band thresholds, entirely on the GPU
//   up      R_k = up(R_(k+1)) + shrink(G_k - up(G_(k+1))), coarse to fine
//   final   recomputes stabilised level 0 on the fly, shrinks the finest
//           band and writes linear RGB
//
// Full-resolution memory is touched three times (down0 read, final read,
// final write); the pyramid below it costs ~1/3 more. With both strengths at
// zero the node is a plain image copy.
#pragma once

#include "denoise.hpp"
#include "render_pipeline.hpp"

#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

namespace darkhouse {

class DenoiseNode final : public ComputeNode {
public:
    static constexpr std::string_view kTypeName = "denoise";

    // Loads <shaderDirectory>/denoise_{down0,down,sigma,up,final}.spv.
    DenoiseNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory);
    ~DenoiseNode() override;

    DenoiseNode(const DenoiseNode&) = delete;
    DenoiseNode& operator=(const DenoiseNode&) = delete;

    [[nodiscard]] std::string_view typeName() const noexcept override { return kTypeName; }
    [[nodiscard]] std::uint32_t inputCount() const noexcept override { return 1; }

    void setInputTexture(std::uint32_t slot, const GPUTexture& texture) override;
    [[nodiscard]] const GPUTexture& getOutputTexture() const override { return output_; }
    void executeCompute(VkCommandBuffer commandBuffer) override;
    void updateUniforms(std::span<const std::byte> packedParams) override;

    void setParams(const DenoiseParams& params) noexcept { params_ = sanitize(params); }
    [[nodiscard]] const DenoiseParams& params() const noexcept { return params_; }
    [[nodiscard]] static std::vector<std::byte> pack(const DenoiseParams& params);

    // Noise measured by the last recorded evaluation. Valid once that
    // submission has completed (the develop graph waits for it); sigma is
    // zero before the first evaluation and while the node is bypassed.
    [[nodiscard]] DenoiseStatistics statistics() const noexcept;

private:
    enum Pass : std::size_t { DOWN0, DOWN, SIGMA, UP, FINAL, PASS_COUNT };
    struct Pipeline {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    void createPipelines(const std::filesystem::path& shaderDirectory);
    void allocateResources(std::uint32_t width, std::uint32_t height);
    void releaseResources() noexcept;
    void writeDescriptors();
    void destroy() noexcept;
    void recordCopy(VkCommandBuffer commandBuffer);
    void dispatch(VkCommandBuffer commandBuffer, Pass pass, VkDescriptorSet set, const void* push,
                  std::uint32_t pushSize, std::uint32_t groupsX, std::uint32_t groupsY) const;

    const VulkanContext& context_;
    std::array<Pipeline, PASS_COUNT> pipelines_{};
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    GPUBuffer histogram_;   // 3 * DH_DENOISE_HIST_BINS uint, device local
    GPUBuffer thresholds_;  // DH_DENOISE_PARAMS_FLOATS floats, host visible (sigma readback)

    GPUTexture input_;
    GPUTexture output_;
    std::vector<GPUTexture> gaussian_;       // G_1 .. G_L
    std::vector<GPUTexture> reconstructed_;  // R_1 .. R_(L-1)
    std::uint32_t levels_ = 0;

    VkDescriptorSet down0Set_ = VK_NULL_HANDLE;
    VkDescriptorSet sigmaSet_ = VK_NULL_HANDLE;
    VkDescriptorSet finalSet_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> downSets_;  // [k - 1]: G_k -> G_(k+1), k = 1 .. L-1
    std::vector<VkDescriptorSet> upSets_;    // [k - 1]: R_k, k = 1 .. L-1

    DenoiseParams params_;
    bool measured_ = false;  // the last recorded evaluation ran the full pyramid
};

}  // namespace darkhouse
