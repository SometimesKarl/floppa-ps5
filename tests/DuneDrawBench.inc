// Included inside VulkanHarness (ShaderRecompilerComputeTests.cpp).
//
// Dune draw benchmark (`--dune-draw-bench [draws]`). ASTRO BOT's dune issues ~14k small draws a
// second and the GPU command thread's CPU time per draw bounds the frame rate there
// (perf-handoff/FINDINGS.md, X52). This records many small draws through the whole draw path
// (RenderExecutor::DrawAuto: program lookup and resource walk, bindings, texture and buffer
// lookups, pipeline lookup, Vulkan recording) with the dune's shape: a pixel shader that loads two
// texture descriptors and a sampler through a pointer in user data, and a per-draw constant buffer
// through a second pointer that changes every draw. It prints the CPU time per draw (lavapipe's
// recording included); run it under callgrind for a per-function breakdown of the emulator's part.
void RunDuneDrawBench(uint32_t draws) {
  constexpr const char *name = "DuneDrawBench";
  constexpr uintptr_t base = 0x0000000206000000ull;
  constexpr uint64_t allocation_size = 0x100000;
  constexpr uint32_t extent = 64;
  constexpr uint32_t texture_count = 2;
  constexpr uint32_t draw_tables = 64;
  constexpr uint64_t target_address = base;              // 64x64 RGBA8
  constexpr uint64_t texture_address = base + 0x10000;   // 64x64 RGBA8 each, 0x4000 apart
  constexpr uint64_t srt_address = base + 0x20000;       // T# at +0x00/+0x20, S# at +0x80
  constexpr uint64_t table_address = base + 0x30000;     // per-draw V#, 16 bytes each
  constexpr uint64_t constants_address = base + 0x40000; // per-draw constants, 256 bytes each
  EnsureRuntimeContext();

  int64_t direct_offset = -1;
  Require(name, "direct allocation",
          Libs::LibKernel::Memory::KernelAllocateDirectMemory(
              0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size,
              0x10000, 0, &direct_offset) == 0,
          "benchmark allocation failed");
  void *mapped = reinterpret_cast<void *>(base);
  Require(name, "direct mapping",
          Libs::LibKernel::Memory::KernelMapDirectMemory(&mapped, allocation_size, 0x3, 0x10,
                                                         direct_offset, 0x10000) == 0 &&
              mapped == reinterpret_cast<void *>(base),
          "benchmark mapping failed");
  std::memset(mapped, 0, allocation_size);

  // Texture t holds 0x20 * (t + 1) in every channel; the pixel shader adds both and scales the
  // sum by the draw's constant.
  for (uint32_t t = 0; t < texture_count; t++) {
    auto *texels = reinterpret_cast<u32 *>(texture_address + t * 0x4000);
    const u32 channel = 0x20u * (t + 1u);
    for (uint32_t i = 0; i < extent * extent; i++) {
      texels[i] = channel * 0x01010101u;
    }
  }
  auto *srt = reinterpret_cast<u32 *>(srt_address);
  for (uint32_t t = 0; t < texture_count; t++) {
    const ShaderTextureResource texture{{
        static_cast<u32>((texture_address + t * 0x4000) >> 8u),
        (static_cast<u32>(Prospero::BufferFormat::k8_8_8_8UNorm) << 20u) |
            (((extent - 1u) & 3u) << 30u),
        ((extent - 1u) >> 2u) | ((extent - 1u) << 14u),
        DstSel(4, 5, 6, 7) | (static_cast<u32>(Prospero::ImageType::kColor2D) << 28u),
        0, 0, 0, 0}};
    std::copy_n(texture.fields, 8, srt + t * 8u);
  }
  auto *tables = reinterpret_cast<u32 *>(table_address);
  for (uint32_t i = 0; i < draw_tables; i++) {
    const uint64_t constants = constants_address + i * 256u;
    const ShaderBufferResource buffer{{
        static_cast<u32>(constants), static_cast<u32>(constants >> 32u), 256,
        DstSel(4, 5, 6, 7) | (static_cast<u32>(Prospero::BufferFormat::k32Float) << 12u)}};
    std::copy_n(buffer.fields, 4, tables + i * 4u);
    *reinterpret_cast<float *>(constants) = 0.5f + static_cast<float>(i) / 256.0f;
  }

  // As the emulator's device setup (vulkanWindow.cpp): draws with few descriptors use push
  // descriptors. The harness context leaves the limit at 0 for the other tests.
  vk::PhysicalDevicePushDescriptorPropertiesKHR push_properties{};
  vk::PhysicalDeviceProperties2 properties{};
  properties.pNext = &push_properties;
  m_physical_device.getProperties2(&properties);
  const auto saved_push_limit = m_runtime_context.max_push_descriptors;
  m_runtime_context.max_push_descriptors = push_properties.maxPushDescriptors;
  struct RestorePushLimit {
    GraphicContext &context;
    uint32_t value;
    ~RestorePushLimit() { context.max_push_descriptors = value; }
  } restore_push_limit{m_runtime_context, saved_push_limit};

  RenderContext context(m_runtime_context);
  // The emulator's guest-memory routines consult this renderer (GPU-dirty pages, direct reads).
  LibKernel::Memory::InstallGpuResources(&context);
  struct UninstallGpuResources {
    ~UninstallGpuResources() { LibKernel::Memory::InstallGpuResources(nullptr); }
  } uninstall_gpu_resources;
  auto &scheduler = context.GetCommandScheduler();
  auto &executor = context.GetRenderExecutor();
  auto &cache = context.GetTextureCache();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  registers.SetViewportTransformControl(0x300);
  registers.SetViewportScaleOffset(0, extent / 2, extent / 2, extent / 2, extent / 2, 1, 0);
  registers.SetViewportZMax(0, 1);
  registers.SetScreenScissor(0, 0, extent, extent);
  registers.SetWindowScissor(0, 0, extent, extent, false);
  registers.SetGenericScissor(0, 0, extent, extent, false);
  registers.SetViewportScissor(0, 0, 0, extent, extent, false);
  registers.SetRenderTargetMask(0xf);
  registers.SetShaderMask(0xf);
  registers.SetColorBase(0, {.addr = target_address});
  registers.SetColorInfo(0, {.format = Prospero::ChannelLayout::k8_8_8_8,
                             .channel_type = Prospero::ChannelType::kUNorm,
                             .channel_order = Prospero::ChannelOrder::kStandard});
  registers.SetColorAttrib2(0, {.height = extent - 1, .width = extent - 1});
  registers.SetColorAttrib3(0, {.tile_mode = Prospero::TileMode::kLinear, .dimension = 1});
  registers.SetTargetOutputMode(0, 4);
  registers.SetPsInControl(0x8001);
  registers.SetPsInputEna(2);
  registers.SetPsInputAddr(2);
  registers.SetPsInputSettings(0, 0);
  user_config.SetPrimitiveType(Prospero::PrimitiveType::kTriList);
  scheduler.Begin(registers, user_config, shaders);
  context.MapMemory(base, allocation_size);

  // Fullscreen triangle from the vertex index (v5), parameter 0 = (0, 0, 0, 1).
  static const auto vertex_code = [] {
    std::vector<u32> code;
    AppendVMovLiteral(&code, 1, 0xbf800000u);
    AppendVMovLiteral(&code, 2, 0x40400000u);
    code.push_back(EncodeVopc(0xc2, InlineU32(1), 5));
    code.push_back(EncodeVop2(0x01, 3, Vgpr(1), 2));
    code.push_back(EncodeVopc(0xc2, InlineU32(2), 5));
    code.push_back(EncodeVop2(0x01, 4, Vgpr(1), 2));
    AppendVMovU32(&code, 0, 0);
    AppendVMovLiteral(&code, 6, 0x3f800000u);
    code.push_back(EncodeExp0(0x0c, 0xf));
    code.push_back(EncodeExp1(3, 4, 0, 6));
    code.push_back(EncodeExp0(0x20, 0xf));
    code.push_back(EncodeExp1(0, 0, 0, 6));
    AppendEnd(&code);
    return code;
  }();
  // s[0:1]: resource table, s[2:3]: this draw's table. SOFFSET 125 (null): no register offset.
  static const auto pixel_code = [] {
    std::vector<u32> code;
    code.push_back(EncodeSmem0(0x03, 8, 0));  // s_load_dwordx8 s[8:15], s[0:1], 0x00
    code.push_back(EncodeSmem1(0x00, 125));
    code.push_back(EncodeSmem0(0x03, 16, 0)); // s_load_dwordx8 s[16:23], s[0:1], 0x20
    code.push_back(EncodeSmem1(0x20, 125));
    code.push_back(EncodeSmem0(0x02, 24, 0)); // s_load_dwordx4 s[24:27], s[0:1], 0x80
    code.push_back(EncodeSmem1(0x80, 125));
    code.push_back(EncodeSmem0(0x02, 28, 1)); // s_load_dwordx4 s[28:31], s[2:3], 0x00
    code.push_back(EncodeSmem1(0x00, 125));
    code.push_back(EncodeSopp(0x0c, 0xc07f)); // s_waitcnt lgkmcnt(0)
    code.push_back(EncodeSmem0(0x08, 32, 14)); // s_buffer_load_dword s32, s[28:31], 0
    code.push_back(EncodeSmem1(0x00, 125));
    for (uint32_t component = 0; component < 2; component++) {
      code.push_back(EncodeVintrp(0, 20 + component, 0, component, 0));
      code.push_back(EncodeVintrp(1, 20 + component, 0, component, 1));
    }
    code.push_back(EncodeMimg0(0x27, 0xf)); // image_sample_lz v[0:3], v[20:21], s[8:15], s[24:27]
    code.push_back(EncodeMimg1(0, 20, 2, 6));
    code.push_back(EncodeMimg0(0x27, 0xf)); // image_sample_lz v[4:7], v[20:21], s[16:23], s[24:27]
    code.push_back(EncodeMimg1(4, 20, 4, 6));
    code.push_back(EncodeSopp(0x0c, 0x3f70)); // s_waitcnt vmcnt(0)
    for (uint32_t component = 0; component < 4; component++) {
      code.push_back(EncodeVop2(0x03, component, Vgpr(component), 4 + component)); // v_add_f32
      code.push_back(EncodeVop2(0x08, component, 32, component));                   // v_mul_f32
    }
    code.push_back(EncodeExp0(0x00, 0xf));
    code.push_back(EncodeExp1(0, 1, 2, 3));
    AppendEnd(&code);
    return code;
  }();
  static ShaderUserData vertex_user_data{};
  const auto vertex_address = reinterpret_cast<uint64_t>(vertex_code.data());
  const auto pixel_address = reinterpret_cast<uint64_t>(pixel_code.data());
  ShaderMapUserData(vertex_address,
                    {.type = Prospero::ShaderBinaryType::kGs,
                     .user_data = &vertex_user_data,
                     .code_size_bytes = static_cast<u32>(vertex_code.size() * sizeof(u32))});
  ShaderMapUserData(pixel_address,
                    {.type = Prospero::ShaderBinaryType::kPs,
                     .code_size_bytes = static_cast<u32>(pixel_code.size() * sizeof(u32))});
  shaders.SetEsShaderBase(vertex_address);
  shaders.SetPsShaderBase(pixel_address);
  shaders.SetPsShaderResource2({.user_sgpr = 4});
  shaders.SetPsUserSgpr(0, static_cast<u32>(srt_address), HW::UserSgprType::Unknown);
  shaders.SetPsUserSgpr(1, static_cast<u32>(srt_address >> 32u), HW::UserSgprType::Unknown);
  const auto set_draw_table = [&](uint32_t draw) {
    const uint64_t table = table_address + (draw % draw_tables) * 16u;
    shaders.SetPsUserSgpr(2, static_cast<u32>(table), HW::UserSgprType::Unknown);
    shaders.SetPsUserSgpr(3, static_cast<u32>(table >> 32u), HW::UserSgprType::Unknown);
  };

  // Draws run on a GPU command thread, as in the emulator: guest reads there take the direct
  // path (TryReadGuestWithoutFault) instead of the locked backing read other threads use.
  context.InitializeGpu(nullptr);
  context.GetGpu().SendCommandSync([&] {
    // First draw: translates both shaders and builds the pipeline; checks the result.
    set_draw_table(0);
    RenderExecutorTestAccess::DrawAuto(executor, scheduler.Current(),
                                       {.vertex_count = 3, .instance_count = 1});
    scheduler.Finish();
    RenderColorInfo color{};
    RenderExecutorTestAccess::ResolveRenderColorTarget(executor, scheduler.Current(), color, 0);
    const auto pixels = ReadCachedTexel(name, context, color.image_id, {}, {extent, extent, 1});
    // (0x20 + 0x40) / 255 * 0.5 in every channel -> 0x30.
    const bool drawn = !pixels.empty() && std::ranges::all_of(pixels, [](u32 texel) {
      for (uint32_t shift = 0; shift < 32; shift += 8) {
        const auto channel = static_cast<int>((texel >> shift) & 0xffu);
        if (std::abs(channel - 0x30) > 1) {
          return false;
        }
      }
      return true;
    });
    Require(name, "first draw output", drawn,
            "the benchmark draw did not produce (tex0 + tex1) * constant");

    // Recording only: the time inside DrawAuto (submission and lavapipe's rendering excluded).
    const auto recording = DuneDrawBenchRecord(executor, scheduler, draws, set_draw_table);
    const auto begin     = std::chrono::steady_clock::now();
    scheduler.Finish();
    const auto finish =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
    std::printf("[gpu]     %-32s %u draws, %.2f us per draw recorded (DrawAuto), then %.0f us "
                "until lavapipe finished\n",
                name, draws, recording / static_cast<double>(std::max(draws, 1u)), finish);
  });
  (void)cache;
}

// The measured loop, apart so `valgrind --tool=callgrind --toggle-collect='*DuneDrawBenchRecord*'`
// profiles only the draws after the first.
template <typename SetTable>
[[gnu::noinline]] double DuneDrawBenchRecord(RenderExecutor &executor, CommandScheduler &scheduler,
                                             uint32_t draws, const SetTable &set_draw_table) {
  double recording = 0;
  for (uint32_t draw = 1; draw <= draws; draw++) {
    set_draw_table(draw);
    const auto begin = std::chrono::steady_clock::now();
    RenderExecutorTestAccess::DrawAuto(executor, scheduler.Current(),
                                       {.vertex_count = 3, .instance_count = 1});
    recording +=
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
    if ((draw & 1023u) == 0u) {
      scheduler.Flush();
    }
  }
  return recording;
}
