#include "air_signature.hpp"
#include "airconv_error.hpp"
#include "dxbc_converter.hpp"
#include "nt/air_builder.hpp"
#include "nt/dxbc_converter_base.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/AtomicOrdering.h"

namespace dxmt::dxbc {

struct TessMeshWorkload {
  short inner0[2];
  short inner1[2];
  short outer0[2];
  short outer1[2];
  int inner_factor;
  int outer_factor;
  char inner_factor_i;
  char outer_factor_i;
  bool has_complement;
  bool padding;
  short inner2[2];
  short inner3[2];
  short outer2[2];
  short outer3[2];
  int inner_factor_23;
  int outer_factor_23;
  char inner_factor_23_i;
  char outer_factor_23_i;
  short patch_index;
};

TessellatorPartitioning
get_partitioning(SM50ShaderInternal *pHullStage) {
  TessellatorPartitioning partitioning;
  switch (pHullStage->tessellation_partition) {
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_POW2:
    partitioning = TessellatorPartitioning::pow2;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD:
    partitioning = TessellatorPartitioning::fractional_odd;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN:
    partitioning = TessellatorPartitioning::fractional_even;
    break;
  default:
    partitioning = TessellatorPartitioning::integer;
    break;
  }
  return partitioning;
}

TessellatorOutputPrimitive
get_output_primitive(SM50ShaderInternal *pHullStage) {
  TessellatorOutputPrimitive primitive;
  switch (pHullStage->tessellator_output_primitive) {
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_LINE:
    primitive = TessellatorOutputPrimitive::line;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW:
    primitive = TessellatorOutputPrimitive::triangle;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW:
    primitive = TessellatorOutputPrimitive::triangle_ccw;
    break;
  default:
    primitive = TessellatorOutputPrimitive::point;
    break;
  }
  return primitive;
}

uint32_t
get_integer_factor(float factor, SM50ShaderInternal *pHullStage) {
  uint32_t integer_factor = 0;
  switch (pHullStage->tessellation_partition) {
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_UNDEFINED:
    return 0;
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_INTEGER: {
    factor = std::clamp<float>(factor, 1.0, 64.0);
    integer_factor = std::ceil(factor);
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_POW2: {
    integer_factor = std::clamp<float>(factor, 1.0, 64.0);
    integer_factor = integer_factor == 1 ? 1 : 1 << (32 - __builtin_clz(integer_factor - 1));
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD: {
    factor = std::clamp<float>(factor, 1.0, 63.0);
    integer_factor = std::ceil(factor);
    integer_factor = integer_factor & 1 ? integer_factor : integer_factor + 1;
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN: {
    factor = std::clamp<float>(factor, 2.0, 64.0);
    integer_factor = std::ceil(factor);
    integer_factor = integer_factor & 1 ? integer_factor + 1 : integer_factor;
    break;
  }
  }
  return integer_factor;
}

uint32_t
get_max_potential_workload_count(uint32_t max_tess_factor, SM50ShaderInternal *pHullStage) {

  switch (pHullStage->tessellation_domain) {
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_UNDEFINED:
    return 0;
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE:
    return max_tess_factor; // AnyOS: one workload per line
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_TRI:
    return std::ceil((max_tess_factor - 1) / 4.0) * 3 + (max_tess_factor & 1);
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_QUAD:
    // AnyOS: + 1 -- integer inner factors (63, 64) make 64 ring workloads plus
    // the centre workload (gen_workload_quad_impl), one more than the formula.
    return std::ceil((max_tess_factor - 1) / 4.0) * 4 + (max_tess_factor & 1) + 1;
  }
}

size_t
estimate_payload_size(SM50ShaderInternal *pHullStage, float factor, uint32_t patch_per_group) {
  auto max_hs_output_register = pHullStage->max_output_register;
  auto cp_size_per_point = sizeof(uint32_t) * 4 * max_hs_output_register;
  auto cp_size_per_patch = cp_size_per_point * pHullStage->output_control_point_count;
  auto cp_size_per_group = cp_size_per_patch * patch_per_group;
  auto pc_size_per_patch = sizeof(uint32_t) * pHullStage->patch_constant_scalars.size();
  auto pc_size_per_group = pc_size_per_patch * patch_per_group;

  auto factor_int = get_integer_factor(factor, pHullStage);
  uint32_t max_workload_count = get_max_potential_workload_count(factor_int, pHullStage);
  constexpr uint32_t size_workload_info = sizeof(TessMeshWorkload);

  return cp_size_per_group + pc_size_per_group + sizeof(uint32_t) +
         max_workload_count * patch_per_group * size_workload_info +
         (max_workload_count * patch_per_group + 1) * sizeof(uint32_t); // AnyOS: group table
};

size_t estimate_mesh_size(SM50ShaderInternal *pDomainStage, uint32_t max_potential_factor_int) {
  /*
  to calculate mesh size:
  max_vertex_count * size_per_vertex
  = calc_max_vertex_count(tess_factor) * ds_output_register_count * sizeof(vec4)

  NOTE: it assumes 1.all output registers are vec4 2.primitive count <= vertex count
  */
  size_t size_per_vertex = pDomainStage->max_output_register * 16;
  size_t max_edge_point = max_potential_factor_int + 1;
  // AnyOS: a workload pair may hold 2 * factor + 10 vertices (an inner edge
  // of 3 with two outer edges of `factor`, gen_workload_triangle_impl's
  // r + 1 == u case), one to five more than the edge-point formula.
  size_t vertices = std::max<size_t>((max_edge_point + 2 - (max_edge_point & 1)) * 2 + 1, 2 * max_potential_factor_int + 10);
  return vertices * size_per_vertex;
}

std::pair<float, uint32_t>
get_final_maxtessfactor(SM50ShaderInternal *pHullStage, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs) {
  /*
  the factor received by domain (mesh) shader will be strictly less or equal to returned value
  so we can reserve enough spaces for output mesh
  */
  float maxtessfactor = pHullStage->max_tesselation_factor;
  uint32_t maxtessfactor_int = get_integer_factor(maxtessfactor, pHullStage);
  SM50_SHADER_PSO_TESSELLATOR_DATA *pso_tess = nullptr;
  if (args_get_data<SM50_SHADER_PSO_TESSELLATOR, SM50_SHADER_PSO_TESSELLATOR_DATA>(pArgs, &pso_tess) &&
      maxtessfactor_int > pso_tess->max_potential_tess_factor) {
    maxtessfactor = pso_tess->max_potential_tess_factor;
    do {
      maxtessfactor_int = get_integer_factor(maxtessfactor, pHullStage);
      if (maxtessfactor_int <= pso_tess->max_potential_tess_factor)
        return {maxtessfactor, maxtessfactor_int};
      maxtessfactor = maxtessfactor - 1.0f;
    } while (maxtessfactor > 1.0f);
    auto minimal_factor = get_integer_factor(1.0f, pHullStage);
    return {minimal_factor, minimal_factor};
  }
  return {maxtessfactor, maxtessfactor_int};
}

// AnyOS: mesh workload groups.  Whether the object stage packs workloads into
// groups (one mesh threadgroup per group) and the mesh output a group may
// fill.  Both stages derive it from the same inputs: the pipeline's potential
// tessellation factor (whose single-workload mesh fits the mesh-output budget
// by construction, see MaxPotentialTessFactor).  Rasterized triangle output of the tri and
// quad domains only; isolines, points, lines and streamed (geometry / stream
// output) pipelines keep one workload per mesh threadgroup.  Group bounds are
// encoded in 10 bits each, so the grid is limited to 1023 workloads.  The
// per-primitive render-target / viewport array index of a pass-through
// geometry shader is written per vertex index (primitive_id below), which
// only lines up with one workload per threadgroup, so those pipelines keep
// single workloads too.
struct MeshGroupPlan {
  bool enabled;
  uint32_t max_vertices;
  uint32_t max_primitives;
};

MeshGroupPlan
get_mesh_group_plan(
    SM50ShaderInternal *pHullStage, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, bool stream,
    uint32_t max_workloads_per_grid
) {
  using namespace microsoft;
  auto [final_maxtessfactor, factor_int] = get_final_maxtessfactor(pHullStage, pArgs);
  uint32_t potential = factor_int;
  SM50_SHADER_PSO_TESSELLATOR_DATA *pso_tess = nullptr;
  if (args_get_data<SM50_SHADER_PSO_TESSELLATOR, SM50_SHADER_PSO_TESSELLATOR_DATA>(pArgs, &pso_tess))
    potential = std::max<uint32_t>(potential, pso_tess->max_potential_tess_factor);
  // The mesh output the single-workload pipeline declares for this factor
  // (vertices == primitives; a level-64 quad ring pair needs the full 133,
  // clamping to the helper's 130 broke quad-level-64 in d3d11-tess-probe).
  // Capacity: the largest single workload pair (2 * factor + 10 vertices,
  // 2 * factor + 6 primitives: an inner edge of 3 with two outer edges of
  // `factor`), never below the edge-point formula the single-workload mesh
  // declares; estimate_mesh_size budgets the same count.
  uint32_t edge_point = potential + 1;
  uint32_t formula = (edge_point + 2 - (edge_point & 1)) * 2 + 1;
  uint32_t vertices = std::max<uint32_t>(formula, 2 * potential + 10);
  uint32_t primitives = std::max<uint32_t>(formula, 2 * potential + 6);
  SM50_SHADER_GS_PASS_THROUGH_DATA *gs_passthrough = nullptr;
  bool primitive_attributes =
      args_get_data<SM50_SHADER_GS_PASS_THROUGH, SM50_SHADER_GS_PASS_THROUGH_DATA>(pArgs, &gs_passthrough) &&
      (gs_passthrough->Data.RenderTargetArrayIndexReg != 255 || gs_passthrough->Data.ViewportArrayIndexReg != 255);
  bool enabled = !stream && !primitive_attributes &&
                 pHullStage->tessellation_domain != D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE &&
                 pHullStage->tessellator_output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_POINT &&
                 pHullStage->tessellator_output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_LINE &&
                 max_workloads_per_grid <= 1023;
  return {enabled, vertices, primitives};
}

llvm::Error
convert_dxbc_vertex_hull_shader(
    SM50ShaderInternal *pVertexStage, SM50ShaderInternal *pHullStage, const char *name, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  // NOTE: func_signature of vertex stage only defines some outputs
  // that we don't need them in vertex-hull object shader.
  // And func_signature of hull stage simply has nothing defined yet
  // TODO: should get rid of func_signature in SM50ShaderInternal and
  // always create a new fresh instance + use signature handlers
  auto func_signature = pHullStage->func_signature;
  auto &vertex_shader_info = pVertexStage->shader_info;
  auto &hull_shader_info = pHullStage->shader_info;

  uint32_t max_vs_input_register = pVertexStage->max_input_register;
  uint32_t max_vs_output_register = 0;
  for (auto &out : pVertexStage->output_signature) {
    max_vs_output_register = std::max(max_vs_output_register, out.reg() + 1);
  }
  uint32_t max_hs_output_register = pHullStage->max_output_register;
  uint32_t max_patch_constant_output_register = pHullStage->max_patch_constant_output_register;

  auto [final_maxtessfactor, factor_int] = get_final_maxtessfactor(pHullStage, pArgs);

  // AnyOS: the domain stage streams to a geometry pipeline / stream output
  // (passed to the hull variant as well so both stages agree on mesh groups)
  SM50_SHADER_TESSELLATION_STREAM_DATA *hs_stream_data = nullptr;
  bool hs_stream =
      args_get_data<SM50_SHADER_TESSELLATION_STREAM, SM50_SHADER_TESSELLATION_STREAM_DATA>(pArgs, &hs_stream_data);

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(pArgs, &ia_layout);
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);

  bool is_indexed_draw = ia_layout && ia_layout->index_buffer_format > 0;

  IREffect prologue_vs([](auto) { return std::monostate(); });
  IREffect prologue_hs([](auto) { return std::monostate(); });
  IRValue epilogue_vs([](struct context ctx) -> pvalue {
    // just return null since the function returns void
    // and we handle output registers directly
    return nullptr;
  });
  IRValue epilogue_hs([](struct context ctx) -> pvalue {
    // just return null since the function returns void
    // and we handle output registers directly
    return nullptr;
  });

  io_binding_map resource_map_vs;
  io_binding_map resource_map_hs;
  air::AirType types(context);

  {
    SignatureContext sig_ctx_vs(prologue_vs, epilogue_vs, func_signature, resource_map_vs);
    sig_ctx_vs.ia_layout = ia_layout;
    sig_ctx_vs.skip_vertex_output = true;
    for (auto &p : pVertexStage->signature_handlers) {
      p(sig_ctx_vs);
    }
    SignatureContext sig_ctx_hs(prologue_hs, epilogue_hs, func_signature, resource_map_hs);
    for (auto &p : pHullStage->signature_handlers) {
      p(sig_ctx_hs);
    }
  }

  auto binding_map = rootsig
                         ? setup_binding_rootsig(
                               &vertex_shader_info, func_signature, module, D3D10_SB_VERTEX_SHADER, rootsig->bytecode,
                               rootsig->bytecode_length, SM50_BINDING_INDEX_ROOT_ARGUMENTS + 2, SM50_BINDING_INDEX_STATIC_SAMPLERS + 2
                           )
                         : setup_binding_table2(
                               &vertex_shader_info, func_signature, module, SM50_BINDING_INDEX_CONSTANT_BUFFER2,
                               SM50_BINDING_INDEX_ARGUMENT_TABLE2
                           );
  auto binding_map_hs = rootsig ? setup_binding_rootsig(
                                      &hull_shader_info, func_signature, module, D3D11_SB_HULL_SHADER,
                                      rootsig->bytecode, rootsig->bytecode_length
                                  )
                                : setup_binding_table2(&hull_shader_info, func_signature, module);

  uint32_t threads_per_patch = next_pow2(pHullStage->hull_maximum_threads_per_patch);
  uint32_t patch_per_group = next_pow2(32 / threads_per_patch);

  auto hs_output_per_point_type = llvm::ArrayType::get(types._int4, max_hs_output_register);
  auto hs_output_per_patch_type =
      llvm::ArrayType::get(hs_output_per_point_type, pHullStage->output_control_point_count);
  auto hs_output_per_group_type = llvm::ArrayType::get(hs_output_per_patch_type, patch_per_group);
  auto hs_output_per_point_type_float = llvm::ArrayType::get(types._float4, max_hs_output_register);
  auto hs_output_per_patch_type_float =
      llvm::ArrayType::get(hs_output_per_point_type_float, pHullStage->output_control_point_count);

  auto hs_pcout_per_patch_type = llvm::ArrayType::get(types._int4, max_patch_constant_output_register);
  auto hs_pcout_per_group_type = llvm::ArrayType::get(hs_pcout_per_patch_type, patch_per_group);
  auto hs_pcout_per_patch_type_float = llvm::ArrayType::get(types._float4, max_patch_constant_output_register);

  auto hs_pcout_per_patch_scaler_type = llvm::ArrayType::get(types._int, pHullStage->patch_constant_scalars.size());
  auto hs_pcout_per_group_scaler_type = llvm::ArrayType::get(hs_pcout_per_patch_scaler_type, patch_per_group);

  uint32_t max_workload_count = get_max_potential_workload_count(factor_int, pHullStage);

  // AnyOS: every patch of the threadgroup adds its workloads to one mesh grid,
  // so the grid can hold patch_per_group times one patch's maximum. Declaring
  // one patch's maximum let the object stage dispatch past its declared limit
  // (undefined: patches dropped or drawn from stale payload, flickering per
  // frame -- Unigine Heaven tessellation, 2026-10-02).
  func_signature.UseMaxMeshWorkgroupSize(max_workload_count * patch_per_group);
  auto group_plan = get_mesh_group_plan(pHullStage, pArgs, hs_stream, max_workload_count * patch_per_group);

  constexpr uint32_t size_workload_info = sizeof(TessMeshWorkload);

  auto payload_struct_type = llvm::StructType::create(
      context,
      {hs_output_per_group_type, hs_pcout_per_group_scaler_type, types._int,
       llvm::ArrayType::get(types._int, max_workload_count * patch_per_group * size_workload_info / 4),
       llvm::ArrayType::get(types._int, max_workload_count * patch_per_group + 1)}, // AnyOS: group table
      "payload"
  );
  uint32_t payload_struct_size = module.getDataLayout().getTypeAllocSize(payload_struct_type);

  uint32_t payload_idx = func_signature.DefineInput(air::InputPayload{.size = payload_struct_size});
  uint32_t thread_id_idx = func_signature.DefineInput(air::InputThreadPositionInThreadgroup{});
  // (batched_patch_id_start, instance_id, 0)
  uint32_t tg_id_idx = func_signature.DefineInput(air::InputThreadgroupPositionInGrid{});
  func_signature.DefineInput(air::InputMeshGridProperties{});
  uint32_t draw_argument_idx = func_signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = {},
      .location_index = SM50_BINDING_INDEX_DRAW_ARGUMENTS,
      .array_size = 0,
      .memory_access = air::MemoryAccess::read,
      .address_space = air::AddressSpace::constant,
      .type = is_indexed_draw //
                  ? air::MSLWhateverStruct{"draw_indexed_arguments", types._dxmt_draw_indexed_arguments}
                  : air::MSLWhateverStruct{"draw_arguments", types._dxmt_draw_arguments},
      .arg_name = "draw_arguments",
      .raster_order_group = {}
  });

  uint32_t index_buffer_idx = ~0u;
  if (is_indexed_draw) {
    index_buffer_idx = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = {},
        .location_index = SM50_BINDING_INDEX_INDEX_BUFFER,
        .array_size = 0,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::device,
        .type = ia_layout->index_buffer_format == 1 ? air::MSLRepresentableType(air::MSLUshort{})
                                                    : air::MSLRepresentableType(air::MSLUint{}),
        .raster_order_group = {}
    });
  }

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, true);

  auto entry_bb_global = llvm::BasicBlock::Create(context, "entry", function);
  auto vertex_stage_end = llvm::BasicBlock::Create(context, "vertex_end", function);
  llvm::IRBuilder<> builder(entry_bb_global);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );

  // these values are initialized by vertex stage and accessed by hull stage
  llvm::Value *instance_id = nullptr;
  llvm::Value *batched_patch_start = nullptr;
  llvm::Value *patch_id = nullptr;
  llvm::Value *patch_count = nullptr;

  setup_temp_register(&vertex_shader_info, resource_map_vs, types, module, builder);
  setup_temp_register(&hull_shader_info, resource_map_hs, types, module, builder);
  setup_immediate_constant_buffer(&vertex_shader_info, resource_map_vs, types, module, builder);
  setup_immediate_constant_buffer(&hull_shader_info, resource_map_hs, types, module, builder);

  auto output_reg_per_point_type = llvm::ArrayType::get(types._int4, max_vs_output_register);
  auto output_reg_per_patch_type =
      llvm::ArrayType::get(output_reg_per_point_type, pHullStage->input_control_point_count);
  auto output_reg_per_group_type = llvm::ArrayType::get(output_reg_per_patch_type, patch_per_group);
  auto output_reg_per_point_type_float = llvm::ArrayType::get(types._float4, max_vs_output_register);
  auto output_reg_per_patch_type_float =
      llvm::ArrayType::get(output_reg_per_point_type_float, pHullStage->input_control_point_count);

  llvm::GlobalVariable *vertex_out = new llvm::GlobalVariable(
      module, output_reg_per_group_type, false, llvm::GlobalValue::InternalLinkage,
      llvm::UndefValue::get(output_reg_per_group_type), "vertex_out_hull_in", nullptr,
      llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
  );
  vertex_out->setAlignment(llvm::Align(4));

  // AnyOS: the patches of a threadgroup add their mesh workloads to one
  // counter. It is zeroed once here, before the barrier that ends the vertex
  // zone (every thread stores the same 0), not by each patch's first thread:
  // that cleared other patches' already-counted workloads, and the count was
  // read before every patch had added to it, so the mesh stage ran stale
  // workload entries (flickering spikes and flat triangles, Unigine Heaven
  // instanced tessellation, 2026-10-02).
  llvm::GlobalVariable *workload_count = new llvm::GlobalVariable(
      module, types._int, false, llvm::GlobalValue::InternalLinkage, llvm::UndefValue::get(types._int),
      "workload_count", nullptr, llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
  );
  workload_count->setAlignment(llvm::Align(4));
  builder.CreateStore(builder.getInt32(0), workload_count);

  auto thread_position_in_group = function->getArg(thread_id_idx);
  auto control_point_id_in_patch = builder.CreateExtractElement(thread_position_in_group, (uint32_t)0);
  auto patch_offset_in_group = builder.CreateExtractElement(thread_position_in_group, 1);

  {
    /* Vertex Shader Zone */
    auto shader_info = vertex_shader_info;
    auto &resource_map = resource_map_vs;
    auto &prologue = prologue_vs;
    auto &epilogue = epilogue_vs;
    auto max_input_register = max_vs_input_register;
    auto max_output_register = max_vs_output_register;

    auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue_vertex", function);
    setup_metal_version(module, metal_version);

    auto active = llvm::BasicBlock::Create(context, "active_vertex", function);

    auto threadgroup_position_in_grid = function->getArg(tg_id_idx);
    batched_patch_start = builder.CreateMul(
        builder.CreateExtractElement(threadgroup_position_in_grid, (uint32_t)0),
        builder.getInt32(32 / threads_per_patch)
    );
    patch_id = builder.CreateAdd(batched_patch_start, patch_offset_in_group);
    instance_id = builder.CreateExtractElement(threadgroup_position_in_grid, (uint32_t)1);
    auto control_point_index = builder.CreateAdd(
        builder.CreateMul(patch_id, builder.getInt32(pHullStage->input_control_point_count)), control_point_id_in_patch
    );

    auto draw_arguments = builder.CreateLoad(
        is_indexed_draw ? types._dxmt_draw_indexed_arguments : types._dxmt_draw_arguments,
        function->getArg(draw_argument_idx)
    );

    patch_count = builder.CreateUDiv(
        builder.CreateExtractValue(draw_arguments, 0), builder.getInt32(pHullStage->input_control_point_count)
    );

    resource_map.input.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
    resource_map.input.ptr_float4 = builder.CreateBitCast(
        resource_map.input.ptr_int4, llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
    );
    resource_map.input_element_count = max_input_register;

    // of type "output_reg_per_point_type"
    resource_map.output.ptr_int4 = builder.CreateGEP(
        output_reg_per_group_type, vertex_out, {builder.getInt32(0), patch_offset_in_group, control_point_id_in_patch}
    );
    resource_map.output.ptr_float4 = builder.CreateBitCast(
        resource_map.output.ptr_int4, output_reg_per_point_type_float->getPointerTo(vertex_out->getAddressSpace())
    );

    resource_map.output_element_count = max_output_register;

    struct context ctx {
        .builder = builder,
        .air = air,
        .binding = *binding_map,
        .llvm = context,
        .module = module,
        .function = function,
        .resource = resource_map,
        .types = types,
        .pso_sample_mask = 0xffffffff,
        .shader_type = pVertexStage->shader_type,
        .metal_version = metal_version,
    };

    builder.CreateCondBr(
        builder.CreateLogicalAnd(
            builder.CreateICmp(
                llvm::CmpInst::ICMP_ULT, control_point_id_in_patch,
                builder.getInt32(pHullStage->input_control_point_count)
            ),
            builder.CreateICmp(llvm::CmpInst::ICMP_ULT, patch_id, patch_count)
        ),
        active, vertex_stage_end
    );
    builder.SetInsertPoint(active);

    if (index_buffer_idx != ~0u) {
      auto start_index = builder.CreateExtractValue(draw_arguments, 2);
      auto index_buffer = function->getArg(index_buffer_idx);
      auto index_buffer_element_type = index_buffer->getType()->getNonOpaquePointerElementType();
      auto vertex_id = builder.CreateLoad(
          index_buffer_element_type,
          builder.CreateGEP(
              index_buffer_element_type, index_buffer, {builder.CreateAdd(start_index, control_point_index)}
          )
      );
      resource_map.vertex_id = builder.CreateZExt(vertex_id, types._int);
    } else {
      resource_map.vertex_id = control_point_index;
    }

    resource_map.base_vertex_id = builder.CreateExtractValue(draw_arguments, is_indexed_draw ? 3 : 2);
    resource_map.instance_id = instance_id;
    resource_map.vertex_id_with_base = builder.CreateAdd(resource_map.vertex_id, resource_map.base_vertex_id);
    resource_map.base_instance_id = builder.CreateExtractValue(draw_arguments, is_indexed_draw ? 4 : 3);
    resource_map.instance_id_with_base = builder.CreateAdd(resource_map.instance_id, resource_map.base_instance_id);

    if (auto err = prologue.build(ctx).takeError()) {
      return err;
    }
    auto real_entry = convert_basicblocks(pVertexStage->entry(), ctx, epilogue_bb);
    if (auto err = real_entry.takeError()) {
      return err;
    }
    builder.CreateBr(real_entry.get());

    builder.SetInsertPoint(epilogue_bb);
    auto epilogue_result = epilogue.build(ctx);
    if (auto err = epilogue_result.takeError()) {
      return err;
    }

    builder.CreateBr(vertex_stage_end);
    builder.SetInsertPoint(vertex_stage_end);

    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
  }

  {

    /* Hull Shader Zone */
    auto shader_info = hull_shader_info;
    auto &resource_map = resource_map_hs;
    auto &prologue = prologue_hs;
    auto &epilogue = epilogue_hs;

    auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue_hull", function);

    uint32_t vertex_max_output_register = max_vs_output_register;

    auto payload = builder.CreateBitCast(function->getArg(payload_idx), payload_struct_type->getPointerTo(6));

    assert(pHullStage->input_control_point_count != ~0u);

    resource_map.input.ptr_int4 =
        builder.CreateGEP(output_reg_per_group_type, vertex_out, {builder.getInt32(0), patch_offset_in_group});
    resource_map.input.ptr_float4 = builder.CreateBitCast(
        resource_map.input.ptr_int4, output_reg_per_patch_type_float->getPointerTo(vertex_out->getAddressSpace())
    );
    resource_map.input_element_count = vertex_max_output_register;

    resource_map.instance_id = instance_id;

    resource_map.patch_id = patch_id;

    resource_map.thread_id_in_patch = builder.CreateSelect(
        builder.CreateICmp(llvm::CmpInst::ICMP_ULT, resource_map.patch_id, patch_count), control_point_id_in_patch,
        builder.getInt32(32) // so all phases are effectively skipped
    );

    if (shader_info.no_control_point_phase_passthrough) {
      // TODO: check this out carefully......
      assert(pHullStage->output_control_point_count != ~0u);

      if (shader_info.output_control_point_read) {
        llvm::GlobalVariable *control_point_phase_out = new llvm::GlobalVariable(
            module, hs_output_per_group_type, false, llvm::GlobalValue::InternalLinkage,
            llvm::UndefValue::get(hs_output_per_group_type), "hull_control_point_phase_out", nullptr,
            llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
        );
        control_point_phase_out->setAlignment(llvm::Align(4));
        resource_map.output.ptr_int4 = builder.CreateGEP(
            hs_output_per_group_type, control_point_phase_out, {builder.getInt32(0), patch_offset_in_group}
        );

        resource_map.hull_cp_passthrough_dst = builder.CreateGEP(
            payload_struct_type, payload,
            {builder.getInt32(0), builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
        );
        resource_map.hull_cp_passthrough_src = builder.CreateGEP(
            hs_output_per_group_type, control_point_phase_out,
            {builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
        );
        resource_map.hull_cp_passthrough_type = hs_output_per_point_type;
      } else {
        resource_map.output.ptr_int4 = builder.CreateGEP(
            payload_struct_type, payload,
            {builder.getInt32(0), builder.getInt32(0) /* field: control points*/, patch_offset_in_group}
        );
      }
      resource_map.output.ptr_float4 = builder.CreateBitCast(
          resource_map.output.ptr_int4,
          hs_output_per_patch_type_float->getPointerTo(
              cast<llvm::PointerType>(resource_map.output.ptr_int4->getType())->getPointerAddressSpace()
          )
      );
      resource_map.output_element_count = max_hs_output_register;
    } else {
      /* since no control point phase */
      if (vertex_max_output_register < max_hs_output_register) {
        return llvm::make_error<UnsupportedFeature>(
            "Hull shader has control point phase pass-through, but vertex shader "
            "output signature doesn't match hull shader input signature."
        );
      }
      resource_map.output.ptr_float4 = resource_map.input.ptr_float4;
      resource_map.output.ptr_int4 = resource_map.input.ptr_int4;
      resource_map.output_element_count = resource_map.input_element_count;

      resource_map.hull_cp_passthrough_dst = builder.CreateGEP(
          payload_struct_type, payload,
          {builder.getInt32(0), builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
      );
      resource_map.hull_cp_passthrough_src = builder.CreateGEP(
          output_reg_per_group_type, vertex_out,
          {builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
      );
      resource_map.hull_cp_passthrough_type = hs_output_per_point_type;
    }

    if (max_patch_constant_output_register) {
      /* all instances write to the same output (threadgroup memory) */
      llvm::GlobalVariable *patch_constant_out = new llvm::GlobalVariable(
          module, hs_pcout_per_group_type, false, llvm::GlobalValue::InternalLinkage,
          llvm::UndefValue::get(hs_pcout_per_group_type), "hull_patch_constant_out", nullptr,
          llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
      );
      patch_constant_out->setAlignment(llvm::Align(4));
      resource_map.patch_constant_output.ptr_int4 =
          builder.CreateGEP(hs_pcout_per_group_type, patch_constant_out, {builder.getInt32(0), patch_offset_in_group});
      resource_map.patch_constant_output.ptr_float4 = builder.CreateBitCast(
          resource_map.patch_constant_output.ptr_int4,
          hs_pcout_per_patch_type_float->getPointerTo((uint32_t)air::AddressSpace::threadgroup)
      );
    }

    struct context ctx {
        .builder = builder,
        .air = air,
        .binding = *binding_map_hs,
        .llvm = context,
        .module = module,
        .function = function,
        .resource = resource_map,
        .types = types,
        .pso_sample_mask = 0xffffffff,
        .shader_type = pHullStage->shader_type,
        .metal_version = metal_version,
    };
    dxbc::Converter dxbc(ctx.air, ctx, ctx.resource);

    if (auto err = prologue.build(ctx).takeError()) {
      return err;
    }
    auto real_entry = convert_basicblocks(pHullStage->entry(), ctx, epilogue_bb);
    if (auto err = real_entry.takeError()) {
      return err;
    }
    builder.CreateBr(real_entry.get());

    builder.SetInsertPoint(epilogue_bb);

    /* populate patch constant output */

    auto write_patch_constant = llvm::BasicBlock::Create(context, "write_patch_constant", function);
    auto dispatch_mesh = llvm::BasicBlock::Create(context, "dispatch_mesh", function);
    auto real_return = llvm::BasicBlock::Create(context, "real_return", function);

    builder.CreateCondBr(
        builder.CreateICmp(llvm::CmpInst::ICMP_EQ, resource_map.thread_id_in_patch, builder.getInt32(0)),
        write_patch_constant, real_return
    );

    builder.SetInsertPoint(write_patch_constant);
    if (auto err = epilogue.build(ctx).takeError()) {
      return err;
    }

    std::array<llvm::Value *, 6> tess_factors;
    tess_factors.fill(air.getFloat(0));

    auto max_tess_factor_value = air.getFloat(final_maxtessfactor);

    for (unsigned i = 0; i < pHullStage->patch_constant_scalars.size(); i++) {
      auto pc_scalar = pHullStage->patch_constant_scalars[i];
      auto dst_ptr = builder.CreateGEP(
          payload_struct_type, payload,
          {builder.getInt32(0),   // struct
           builder.getInt32(1),   // pcout
           patch_offset_in_group, // patch_id,
           builder.getInt32(i)}
      );
      auto src_ptr = builder.CreateGEP(
          hs_pcout_per_patch_type, resource_map.patch_constant_output.ptr_int4,
          {builder.getInt32(0), builder.getInt32(pc_scalar.reg), builder.getInt32(pc_scalar.component)}
      );
      if (pc_scalar.tess_factor_index >= 0 && pc_scalar.tess_factor_index < 6) {
        auto value = builder.CreateBitCast(builder.CreateLoad(types._int, src_ptr), types._float);
        auto value_clamp = air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, value, max_tess_factor_value);
        tess_factors[pc_scalar.tess_factor_index] = value_clamp;
        builder.CreateStore(builder.CreateBitCast(value_clamp, types._int), dst_ptr);
      } else {
        builder.CreateStore(builder.CreateLoad(types._int, src_ptr), dst_ptr);
      }
    };


    switch (pHullStage->tessellation_domain) {
    case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE:
      // AnyOS: line detail is tess factor 0, line density 1.
      dxbc.HullGenerateWorkloadForIsoline(
          patch_offset_in_group, workload_count,
          builder.CreateGEP(
              payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)}
          ),
          get_partitioning(pHullStage), tess_factors[0], tess_factors[1]
      );
      break;
    case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_QUAD:
      dxbc.HullGenerateWorkloadForQuad(
          patch_offset_in_group, workload_count,
          builder.CreateGEP(
              payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)}
          ),
          get_partitioning(pHullStage), tess_factors[4], tess_factors[5], tess_factors[0], tess_factors[1],
          tess_factors[2], tess_factors[3]
      );
      break;
    default: {
      dxbc.HullGenerateWorkloadForTriangle(
          patch_offset_in_group, workload_count,
          builder.CreateGEP(
              payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)}
          ),
          get_partitioning(pHullStage), tess_factors[3], tess_factors[0], tess_factors[1], tess_factors[2]
      );
    }
    }

    builder.CreateBr(real_return);
    builder.SetInsertPoint(real_return);

    // Every patch has added its workloads: one thread dispatches the meshes.
    // AnyOS: the payload (object_data) too -- the group packer below reads the
    // workloads the other patches' threads wrote.
    air.CreateBarrier((llvm::air::MemFlags)((uint32_t)llvm::air::MemFlags::Threadgroup | (uint32_t)llvm::air::MemFlags::ObjectData));
    auto final_return = llvm::BasicBlock::Create(context, "final_return", function);
    builder.CreateCondBr(
        builder.CreateAnd(
            builder.CreateICmp(llvm::CmpInst::ICMP_EQ, patch_offset_in_group, builder.getInt32(0)),
            builder.CreateICmp(llvm::CmpInst::ICMP_EQ, control_point_id_in_patch, builder.getInt32(0))
        ),
        dispatch_mesh, final_return
    );

    builder.SetInsertPoint(dispatch_mesh);

    llvm::Value *meshgroup_to_dispatch = air.getInt3(1, 1, 1);
    llvm::Value *meshgroups = builder.CreateLoad(types._int, workload_count);
    if (group_plan.enabled) {
      // AnyOS: one mesh threadgroup per workload group
      meshgroups = dxbc.HullPackWorkloadGroups(
          builder.CreateGEP(
              payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)}
          ),
          meshgroups, builder.getInt32(group_plan.max_vertices), builder.getInt32(group_plan.max_primitives),
          builder.CreateGEP(
              payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(4), builder.getInt32(0)}
          )
      );
    }
    meshgroup_to_dispatch = builder.CreateInsertElement(meshgroup_to_dispatch, meshgroups, (uint64_t)0);

    air.CreateSetMeshProperties(meshgroup_to_dispatch);

    builder.CreateStore(
        batched_patch_start,
        builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(2)})
    );

    builder.CreateBr(final_return);
    builder.SetInsertPoint(final_return);
  }

  builder.CreateRetVoid();

  module.getOrInsertNamedMetadata("air.object")->addOperand(function_metadata);
  return llvm::Error::success();
}

