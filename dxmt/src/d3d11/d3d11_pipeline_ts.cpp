/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "Metal.hpp"
#include "airconv_public.h"
#include "d3d11_device.hpp"
#include "d3d11_pipeline.hpp"
#include "d3d11_shader.hpp"
#include "log/log.hpp"
#include "thread.hpp"

namespace dxmt {

static dxmt::mutex ts_global_mutex;

class MTLCompiledTessellationPipelineImpl: public MTLCompiledTessellationMeshPipeline {
public:
  MTLCompiledTessellationPipelineImpl(MTLD3D11Device *pDevice,
                                  const MTL_GRAPHICS_PIPELINE_DESC *pDesc)
      : num_rtvs(pDesc->NumColorAttachments),
        depth_stencil_format(pDesc->DepthStencilFormat), device_(pDevice),
        pBlendState(pDesc->BlendState),
        RasterizationEnabled(pDesc->RasterizationEnabled),
        SampleCount(pDesc->SampleCount) {
    // AnyOS: a geometry shader behind tessellation -- this pipeline only
    // streams the domain shader's vertices and primitives to buffers (no
    // rasterization); MTLCompiledGeometryPipelineImpl draws them.
    bool stream_to_geometry = pDesc->GeometryShader && pDesc->GSPassthrough == ~0u;
    if (stream_to_geometry)
      RasterizationEnabled = false;
    // AnyOS: domain shader stream output -- this pipeline streams the domain
    // shader's output with a record per workload (no rasterization); the
    // stream-output kernels write it to the SO buffers in patch order.
    bool stream_output = !stream_to_geometry && pDesc->SOLayout;
    if (stream_output)
      RasterizationEnabled = false;
    if (pDesc->RasterizationEnabled && !pDesc->PixelShader)
      num_rtvs = 0;
    uint32_t unorm_output_reg_mask = 0;
    uint32_t uint_output_reg_mask = 0;
    uint32_t sint_output_reg_mask = 0;
    for (unsigned i = 0; i < num_rtvs; i++) {
      rtv_formats[i] = pDesc->ColorAttachmentFormats[i];
      unorm_output_reg_mask |= (uint32_t(IsUnorm8RenderTargetFormat(
                                    pDesc->ColorAttachmentFormats[i]))
                                << i);
      uint_output_reg_mask |=
          (uint32_t(IsUintColorFormat(pDesc->ColorAttachmentFormats[i])) << i);
      sint_output_reg_mask |=
          (uint32_t(IsSintColorFormat(pDesc->ColorAttachmentFormats[i])) << i);
    }

    hull_reflection = pDesc->HullShader->reflection();
    auto &domain_reflection = pDesc->DomainShader->reflection();
    uint32_t max_potential_factor = domain_reflection.PostTessellator.MaxPotentialTessFactor;

    // AnyOS: no Apple9-only clamp to 8.  The mesh-size bound computed from
    // the domain shader's outputs (<= 32 KiB per mesh) already holds on
    // Apple7/8 (M1/M2), and a factor-8 cap visibly flattened every
    // displacement-mapped surface asking for more.
    if ((float)max_potential_factor < hull_reflection.Tessellator.MaxFactor) {
      WARN("maxtessfactor(", hull_reflection.Tessellator.MaxFactor,
           ") is too large for a mesh pipeline. Clamping to ",
           max_potential_factor);
    }

    VertexHullShader =
        pDesc->HullShader->get_shader(ShaderVariantTessellationVertexHull{
            pDesc->InputLayout,
            pDesc->VertexShader, pDesc->IndexBufferFormat,
            max_potential_factor, stream_to_geometry || stream_output, pDesc->GSPassthrough});
    DomainShader =
        pDesc->DomainShader->get_shader(ShaderVariantTessellationDomain{
            pDesc->HullShader, pDesc->GSPassthrough,
            max_potential_factor, !RasterizationEnabled, stream_to_geometry || stream_output, stream_output});
    if (pDesc->PixelShader && !stream_to_geometry && !stream_output) {
      PixelShader = pDesc->PixelShader->get_shader(ShaderVariantPixel{
          pDesc->SampleMask, pDesc->BlendState->IsDualSourceBlending(),
          depth_stencil_format == WMTPixelFormatInvalid,
          unorm_output_reg_mask, uint_output_reg_mask,
          sint_output_reg_mask});
      ps_valid_render_targets = pDesc->PixelShader->reflection().PSValidRenderTargets;
      ps_uint_output_registers = pDesc->PixelShader->reflection().PSUintOutputRegisterMask;
      ps_sint_output_registers = pDesc->PixelShader->reflection().PSSintOutputRegisterMask;
    } else {
      PixelShader = nullptr;
      ps_valid_render_targets = 0;
      ps_uint_output_registers = 0;
      ps_sint_output_registers = 0;
    }
  }

  void GetPipeline(MTL_COMPILED_TESSELLATION_MESH_PIPELINE *pPipeline) final {
    ready_.wait(false, std::memory_order_acquire);
    *pPipeline = {state_rasterization_, hull_reflection.NumOutputElement,
                  hull_reflection.ThreadsPerPatch};
  }

