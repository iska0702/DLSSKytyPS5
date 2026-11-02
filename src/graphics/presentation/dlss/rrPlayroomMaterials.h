#pragma once
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/dlss/dlssRayReconstruction.h"
#include <memory>

namespace Libs::Graphics {
struct GraphicContext;

struct RRPlayroomMaterialInputs {
    RRTexture base_color, metal_roughness, view_normal, depth;
    // Exact 496-byte view block used by the matching lighting submission.
    std::array<float,124> camera {};
};

// Native GPU material conversion. This does not certify the noisy lighting,
// reflection guide, denoiser boundary, or coverage for RR evaluation.
class RRPlayroomMaterials {
public:
    explicit RRPlayroomMaterials(GraphicContext& graphics);
    ~RRPlayroomMaterials();
    struct Frame;
    // Keep Frame and all input resources alive until the recorded work completes.
    // Input images must already be shader-readable and producer writes visible.
    std::shared_ptr<Frame> Record(vk::CommandBuffer command, const RRPlayroomMaterialInputs& inputs);
    static std::string Validate(const RRPlayroomMaterialInputs& inputs);
private:
    struct Pipeline;
    GraphicContext& m_graphics;
    std::shared_ptr<Pipeline> m_pipeline;
};
struct RRPlayroomMaterials::Frame {
    ~Frame(); // GPU completion is the caller's responsibility.
    const RRTexture& Diffuse() const;
    const RRTexture& Specular() const;
    const RRTexture& NormalRoughness() const;
    void RecordReadback(vk::CommandBuffer command);
    void Report(uint64_t guest_frame) const; // only after GPU completion
private:
    friend class RRPlayroomMaterials;
    struct Storage;
    std::unique_ptr<Storage> storage;
};
}
