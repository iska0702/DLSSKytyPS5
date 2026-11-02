// KYTY_WAVE32_CLUSTERS tests (included by ShaderRecompilerComputeTests.cpp after
// ShaderCodegenTests.inc).
//
// The AMD proprietary driver runs vertex, mesh and pixel shaders on 64-wide subgroups and lets only
// compute pipelines require 32, so a wave32 graphics program shares a host subgroup with a second
// guest wave. Its waterfall loops (V_READFIRSTLANE, V_CMP_EQ, S_AND_SAVEEXEC_B32, S_XOR_B32 EXEC,
// S_CBRANCH_EXECNZ) then read the other wave's EXEC word and key, never remove their own lanes,
// and spin until a device reset. Clusters keep every guest wave to its own 32 host lanes. A 32-wide
// host (the RTX 3090) cannot show the hang; these tests check that the cluster code is chosen only
// where it is needed, leaves the SPIR-V unchanged elsewhere, validates, and gives the guest's
// results when forced (KYTY_WAVE32_CLUSTERS=force) on a 32-wide host.

namespace WaveClusterTests {

using CodegenTests::CountSpirv;
using CodegenTests::RequireEqualWords;
using CodegenTests::RunCollect;
using CodegenTests::ScopedCodegenOptions;
using ShaderRecompiler::GetCodegenOptions;
using ShaderRecompiler::Wave32Clusters;
namespace Spirv = ShaderRecompiler::Spirv;

// Restores the device layer's wave-cluster stages.
class ScopedHostWaveClusters {
public:
  explicit ScopedHostWaveClusters(const Spirv::HostWaveClusters &clusters)
      : m_saved(Spirv::GetHostWaveClusters()) {
    Spirv::SetHostWaveClusters(clusters);
  }
  ~ScopedHostWaveClusters() { Spirv::SetHostWaveClusters(m_saved); }
  ScopedHostWaveClusters(const ScopedHostWaveClusters &) = delete;
  ScopedHostWaveClusters &operator=(const ScopedHostWaveClusters &) = delete;

private:
  Spirv::HostWaveClusters m_saved;
};

ShaderRecompiler::CodegenOptions WithClusters(Wave32Clusters mode) {
  auto options = GetCodegenOptions();
  options.wave32_clusters = mode;
  return options;
}

// A wave32 waterfall loop over eight keys (lane / 8) in a 64-thread workgroup (two waves): every
// lane stores the key it was served with, then its wave's iteration count (4: one per key).
TestCase MakeWaterfallWave32() {
  using O = ShaderOpcode;
  constexpr u32 Threads = 64;
  std::vector<u32> code;
  code.push_back(EncodeVop2(0x16u, 1u, InlineU32(3u), 0u)); // v_lshrrev_b32 v1, 3, v0
  code.push_back(EncodeSop1(0x03u, 10u, 126u));              // s_mov_b32 s10, exec_lo
  AppendVMovLiteral(&code, 2u, 0xdeadbeefu);
  code.push_back(EncodeSMovB32(11u, InlineU32(0u)));
  const auto loop = code.size();
  code.push_back(EncodeVop1(0x02u, 0u, Vgpr(1u)));               // v_readfirstlane_b32 s0, v1
  code.push_back(EncodeVopc(0xc2u, 0u, 1u));                      // v_cmp_eq_u32 vcc_lo, s0, v1
  code.push_back(EncodeSop1(0x3cu, 2u, 106u));                    // s_and_saveexec_b32 s2, vcc_lo
  code.push_back(EncodeVop1(0x01u, 2u, 0u));                      // v_mov_b32 v2, s0
  code.push_back(EncodeSop2(0x00u, 11u, 11u, InlineU32(1u)));     // s_add_u32 s11, s11, 1
  code.push_back(EncodeSop2(0x12u, 126u, 126u, 2u));              // s_xor_b32 exec_lo, exec_lo, s2
  const auto branch = code.size();
  code.push_back(EncodeSopp(0x09u, static_cast<u32>(static_cast<int32_t>(loop) -
                                                     static_cast<int32_t>(branch + 1u)) &
                                       0xffffu)); // s_cbranch_execnz loop
  code.push_back(EncodeSop1(0x03u, 126u, 10u));   // s_mov_b32 exec_lo, s10
  code.push_back(EncodeVop1(0x01u, 3u, 11u));     // v_mov_b32 v3, s11
  AppendStoreVgprAtLaneDwordOffset(&code, 2u, 0u, 0u);
  AppendStoreVgprAtLaneDwordOffset(&code, 3u, 0u, Threads);
  AppendEnd(&code);

  TestCase test;
  test.name = "Wave32ClusterWaterfall";
  test.code = std::move(code);
  test.initial = std::vector<u32>(2u * Threads, 0u);
  test.expected.resize(2u * Threads);
  for (u32 lane = 0; lane < Threads; lane++) {
    test.expected[lane] = lane / 8u;
    test.expected[Threads + lane] = 4u;
  }
  test.opcodes = {O::V_LSHRREV_B32,       O::S_MOV_B32,   O::V_MOV_B32,         O::V_READFIRSTLANE_B32,
                  O::V_CMP_EQ_U32,        O::S_AND_SAVEEXEC_B32, O::S_ADD_U32, O::S_XOR_B32,
                  O::S_CBRANCH_EXECNZ,    O::V_LSHLREV_B32, O::V_ADD_NC_U32,  O::BUFFER_STORE_DWORD,
                  O::S_ENDPGM};
  test.compute_info.threads_num[0] = Threads;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.wave_size = 32;
  test.has_compute_info = true;
  return test;
}

// A wave32 pixel shader that reads its first lane's V_MBCNT lane index (V_READFIRSTLANE).
GraphicsCase MakePixelFirstLaneWave32() {
  GraphicsCase test;
  test.name = "Wave32ClusterPixelFirstLane";
  test.wave_size = 32;
  AppendVop3(&test.fragment_code, 0x365u, 1u, 193u, InlineU32(0u)); // v_mbcnt_lo_u32_b32 v1, -1, 0
  test.fragment_code.push_back(EncodeVop1(0x02u, 0u, Vgpr(1u)));     // v_readfirstlane_b32 s0, v1
  test.fragment_code.push_back(EncodeVop1(0x01u, 2u, 0u));           // v_mov_b32 v2, s0
  test.fragment_code.push_back(EncodeVop1(0x06u, 2u, Vgpr(2u)));     // v_cvt_f32_u32 v2, v2
  test.fragment_code.push_back(EncodeExp0(0u, 0xfu, true, false, true));
  test.fragment_code.push_back(EncodeExp1(2u, 2u, 2u, 2u));
  AppendEnd(&test.fragment_code);
  return test;
}

// CPU only: which programs take clusters, and that every other program's SPIR-V is unchanged.
void CheckWaveClusterSelection() {
  constexpr const char *name = "Wave32ClusterSelection";
  namespace IR = ShaderRecompiler::IR;
  struct Row {
    ShaderType stage;
    u32 wave_size;
    Spirv::HostWaveClusters host;
    Wave32Clusters mode;
    bool active;
  };
  const Spirv::HostWaveClusters none{};
  const Spirv::HostWaveClusters amd{.vertex = true, .pixel = true, .mesh = true};
  const Spirv::HostWaveClusters radv{.vertex = true, .pixel = false, .mesh = false};
  const Row rows[] = {
      {ShaderType::Pixel, 32, amd, Wave32Clusters::Auto, true},
      {ShaderType::Vertex, 32, amd, Wave32Clusters::Auto, true},
      {ShaderType::Local, 32, amd, Wave32Clusters::Auto, true},
      {ShaderType::TessellationEvaluation, 32, amd, Wave32Clusters::Auto, true},
      {ShaderType::Mesh, 32, amd, Wave32Clusters::Auto, true},
      {ShaderType::Pixel, 64, amd, Wave32Clusters::Auto, false},
      {ShaderType::Mesh, 64, amd, Wave32Clusters::Force, false},
      {ShaderType::Compute, 32, amd, Wave32Clusters::Auto, false},
      {ShaderType::Compute, 32, none, Wave32Clusters::Force, true},
      {ShaderType::TessellationControl, 32, amd, Wave32Clusters::Force, false},
      {ShaderType::Pixel, 32, amd, Wave32Clusters::Off, false},
      {ShaderType::Pixel, 32, none, Wave32Clusters::Auto, false},
      {ShaderType::Pixel, 32, none, Wave32Clusters::Force, true},
      {ShaderType::Pixel, 32, radv, Wave32Clusters::Auto, false},
      {ShaderType::Vertex, 32, radv, Wave32Clusters::Auto, true},
      {ShaderType::Mesh, 32, radv, Wave32Clusters::Auto, false},
  };
  for (const auto &row : rows) {
    ScopedHostWaveClusters host(row.host);
    ScopedCodegenOptions scoped(WithClusters(row.mode));
    IR::Program program;
    program.stage = row.stage;
    program.wave_size = row.wave_size;
    Require(name, "selection", Spirv::WaveClustersActive(program) == row.active,
            "stage " + std::to_string(static_cast<u32>(row.stage)) + " wave" +
                std::to_string(row.wave_size) + " mode " +
                std::to_string(static_cast<u32>(row.mode)) + ": expected " +
                (row.active ? "clusters" : "no clusters"));
  }

  // NVIDIA (no cluster stages) and Off: bit-identical SPIR-V; Force: the cluster code, valid.
  const auto waterfall = MakeWaterfallWave32();
  std::vector<u32> base;
  {
    ScopedHostWaveClusters host(none);
    ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Off));
    base = CompileCase(waterfall, 32u).spirv;
  }
  for (const auto &clusters : {none, amd}) {
    // Compute never takes clusters outside Force (its pipelines require the wave size).
    ScopedHostWaveClusters host(clusters);
    ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Auto));
    Require(name, "auto compute", CompileCase(waterfall, 32u).spirv == base,
            "KYTY_WAVE32_CLUSTERS=auto changed a compute program");
  }
  std::vector<u32> forced;
  {
    ScopedHostWaveClusters host(none);
    ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Force));
    forced = CompileCase(waterfall, 32u).spirv;
  }
  ValidateSpirv(name, forced);
  Require(name, "force",
          CountSpirv(forced, "OpVectorExtractDynamic") > CountSpirv(base, "OpVectorExtractDynamic") &&
              CountSpirv(forced, "BuiltIn SubgroupLocalInvocationId") == 1u,
          "forced clusters did not select the wave's ballot word by its subgroup lane");
  {
    // A wave64 program is never clustered.
    auto wave64 = waterfall;
    wave64.compute_info.wave_size = 64;
    std::vector<u32> off;
    {
      ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Off));
      off = CompileCase(wave64, 32u).spirv;
    }
    ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Force));
    Require(name, "wave64", CompileCase(wave64, 32u).spirv == off,
            "KYTY_WAVE32_CLUSTERS=force changed a wave64 program");
  }

  // A wave32 pixel shader on an AMD-like host: clustered; on NVIDIA: unchanged.
  const auto pixel = MakePixelFirstLaneWave32();
  std::vector<u32> pixel_nvidia;
  std::vector<u32> pixel_off;
  std::vector<u32> pixel_amd;
  {
    ScopedHostWaveClusters host(none);
    ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Auto));
    pixel_nvidia = CompileFragmentCase(pixel).spirv;
  }
  {
    ScopedHostWaveClusters host(amd);
    ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Off));
    pixel_off = CompileFragmentCase(pixel).spirv;
  }
  {
    ScopedHostWaveClusters host(amd);
    ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Auto));
    pixel_amd = CompileFragmentCase(pixel).spirv;
  }
  ValidateSpirv(name, pixel_amd);
  Require(name, "pixel", pixel_nvidia == pixel_off && pixel_amd != pixel_off &&
                             CountSpirv(pixel_amd, "OpVectorExtractDynamic") >
                                 CountSpirv(pixel_off, "OpVectorExtractDynamic"),
          "a wave32 pixel shader is not clustered exactly where the host needs it");
  std::printf("[codegen] %-32s ok (%zu selection rows)\n", name, std::size(rows));
}