llvm::Error
convert_dxbc_tesselator_domain_shader(
    SM50ShaderInternal *pShaderInternal, const char *name, SM50ShaderInternal *pHullStage, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  uint32_t max_input_register = pShaderInternal->max_input_register;
  uint32_t max_output_register = pShaderInternal->max_output_register;
  SM50_SHADER_GS_PASS_THROUGH_DATA *gs_passthrough = nullptr;
  bool rasterization_disabled =
      (args_get_data<SM50_SHADER_GS_PASS_THROUGH, SM50_SHADER_GS_PASS_THROUGH_DATA>(pArgs, &gs_passthrough) &&
       gs_passthrough->RasterizationDisabled);
  // AnyOS: tessellation followed by a geometry shader -- stream the vertices
  // and primitives to buffers for the geometry pipeline instead of rasterizing.
  SM50_SHADER_TESSELLATION_STREAM_DATA *stream_data = nullptr;
  bool stream = args_get_data<SM50_SHADER_TESSELLATION_STREAM, SM50_SHADER_TESSELLATION_STREAM_DATA>(pArgs, &stream_data);
  if (stream)
    rasterization_disabled = true;
  // AnyOS: domain shader stream output -- the stream above plus a record per
  // workload (dxmt.domain_so_stream_primitives.*).
  SM50_SHADER_TESSELLATION_STREAM_OUTPUT_DATA *so_data = nullptr;
  bool stream_output =
      stream && args_get_data<SM50_SHADER_TESSELLATION_STREAM_OUTPUT, SM50_SHADER_TESSELLATION_STREAM_OUTPUT_DATA>(
                    pArgs, &so_data
                );
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);

  auto [final_maxtessfactor, factor_int] = get_final_maxtessfactor(pHullStage, pArgs);

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue epilogue([](struct context ctx) -> pvalue {
    return nullptr; // a mesh shader...
  });

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }
  }

  // TESS TODO: 17e30e0855effcaa4da9a05c018a3ba7189d7653 changes

  auto &ds_output_handlers = pShaderInternal->mesh_output_handlers;

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D11_SB_DOMAIN_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);

  uint32_t rta_idx_out = ~0u;
  if (gs_passthrough && gs_passthrough->Data.RenderTargetArrayIndexReg != 255) {
    rta_idx_out = func_signature.DefineMeshPrimitiveOutput(air::OutputRenderTargetArrayIndex{});
  }
  uint32_t va_idx_out = ~0u;
  if (gs_passthrough && gs_passthrough->Data.ViewportArrayIndexReg != 255) {
    va_idx_out = func_signature.DefineMeshPrimitiveOutput(air::OutputViewportArrayIndex{});
  }

  uint32_t thread_id_idx = func_signature.DefineInput(air::InputThreadPositionInThreadgroup{});
  uint32_t tg_id_idx = func_signature.DefineInput(air::InputThreadgroupPositionInGrid{});

  uint32_t threads_per_patch = next_pow2(pHullStage->hull_maximum_threads_per_patch);
  uint32_t patch_per_group = next_pow2(32 / threads_per_patch);

  auto hs_output_per_point_type = llvm::ArrayType::get(types._int4, pHullStage->max_output_register);
  auto hs_output_per_patch_type =
      llvm::ArrayType::get(hs_output_per_point_type, pHullStage->output_control_point_count);
  auto hs_output_per_group_type = llvm::ArrayType::get(hs_output_per_patch_type, patch_per_group);
  auto hs_output_per_point_type_float = llvm::ArrayType::get(types._float4, pHullStage->max_output_register);
  auto hs_output_per_patch_type_float =
      llvm::ArrayType::get(hs_output_per_point_type_float, pHullStage->output_control_point_count);

  auto hs_pcout_per_patch_scaler_type = llvm::ArrayType::get(types._int, pHullStage->patch_constant_scalars.size());
  auto hs_pcout_per_group_scaler_type = llvm::ArrayType::get(hs_pcout_per_patch_scaler_type, patch_per_group);

  // n segments has n + 1 points
  uint32_t max_edge_point = factor_int + 1;
  uint32_t max_workload_count = get_max_potential_workload_count(factor_int, pHullStage);
  constexpr uint32_t size_workload_info = sizeof(TessMeshWorkload);
  auto payload_struct_type = llvm::StructType::create(
      context,
      {hs_output_per_group_type, hs_pcout_per_group_scaler_type, types._int,
       llvm::ArrayType::get(types._int, max_workload_count * patch_per_group * size_workload_info / 4),
       llvm::ArrayType::get(types._int, max_workload_count * patch_per_group + 1)}, // AnyOS: group table
      "payload"
  );
  uint32_t payload_struct_size = module.getDataLayout().getTypeAllocSize(payload_struct_type);

  uint32_t payload_idx = func_signature.DefineInput(air::InputPayload{.size = payload_struct_size});

  bool point_rasterization = false;
  auto group_plan = get_mesh_group_plan(pHullStage, pArgs, stream, max_workload_count * patch_per_group);

  switch (pHullStage->tessellator_output_primitive) {
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_POINT: {
    func_signature.DefineInput(
        air::InputMesh{(uint32_t)max_edge_point * 2, (uint32_t)max_edge_point * 2, air::MeshOutputTopology::Point}
    );
    point_rasterization = true; // TESS TODO: not necessary... if combined with GS/SO
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_LINE: {
    // AnyOS: one isoline per workload (generatePrimitiveLine).
    func_signature.DefineInput(
        air::InputMesh{(uint32_t)max_edge_point, (uint32_t)max_edge_point - 1, air::MeshOutputTopology::Line}
    );
    break;
  }
  default: {
    if (group_plan.enabled) {
      // AnyOS: a workload group fills the mesh output
      func_signature.DefineInput(
          air::InputMesh{group_plan.max_vertices, group_plan.max_primitives, air::MeshOutputTopology::Triangle}
      );
      break;
    }
    func_signature.DefineInput(air::InputMesh{
        (uint32_t)(max_edge_point + 2 - (max_edge_point & 1)) * 2 + 1,
        (uint32_t)(max_edge_point + 2 - (max_edge_point & 1)) * 2 + 1,
        air::MeshOutputTopology::Triangle
    });
    break;
  }
  }

  if (point_rasterization) {
    func_signature.DefineMeshVertexOutput(air::OutputPointSize {});
  }

  if (pShaderInternal->clip_distance_scalars.size() > 0) {
    func_signature.DefineMeshVertexOutput(
        air::OutputClipDistance{.count = pShaderInternal->clip_distance_scalars.size()}
    );
  }

  uint32_t stream_vertex_idx = ~0u, stream_index_idx = ~0u, stream_counter_idx = ~0u, stream_record_idx = ~0u;
  if (stream) {
    auto stream_buffer = [&](uint32_t location, const char *arg_name) {
      return func_signature.DefineInput(air::ArgumentBindingBuffer{
          .buffer_size = {},
          .location_index = location,
          .array_size = 0,
          .memory_access = air::MemoryAccess::read_write,
          .address_space = air::AddressSpace::device,
          .type = air::MSLUint{},
          .arg_name = arg_name,
          .raster_order_group = {}
      });
    };
    stream_vertex_idx = stream_buffer(SM50_BINDING_INDEX_TESS_STREAM_VERTEX, "tess_stream_vertex");
    stream_index_idx = stream_buffer(SM50_BINDING_INDEX_TESS_STREAM_INDEX, "tess_stream_index");
    stream_counter_idx = stream_buffer(SM50_BINDING_INDEX_TESS_STREAM_COUNTER, "tess_stream_counter");
    if (stream_output)
      stream_record_idx = stream_buffer(SM50_BINDING_INDEX_TESS_STREAM_RECORD, "tess_stream_record");
  }

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, rasterization_disabled);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto vertex_start = llvm::BasicBlock::Create(context, "vertex_start", function);
  auto vertex_emit = llvm::BasicBlock::Create(context, "vertex_emit", function);
  auto vertex_end = llvm::BasicBlock::Create(context, "vertex_end", function);
  auto generate_primitive_pre = llvm::BasicBlock::Create(context, "generate_primitive_pre", function);
  auto generate_primitive = llvm::BasicBlock::Create(context, "generate_primitive", function);
  auto real_return = llvm::BasicBlock::Create(context, "real_return", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );

  setup_metal_version(module, metal_version);
  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(shader_info, resource_map, types, module, builder);

  auto workload_index = builder.CreateExtractElement(function->getArg(tg_id_idx), 0ull);
  auto thread_index = builder.CreateExtractElement(function->getArg(thread_id_idx), 0ull);

  auto payload = builder.CreateBitCast(function->getArg(payload_idx), payload_struct_type->getPointerTo(6));

  auto data = builder.CreateGEP(
      payload_struct_type, payload,
      {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)}
  );

  struct context ctx {
      .builder = builder,
      .air = air,
      .binding = *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = resource_map,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
  };
  dxbc::Converter dxbc(ctx.air, ctx, ctx.resource);

  auto batched_patch_start = builder.CreateLoad(
      types._int, builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(2)})
  );

  // AnyOS: mesh workload groups -- this threadgroup's workload range
  llvm::Value *group_first = nullptr, *group_last = nullptr;
  if (group_plan.enabled) {
    group_first = builder.CreateLoad(
        types._int,
        builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(4), workload_index})
    );
    group_last = builder.CreateLoad(
        types._int, builder.CreateGEP(
                        payload_struct_type, payload,
                        {builder.getInt32(0), builder.getInt32(4),
                         builder.CreateAdd(workload_index, builder.getInt32(1))}
                    )
    );
  }

  llvm::Value *stream_vertex_base = nullptr;
  if (stream) {
    stream_vertex_base =
        dxbc.DomainStreamReserve(workload_index, data, function->getArg(stream_counter_idx));
  }

  // AnyOS: the patch of a workload (per threadgroup; per vertex when the
  // threadgroup runs a workload group, whose workloads may belong to
  // different patches of the object threadgroup)
  auto setup_patch = [&](llvm::Value *patch_index) {
  resource_map.patch_id = builder.CreateAdd(batched_patch_start, patch_index);

  resource_map.input.ptr_int4 = builder.CreateGEP(
      payload_struct_type, payload,
      {builder.getInt32(0), // struct
       builder.getInt32(0), // controlpoint
       patch_index}
  );
  resource_map.input.ptr_float4 =
      builder.CreateBitCast(resource_map.input.ptr_int4, hs_output_per_patch_type_float->getPointerTo(6));
  resource_map.input_element_count = pHullStage->max_output_register;
  };

  resource_map.patch_constant_output.ptr_int4 =
      builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
  resource_map.patch_constant_output.ptr_float4 = builder.CreateBitCast(
      resource_map.patch_constant_output.ptr_int4,
      llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
  );

  resource_map.output.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_output_register));
  resource_map.output.ptr_float4 = builder.CreateBitCast(
      resource_map.output.ptr_int4,
      llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo(
          cast<llvm::PointerType>(resource_map.output.ptr_int4->getType())->getPointerAddressSpace()
      )
  );
  resource_map.output_element_count = max_output_register;

  /* setup patch constant register */
  auto setup_patch_constants = [&](llvm::Value *patch_index) {
  for (auto x : llvm::enumerate(pHullStage->patch_constant_scalars)) {
    if (x.value().reg >= max_input_register)
      continue;
    auto src_ptr = builder.CreateGEP(
        payload_struct_type, payload,
        {builder.getInt32(0), // struct
         builder.getInt32(1), // pcout
         patch_index, builder.getInt32(x.index())}
    );
    auto dst_ptr = builder.CreateGEP(
        llvm::ArrayType::get(types._int4, max_input_register), resource_map.patch_constant_output.ptr_int4,
        {builder.getInt32(0), builder.getInt32(x.value().reg), builder.getInt32(x.value().component)}
    );
    builder.CreateStore(builder.CreateLoad(types._int, src_ptr), dst_ptr);
  }
  };

  if (!group_plan.enabled) {
    auto patch_index = dxbc.DomainGetPatchIndex(workload_index, data);
    setup_patch(patch_index);
    setup_patch_constants(patch_index);
  }

  builder.CreateBr(vertex_start);

  builder.SetInsertPoint(vertex_start);

  auto phi_thread_index_base = builder.CreatePHI(types._int, 2);
  phi_thread_index_base->addIncoming(air.getInt(0), entry_bb);

  auto actual_thread_index = builder.CreateAdd(phi_thread_index_base, thread_index);

  llvm::Value *location, *active, *iterate;
  if (group_plan.enabled) {
    // AnyOS: vertex actual_thread_index of the group -> (workload, local vertex)
    auto packed = dxbc.DomainLocateGroupVertex(group_first, group_last, actual_thread_index, data);
    auto vertex_workload = builder.CreateAnd(packed, builder.getInt32(0xffff));
    auto vertex_local = builder.CreateLShr(packed, builder.getInt32(16));
    std::tie(location, active, iterate) =
        dxbc.DomainGetLocation(vertex_workload, vertex_local, data, get_partitioning(pHullStage));
    auto patch_index = dxbc.DomainGetPatchIndex(vertex_workload, data);
    setup_patch(patch_index);
    setup_patch_constants(patch_index);
  } else {
    std::tie(location, active, iterate) =
        dxbc.DomainGetLocation(workload_index, actual_thread_index, data, get_partitioning(pHullStage));
  }

  // It accidentally works on quad as well
  {
    llvm::Value *domain_bary = llvm::UndefValue::get(air.getFloatTy(3));
    llvm::Value *u = builder.CreateExtractElement(location, 0ull);
    llvm::Value *v = builder.CreateExtractElement(location, 1ull);
    llvm::Value *w = builder.CreateFSub(air.getFloat(1.0f), builder.CreateFAdd(u, v));
    domain_bary = builder.CreateInsertElement(domain_bary, u, 0ull);
    domain_bary = builder.CreateInsertElement(domain_bary, v, 1ull);
    domain_bary = builder.CreateInsertElement(domain_bary, w, 2ull);
    resource_map.domain = domain_bary;
  }

  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, vertex_emit);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateCondBr(active, real_entry.get(), vertex_end);

  builder.SetInsertPoint(vertex_emit);

  auto vertex_id = actual_thread_index;
  auto primitive_id = actual_thread_index; // 
  if (stream) {
    // Every output register of the vertex, at (vertex base + vertex) * registers.
    auto store_bb = llvm::BasicBlock::Create(context, "stream_store", function);
    builder.CreateCondBr(
        builder.CreateICmpNE(stream_vertex_base, builder.getInt32(-1)), store_bb, vertex_end
    );
    builder.SetInsertPoint(store_bb);
    auto out_ptr = builder.CreateBitCast(function->getArg(stream_vertex_idx), types._int4->getPointerTo(1));
    auto slot = builder.CreateMul(
        builder.CreateAdd(stream_vertex_base, vertex_id), builder.getInt32(max_output_register)
    );
    for (uint32_t reg = 0; reg < max_output_register; reg++) {
      auto src_ptr = builder.CreateGEP(
          llvm::ArrayType::get(types._int4, max_output_register), resource_map.output.ptr_int4,
          {builder.getInt32(0), builder.getInt32(reg)}
      );
      builder.CreateStore(
          builder.CreateLoad(types._int4, src_ptr),
          builder.CreateGEP(types._int4, out_ptr, {builder.CreateAdd(slot, builder.getInt32(reg))})
      );
    }
    builder.CreateBr(vertex_end);
  } else {
  MeshOutputContext gs_out_ctx{vertex_id, primitive_id};
  for (auto &h : ds_output_handlers) {
    if (auto err = h(gs_out_ctx).build(ctx).takeError()) {
      return err;
    }
  }

  for (auto x : llvm::enumerate(pShaderInternal->clip_distance_scalars)) {
    if (x.value().reg >= max_output_register)
      continue;
    auto src_ptr = builder.CreateGEP(
        llvm::ArrayType::get(types._float4, max_output_register), resource_map.output.ptr_float4,
        {builder.getInt32(0), builder.getInt32(x.value().reg), builder.getInt32(x.value().component)}
    );
    air.CreateSetMeshClipDistance(
        vertex_id, builder.getInt32(x.index()), builder.CreateLoad(types._float, src_ptr)
    );
  }

  if (rta_idx_out != ~0u) {
    auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(), resource_map.output.ptr_int4,
        {builder.getInt32(0), builder.getInt32(gs_passthrough->Data.RenderTargetArrayIndexReg),
         builder.getInt32(gs_passthrough->Data.RenderTargetArrayIndexComponent)}
    );
    air.CreateSetMeshRenderTargetArrayIndex(primitive_id, builder.CreateLoad(types._int, src_ptr));
  }
  if (va_idx_out != ~0u) {
    auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(), resource_map.output.ptr_int4,
        {builder.getInt32(0), builder.getInt32(gs_passthrough->Data.ViewportArrayIndexReg),
         builder.getInt32(gs_passthrough->Data.ViewportArrayIndexComponent)}
    );
    air.CreateSetMeshViewportArrayIndex(primitive_id, builder.CreateLoad(types._int, src_ptr));
  }

  if (point_rasterization) {
    air.CreateSetMeshPointSize(vertex_id, air.getFloat(1.0));
  }

  builder.CreateBr(vertex_end);
  }

  builder.SetInsertPoint(vertex_end);
  phi_thread_index_base->addIncoming(builder.CreateAdd(phi_thread_index_base, air.getInt(32)), vertex_end);
  builder.CreateCondBr(iterate, vertex_start, generate_primitive_pre);

  builder.SetInsertPoint(generate_primitive_pre);
  // AnyOS: rasterized triangle domains generate primitives from every mesh
  // thread (the helper receives the thread index in workload_index bits 16..)
  bool parallel_primitives =
      !stream && !point_rasterization &&
      pHullStage->tessellator_output_primitive != microsoft::D3D11_SB_TESSELLATOR_OUTPUT_LINE;
  if (parallel_primitives || group_plan.enabled)
    builder.CreateBr(generate_primitive);
  else
    builder.CreateCondBr(
        builder.CreateICmp(llvm::CmpInst::ICMP_EQ, thread_index, builder.getInt32(0)), generate_primitive, real_return
    );
  builder.SetInsertPoint(generate_primitive);

  if (stream_output) {
    dxbc.DomainSOStreamPrimitives(
        workload_index, data, function->getArg(stream_index_idx), function->getArg(stream_counter_idx),
        stream_vertex_base, function->getArg(stream_record_idx), resource_map.patch_id,
        get_output_primitive(pHullStage)
    );
  } else if (stream) {
    dxbc.DomainStreamPrimitives(
        workload_index, data, function->getArg(stream_index_idx), function->getArg(stream_counter_idx),
        stream_vertex_base, get_output_primitive(pHullStage)
    );
  } else if (group_plan.enabled) {
    // AnyOS: the group's primitives: first | last << 10 | thread << 20
    dxbc.DomainGenerateGroupPrimitives(
        builder.CreateOr(
            builder.CreateOr(group_first, builder.CreateShl(group_last, 10)), builder.CreateShl(thread_index, 20)
        ),
        data, get_output_primitive(pHullStage)
    );
  } else if (parallel_primitives) {
    // AnyOS: triangle primitive generation also receives the mesh thread index (bits 16..)
    dxbc.DomainGeneratePrimitives(
        builder.CreateOr(workload_index, builder.CreateShl(thread_index, 16)), data, get_output_primitive(pHullStage)
    );
  } else {
    dxbc.DomainGeneratePrimitives(workload_index, data, get_output_primitive(pHullStage));
  }

  builder.CreateBr(real_return);
  builder.SetInsertPoint(real_return);
  builder.CreateRetVoid();

  module.getOrInsertNamedMetadata("air.mesh")->addOperand(function_metadata);
  return llvm::Error::success();
}

} // namespace dxmt::dxbc