  ThreadpoolWork *RunThreadpoolWork() {

    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER vhs, ds, ps;

    if (!VertexHullShader->GetShader(&vhs)) {
      return VertexHullShader;
    }
    if (!vhs.Function) {
      ERR("Failed to create tess-mesh PSO: Invalid vertex-hull shader.");
      return this;
    }
    if (!DomainShader->GetShader(&ds)) {
      return DomainShader;
    }
    if (!ds.Function) {
      ERR("Failed to create tess-mesh PSO: Invalid domain shader.");
      return this;
    }
    if (PixelShader) {
      if (!PixelShader->GetShader(&ps)) {
        return PixelShader;
      }
      if (!ps.Function) {
        ERR("Failed to create mesh PSO: Invalid pixel shader.");
        return this;
      }
    }

    WMTMeshRenderPipelineInfo info;
    WMT::InitializeMeshRenderPipelineInfo(info);

    info.object_function = vhs.Function;
    info.mesh_function = ds.Function;
    info.payload_memory_length = 0;

    // HACK for AMDGPU: always reserve all payload capability
    if (!device_->GetMTLDevice().supportsFamily(WMTGPUFamilyApple7))
      info.payload_memory_length = 16384;

    info.immutable_object_buffers =
        (1 << 16) | (1 << 21) | (1 << 27) | (1 << 28) | (1 << 29) | (1 << 30);
    info.immutable_mesh_buffers = (1 << 29) | (1 << 30);
    info.immutable_fragment_buffers = (1 << 29) | (1 << 30);

    if (PixelShader) {
      info.fragment_function = ps.Function;
    }
    info.rasterization_enabled = RasterizationEnabled;

    for (unsigned i = 0; i < num_rtvs; i++) {
      if (rtv_formats[i] == WMTPixelFormatInvalid)
        continue;
      info.colors[i].pixel_format = rtv_formats[i];
    }

    if (depth_stencil_format != WMTPixelFormatInvalid) {
      info.depth_pixel_format = depth_stencil_format;
    }
    if (DepthStencilPlanarFlags(depth_stencil_format) & 2) {
      info.stencil_pixel_format = depth_stencil_format;
    }

    if (pBlendState) {
      pBlendState->SetupMetalPipelineDescriptor(
          (WMTRenderPipelineBlendInfo *)&info, num_rtvs, ps_valid_render_targets);
    }

    info.raster_sample_count = SampleCount;
    // so far it's a fixed constant
    info.mesh_tgsize_is_multiple_of_sgwidth = true;
    // total threads is 32
    // FIXME: might be different on AMD GPU, if it's ever supported
    info.object_tgsize_is_multiple_of_sgwidth = true;

    {
      std::lock_guard<dxmt::mutex> lock(ts_global_mutex);
      state_rasterization_ =
          device_->GetMTLDevice().newRenderPipelineState(info, err);
    }
    if (state_rasterization_ == nullptr) {
      ERR("Failed to create tessellation raster PSO: ",
          err.description().getUTF8String());
      /* Same undefined-behavior escape hatch as the ordinary graphics
       * PSO path: drop color writes a component-type mismatch can
       * reach and retry; if the failure isn't attributable to a
       * mismatch, fall back to dropping every integer-attachment
       * write. */
      bool retried = false;
      if (DropIntegerColorWrites(info, num_rtvs, ps_uint_output_registers, ps_sint_output_registers, false)) {
        retried = true;
        std::lock_guard<dxmt::mutex> lock(ts_global_mutex);
        state_rasterization_ =
            device_->GetMTLDevice().newRenderPipelineState(info, err);
        if (state_rasterization_ != nullptr)
          WARN("Created tessellation raster PSO with mismatched integer "
               "color attachment write(s) dropped");
      }
      if (state_rasterization_ == nullptr &&
          DropIntegerColorWrites(info, num_rtvs, 0, 0, true)) {
        retried = true;
        std::lock_guard<dxmt::mutex> lock(ts_global_mutex);
        state_rasterization_ =
            device_->GetMTLDevice().newRenderPipelineState(info, err);
        if (state_rasterization_ != nullptr)
          WARN("Created tessellation raster PSO with writes to integer "
               "color attachment(s) dropped");
      }
      if (state_rasterization_ == nullptr && retried)
        ERR("Tessellation PSO retry without integer color writes also "
            "failed: ", err.description().getUTF8String());
    }

    return this;
  }

  bool GetIsDone() { return ready_; }

  void SetIsDone(bool state) {
    ready_.store(state);
    ready_.notify_all();
  }

private:
  UINT num_rtvs;
  UINT ps_valid_render_targets;
  UINT ps_uint_output_registers;
  UINT ps_sint_output_registers;
  WMTPixelFormat rtv_formats[8];
  WMTPixelFormat depth_stencil_format;
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  IMTLD3D11BlendState *pBlendState;
  WMT::Reference<WMT::RenderPipelineState> state_rasterization_;
  bool RasterizationEnabled;
  UINT SampleCount;

  MTL_SHADER_REFLECTION hull_reflection;

  CompiledShader *VertexHullShader;
  CompiledShader *PixelShader;
  CompiledShader *DomainShader;
};

std::unique_ptr<MTLCompiledTessellationMeshPipeline>
CreateTessellationMeshPipeline(MTLD3D11Device *pDevice,
                               MTL_GRAPHICS_PIPELINE_DESC *pDesc) {
  return std::make_unique<MTLCompiledTessellationPipelineImpl>(pDevice, pDesc);
}

} // namespace dxmt