// GPU: the waterfall gives the guest's results with clusters off and forced (on a 32-wide host the
// cluster is the whole subgroup, so both must match the ISA).
void CheckWaveClusterWaterfall(VulkanHarness *vulkan) {
  constexpr const char *name = "Wave32ClusterWaterfall";
  const auto test = MakeWaterfallWave32();
  for (const auto mode : {Wave32Clusters::Off, Wave32Clusters::Force}) {
    ScopedCodegenOptions scoped(WithClusters(mode));
    const auto actual = RunCollect(vulkan, test, test.expected.size());
    RequireEqualWords(name, mode == Wave32Clusters::Off ? "off" : "force", test.expected, actual);
  }
  std::printf("[compute] %-32s ok (subgroup %u)\n", name, vulkan->SubgroupSize());
}

// KYTY_LDS_WAITCNT_BARRIER: S_WAITCNT lgkmcnt(0) after an LDS write is a subgroup-scope memory
// barrier by default (never a workgroup-scope one, which the AMD driver turns into a workgroup
// barrier in mesh shaders: Astro Bot's title-screen particle mesh shader then hung at its next
// S_BARRIER), workgroup scope with =workgroup, none with =0. CPU: the emitted barrier; GPU (with
// `vulkan`): the intra-wave LDS exchange (a wave64 split over two lanes per invocation on a
// 32-wide host) reads its neighbours' writes at either scope.
void CheckLdsWaitcntBarrierScope(VulkanHarness *vulkan) {
  constexpr const char *name = "LdsWaitcntBarrierScope";
  using ShaderRecompiler::LdsWaitcntBarrier;
  auto test = LdsWaitcntOrdersIntraWaveExchange();
  test.ir_counts.clear(); // checked per mode below
  test.required_spirv.clear();
  struct Mode {
    LdsWaitcntBarrier mode;
    const char *label;
    const char *barrier; // AcquireRelease | WorkgroupMemory = 264
  };
  const Mode modes[] = {
      {LdsWaitcntBarrier::Subgroup, "subgroup", "OpMemoryBarrier %uint_3 %uint_264"},
      {LdsWaitcntBarrier::Workgroup, "workgroup", "OpMemoryBarrier %uint_2 %uint_264"},
      {LdsWaitcntBarrier::Off, "off", nullptr},
  };
  {
    // The default is subgroup scope.
    const ShaderRecompiler::CodegenOptions defaults;
    Require(name, "default", defaults.lds_waitcnt_barrier == LdsWaitcntBarrier::Subgroup,
            "the default LDS waitcnt barrier is not subgroup-scoped");
  }
  for (const auto &mode : modes) {
    auto options = GetCodegenOptions();
    options.lds_waitcnt_barrier = mode.mode;
    ScopedCodegenOptions scoped(options);
    const auto spirv = CompileCase(test, vulkan != nullptr ? vulkan->SubgroupSize() : 32u).spirv;
    ValidateSpirv(name, spirv);
    const auto barriers = CountSpirv(spirv, "OpMemoryBarrier");
    if (mode.barrier != nullptr) {
      Require(name, mode.label, barriers == 1u && CountSpirv(spirv, mode.barrier) == 1u,
              std::string("expected one ") + mode.barrier + ", found " + std::to_string(barriers) +
                  " OpMemoryBarrier");
    } else {
      Require(name, mode.label, barriers == 0u, "KYTY_LDS_WAITCNT_BARRIER=0 left a barrier");
    }
    Require(name, mode.label, CountSpirv(spirv, "OpControlBarrier") == 0u,
            "an LDS waitcnt became an execution barrier");
    if (vulkan != nullptr && mode.mode != LdsWaitcntBarrier::Off) {
      const auto actual = RunCollect(vulkan, test, test.expected.size());
      RequireEqualWords(name, mode.label, test.expected, actual);
    }
  }
  std::printf("[%s] %-32s ok\n", vulkan != nullptr ? "compute" : "codegen", name);
}

// Buffer ADD_TID outside compute (Astro Bot's performance patches select a vertex shader whose
// MUBUF loads set it; the emitter used to reject it). The index gets the lane id the stage's
// cross-lane ops use: the host subgroup lane, the wave's own 32 lanes under clusters. Without
// ADD_TID the lane id is not declared, so programs without it keep their SPIR-V.
std::vector<u32> CompileAddTidVertex(const char *name, const std::vector<u32> &code,
                                     const std::array<u32, 64> &data, u32 wave_size) {
  namespace IR = ShaderRecompiler::IR;
  const auto user_data = MakeNativeUserData(&data);
  ShaderVertexInputInfo vertex{};
  vertex.wave_size = wave_size;
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Vertex;
  options.dump_ir = false;
  options.wave_size = wave_size;
  options.input_info.vertex = &vertex;
  options.user_data_base = 8;
  options.user_data = user_data;
  auto translated = ShaderRecompiler::TranslateProgram(code, options);
  auto plan = IR::ExtractResourcePlan(translated.program);
  IR::ResourceSnapshot resources;
  IR::ResourceSpecialization specialization;
  IR::SrtRuntime runtime{.user_data = options.user_data,
                         .shader_base = reinterpret_cast<uint64_t>(code.data())};
  Require(name, "resource materialization",
          IR::MaterializeResources(plan, runtime, resources, specialization),
          "vertex resources could not be materialized");
  auto result = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization);
  Require(name, "SPIR-V emit", !result.spirv.empty(), "recompiler returned empty SPIR-V");
  ValidateSpirv(name, result.spirv);
  return std::move(result.spirv);
}

void CheckGraphicsBufferAddTid() {
  constexpr const char *name = "GraphicsBufferAddTid";
  // buffer_load_dword v0, off, s[n:n+3], 0 (index 0, plus the lane id with ADD_TID); export v0.
  // The V# is the first user data: s0 in a pixel shader, s8 in a vertex (NGG) shader, whose
  // s0-s7 are system registers.
  const auto make_code = [](u32 target, u32 srsrc) {
    std::vector<u32> code;
    code.push_back(EncodeMubuf0(0x0cu, 0, false, false));
    code.push_back(EncodeMubuf1(0, srsrc, 0));
    code.push_back(EncodeExp0(target, 0xfu));
    code.push_back(EncodeExp1(0, 0, 0, 0));
    AppendEnd(&code);
    return code;
  };
  const auto pixel = [&](bool add_tid, u32 wave_size) {
    GraphicsCase test;
    test.name = name;
    test.fragment_code = make_code(0x00u, 0u);
    test.user_data = MakeStructuredStorageBufferData(4, 64, add_tid);
    test.has_user_data = true;
    test.wave_size = wave_size;
    return CompileFragmentCase(test).spirv;
  };
  const auto vertex = [&](bool add_tid, u32 wave_size) {
    return CompileAddTidVertex(name, make_code(0x0cu, 2u),
                               MakeStructuredStorageBufferData(4, 64, add_tid), wave_size);
  };
  const auto lane_ids = [](const std::vector<u32> &spirv) {
    return CountSpirv(spirv, "BuiltIn SubgroupLocalInvocationId");
  };
  const Spirv::HostWaveClusters none{};
  for (const u32 wave_size : {64u, 32u}) {
    ScopedHostWaveClusters host(none);
    ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Auto));
    const auto label = "wave" + std::to_string(wave_size);
    for (const bool is_pixel : {true, false}) {
      const auto stage = std::string(is_pixel ? "pixel " : "vertex ") + label;
      const auto with = is_pixel ? pixel(true, wave_size) : vertex(true, wave_size);
      const auto without = is_pixel ? pixel(false, wave_size) : vertex(false, wave_size);
      Require(name, stage.c_str(), lane_ids(with) == 1u,
              "an ADD_TID load did not read the subgroup lane id");
      Require(name, stage.c_str(), lane_ids(without) == 0u && without != with,
              "a load without ADD_TID declared the subgroup lane id");
    }
  }
  // Clusters (wave32 on a 64-wide host): the lane is masked to the wave's own 32 lanes first.
  for (const bool is_pixel : {true, false}) {
    std::vector<u32> plain, clustered;
    {
      ScopedHostWaveClusters host(none);
      ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Off));
      plain = is_pixel ? pixel(true, 32u) : vertex(true, 32u);
    }
    {
      ScopedHostWaveClusters host(none);
      ScopedCodegenOptions scoped(WithClusters(Wave32Clusters::Force));
      clustered = is_pixel ? pixel(true, 32u) : vertex(true, 32u);
    }
    Require(name, is_pixel ? "pixel clusters" : "vertex clusters",
            lane_ids(clustered) == 1u &&
                CountSpirv(clustered, "OpBitwiseAnd") > CountSpirv(plain, "OpBitwiseAnd"),
            "forced clusters did not mask the ADD_TID lane to the wave's 32 lanes");
  }
  std::printf("[codegen] %-32s ok\n", name);
}

// KYTY_SPEC_UNMAPPED_NULL: a V# the renderer would bind as empty (its base is in no guest mapping,
// or base/records 0) specializes as null, so the garbage words a game leaves in an untaken
// branch's descriptor slot do not create a permutation per value (Astro Bot's [Performance]
// patches); a V# with a mapped base in the same slot specializes normally.
namespace UnmappedNull {
constexpr uint64_t MappedBase = 0x200000000ull, MappedSize = 0x100000ull;
// The renderer's probe: whether the base lies in a mapping (ClampRangeSizeQuiet != 0).
bool TestMapped(uint64_t address, uint64_t /*size*/) {
  return address >= MappedBase && address - MappedBase < MappedSize;
}
} // namespace UnmappedNull

void CheckUnmappedBufferNull() {
  constexpr const char *name = "UnmappedBufferNull";
  namespace IR = ShaderRecompiler::IR;
  // buffer_atomic_umax v4, v0, s[0:3] idxen (the Astro Bot vertex shader's access); end.
  std::vector<u32> code;
  code.push_back(EncodeMubuf0(0x38u /* BUFFER_ATOMIC_UMAX (GFX10) */, 0, true, false));
  code.push_back(EncodeMubuf1(4, 0, 0));
  AppendEnd(&code);
  ShaderPixelInputInfo pixel{};
  pixel.input_num = 1;
  pixel.ps_system_input_base = 2;
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Pixel;
  options.dump_ir = false;
  options.wave_size = 64;
  pixel.wave_size = 64;
  options.input_info.pixel = &pixel;
  std::array<u32, 64> user_data{};
  user_data[3] = DstSel(4, 5, 6, 7) | (1u << 24u);
  options.user_data = user_data;
  auto translated = ShaderRecompiler::TranslateProgram(code, options);
  const auto plan = IR::ExtractResourcePlan(translated.program);
  Require(name, "buffer", plan.info.buffers.size() == 1u, "the atomic has no tracked buffer");
  const auto materialize = [&](u32 w0, u32 w1, u32 w2, u32 w3, IR::ResourceSnapshot &resources) {
    user_data[0] = w0;
    user_data[1] = w1;
    user_data[2] = w2;
    user_data[3] = w3;
    IR::ResourceSpecialization specialization;
    const IR::SrtRuntime runtime{.user_data = user_data, .shader_base = reinterpret_cast<uint64_t>(code.data())};
    Require(name, "materialization", IR::MaterializeResources(plan, runtime, resources, specialization),
            "resources could not be materialized");
    return specialization.buffers.at(0);
  };
  const auto word3 = [](bool add_tid, u32 index_stride) {
    return DstSel(4, 5, 6, 7) | (1u << 24u) | (add_tid ? 1u << 23u : 0u) | (index_stride << 21u);
  };
  IR::SetUnmappedBufferProbe(UnmappedNull::TestMapped);
  // Garbage V#s (unmapped 48-bit bases, random strides, swizzle, ADD_TID): one specialization.
  std::set<std::tuple<u32, u32, bool>> garbage;
  u32 seed = 0x3f4ade30u;
  const auto next = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
  for (u32 i = 0; i < 64; i++) {
    IR::ResourceSnapshot resources;
    const u32 w0 = next(), w1 = (next() & 0xffff0000u) | 0x7000u /* base hi: far from the mapped range */;
    const auto b = materialize(w0, w1, next(), word3((i & 1u) != 0u, i & 3u), resources);
    garbage.insert({b.packed_stride, b.descriptor_swizzle, b.zero_stride_oob});
    Require(name, "garbage snapshot", resources.buffers.at(0).dwords[0] == 0u &&
            resources.buffers.at(0).dwords[3] == 0u, "a garbage V# kept its words for the binding");
  }
  Require(name, "garbage", garbage.size() == 1u && std::get<0>(*garbage.begin()) == 0u,
          "unmapped garbage V#s specialized into more than one permutation");
  // The same slot with a real, mapped V# (stride 4, ADD_TID): a normal specialization.
  {
    IR::ResourceSnapshot resources;
    const auto b = materialize(static_cast<u32>(UnmappedNull::MappedBase), (4u << 16u) | 2u, 64, word3(true, 0), resources);
    Require(name, "mapped", b.packed_stride == (4u | (1u << 20u)) && resources.buffers.at(0).dwords[1] != 0u,
            "a mapped V# was nulled");
  }
  // Edge cases. Nulled (the binding is empty either way): a low base (Astro Bot's walk froze
  // after a V# with base 0x107), an unmapped base whose range ends in mapped memory, an unmapped
  // raw V# with no records. Kept: a mapped base whose range runs past the mapping (the binding
  // clamps it), and everything with the switch off.
  {
    IR::ResourceSnapshot resources;
    const auto low = materialize(0x107u, 0u, 0x108u, word3(true, 0), resources);
    Require(name, "low base", low.packed_stride == 0u && resources.buffers.at(0).dwords[0] == 0u,
            "an unmapped low-base V# was not nulled");
    const u32 below = static_cast<u32>(UnmappedNull::MappedBase - 0x1000u);
    const auto spans = materialize(below, (16u << 16u) | 1u, 0x1000u, word3(false, 0), resources);
    Require(name, "spanning range", spans.packed_stride == 0u, "an unmapped base reaching mapped memory was kept");
    const auto raw = materialize(0x12340000u, 0x00007000u, 0, word3(false, 0), resources);
    Require(name, "raw records 0", raw.packed_stride == 0u && resources.buffers.at(0).dwords[0] == 0u,
            "an unmapped raw V# with no records was not nulled");
    const auto past = materialize(static_cast<u32>(UnmappedNull::MappedBase + UnmappedNull::MappedSize - 0x100u),
                                  (8u << 16u) | 2u, 0x10000u, word3(false, 0), resources);
    Require(name, "mapped base, long range", past.packed_stride == 8u, "a V# with a mapped base was nulled");
    IR::SetUnmappedBufferProbe(nullptr);
    const auto off = materialize(0x12340000u, (12u << 16u) | 0x7000u, 64, word3(false, 0), resources);
    Require(name, "switch off", off.packed_stride == 12u, "the switch off still nulled a V#");
  }
  std::printf("[host]    %-32s ok\n", name);
}

} // namespace WaveClusterTests
