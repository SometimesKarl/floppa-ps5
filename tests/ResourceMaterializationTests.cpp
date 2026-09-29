#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <vector>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ResourceMaterializationTests: failed: %s\n", text);
    std::abort();
  }
}

bool RejectSpecializationRead(void *userdata, uint64_t, std::span<uint32_t>) {
  ++*static_cast<uint32_t *>(userdata);
  return false;
}

Libs::Graphics::ShaderRecompiler::IR::Block &
AddValueBlock(Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto block = std::make_unique<Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  program.block_info.push_back({.id = 0});
  program.block_storage.push_back(std::move(block));
  return *result;
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan SrtPlan(uint64_t address) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  program.memory_info.push_back(memory);
  const auto low = Value(static_cast<uint32_t>(address));
  const auto high = Value(static_cast<uint32_t>(address >> 32u));
  auto &handle =
      value_block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high});
  auto &raw = value_block.AppendNewInst(
      ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});

  auto &srt = value_block.AppendNewInst(ValueOpcode::GetSrtResource);
  auto &flat = value_block.AppendNewInst(ValueOpcode::ReadConst,
                                         {Value(&srt), Value(0u)});
  DescriptorSource source;
  source.dwords[0] = Value(&flat);
  source.dwords[1] = Value(0u);
  source.dword_count = 2;
  program.descriptor_sources.push_back(source);
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UnbasedFlatPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);
  program.info.uses_dma = true;
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UserDataBufferPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  auto &user_data = value_block.AppendNewInst(
      ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  DescriptorSource source;
  source.dwords[0] = Value(&user_data);
  source.dwords[1] = Value(0u);
  source.dwords[2] = Value(0u);
  source.dwords[3] = Value(0u);
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MixedSamplerPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);

  const auto AddSource = [&program](uint32_t dword_count, uint32_t first) {
    DescriptorSource source;
    source.dword_count = dword_count;
    source.dwords[0] = Value(first);
    for (uint32_t i = 1; i < dword_count; i++) {
      source.dwords[i] = Value(0u);
    }
    program.descriptor_sources.push_back(source);
    return static_cast<uint32_t>(program.descriptor_sources.size() - 1u);
  };

  const auto image0 = AddSource(8, 0);
  const auto image1 = AddSource(8, 0);
  const auto sampler0 = AddSource(4, 0x11111111u);
  const auto sampler1 = AddSource(4, 0x22222222u);
  program.info.images.push_back(
      {.source = image0,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.images.push_back(
      {.source = image1,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D,
       .conversion_format =
           Libs::Graphics::Prospero::BufferFormat::k8_8_8_8UNorm});
  program.info.samplers.push_back({.source = sampler0});
  program.info.samplers.push_back({.source = sampler1});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 0});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 1});
  program.info.sampled_pairs.push_back({.image = 1, .sampler = 1});
  return ExtractResourcePlan(program);
}

// Synthetic guest memory read by both walkers (strict and observed). Every address read is logged
// so a test can prove that a pointer under an untaken branch was never dereferenced: in the
// emulator an unreached pointer chain can point at unmapped memory and fault.
struct FakeMemory {
  std::map<uint64_t, uint32_t> words;
  std::vector<uint64_t> reads;
};

bool FakeRead(void *userdata, uint64_t address, std::span<uint32_t> values) {
  auto *memory = static_cast<FakeMemory *>(userdata);
  for (size_t i = 0; i < values.size(); i++) {
    const uint64_t word_address = address + i * sizeof(uint32_t);
    memory->reads.push_back(word_address);
    const auto found = memory->words.find(word_address);
    if (found == memory->words.end()) {
      return false;
    }
    values[i] = found->second;
  }
  return true;
}

Libs::Graphics::ShaderRecompiler::IR::Inst &
AppendLoad(Libs::Graphics::ShaderRecompiler::IR::Block &block, uint64_t address) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto &handle = block.AppendNewInst(
      ValueOpcode::GetAddressResource,
      {Value(static_cast<uint32_t>(address)),
       Value(static_cast<uint32_t>(address >> 32u))});
  auto &raw = block.AppendNewInst(
      ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x80});
  return raw;
}

// Two buffers whose first descriptor word is loaded from guest memory, each used only in one
// arm of a branch on a guest flag word: block 0 branches on flag != 0 to block 1 (buffer 0) or
// block 2 (buffer 1). `conditions` owns the condition instructions and must outlive the plan.
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan
BranchPlan(uint64_t flag_address, uint64_t taken_address,
           uint64_t untaken_address,
           Libs::Graphics::ShaderRecompiler::IR::Block &conditions) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);
  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  program.memory_info.push_back(memory);
  for (const auto address : {taken_address, untaken_address}) {
    DescriptorSource source;
    source.dwords[0] = Value(&AppendLoad(value_block, address));
    for (uint32_t i = 1; i < 4; i++) {
      source.dwords[i] = Value(0u);
    }
    source.dword_count = 4;
    program.descriptor_sources.push_back(source);
  }
  program.info.buffers.push_back({.source = 0});
  program.info.buffers.push_back({.source = 1});
  auto plan = ExtractResourcePlan(program);

  auto &flag = AppendLoad(conditions, flag_address);
  auto &condition = conditions.AppendNewInst(ValueOpcode::INotEqual32,
                                             {Value(&flag), Value(0u)});
  plan.control_flow.clear();
  plan.control_flow.push_back({.condition = Value(&condition), .successors = {1, 2}});
  plan.control_flow.push_back({.sources = {0}});
  plan.control_flow.push_back({.sources = {1}});
  return plan;
}

struct WalkResult {
  bool ok = false;
  Libs::Graphics::ShaderRecompiler::IR::ResourceSnapshot snapshot;
  Libs::Graphics::ShaderRecompiler::IR::ResourceSpecialization specialization;
  std::vector<uint64_t> reads;
};

WalkResult Walk(const Libs::Graphics::ShaderRecompiler::IR::ResourcePlan &plan,
                FakeMemory &memory, bool fast) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  SetSrtFastEvaluation(fast);
  memory.reads.clear();
  const SrtRuntime runtime{.read_memory = FakeRead,
                           .userdata = &memory,
                           .read_specialization_memory = FakeRead};
  WalkResult result;
  result.ok = MaterializeResources(plan, runtime, result.snapshot,
                                   result.specialization);
  result.reads = memory.reads;
  SetSrtFastEvaluation(true);
  return result;
}

bool SameResult(const WalkResult &a, const WalkResult &b) {
  return a.ok == b.ok &&
         (!a.ok || (a.snapshot.buffers == b.snapshot.buffers &&
                    a.snapshot.images == b.snapshot.images &&
                    a.snapshot.samplers == b.snapshot.samplers &&
                    a.snapshot.flattened_srt == b.snapshot.flattened_srt &&
                    a.specialization == b.specialization));
}

bool ReadAddress(const WalkResult &result, uint64_t address) {
  return std::ranges::find(result.reads, address) != result.reads.end();
}

constexpr uint64_t FlagAddress = 0x100000;
constexpr uint64_t TakenAddress = 0x200000;
constexpr uint64_t UntakenAddress = 0x300000;

void TestUntakenBranchPointerIsNotRead() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Block conditions;
  const auto plan =
      BranchPlan(FlagAddress, TakenAddress, UntakenAddress, conditions);
  FakeMemory memory;
  memory.words[FlagAddress] = 1;
  memory.words[TakenAddress] = 0xaaaa0001u;
  // UntakenAddress stays unmapped: reading it fails the walk.
  for (const bool fast : {true, false}) {
    const auto result = Walk(plan, memory, fast);
    Check(result.ok, "walk with an unmapped pointer on the untaken branch failed");
    Check(!ReadAddress(result, UntakenAddress),
          "walk read a pointer on the untaken branch");
    Check(result.snapshot.buffers.size() == 2 &&
              result.snapshot.buffers[0].dwords[0] == 0xaaaa0001u,
          "taken-branch descriptor has the wrong value");
    Check(result.snapshot.buffers[1] ==
              DescriptorValue{.dwords = {}, .dword_count = 4},
          "untaken-branch descriptor is not the inactive (zero) value");
  }
  Check(SameResult(Walk(plan, memory, true), Walk(plan, memory, false)),
        "decoded and interpreted walks differ on the branch plan");
}

void TestTakenBranchReadFailureFailsTheWalk() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Block conditions;
  const auto plan =
      BranchPlan(FlagAddress, TakenAddress, UntakenAddress, conditions);
  FakeMemory memory;
  memory.words[FlagAddress] = 0; // takes the arm whose pointer is unmapped
  memory.words[TakenAddress] = 0xaaaa0001u;
  for (const bool fast : {true, false}) {
    const auto result = Walk(plan, memory, fast);
    Check(!result.ok, "a failed read on the taken branch did not fail the walk");
    Check(!ReadAddress(result, TakenAddress),
          "walk read the pointer of the branch it did not take");
  }
  // An unreadable condition cannot decide the branch: both arms are active.
  memory.words.erase(FlagAddress);
  memory.words[UntakenAddress] = 0xbbbb0002u;
  for (const bool fast : {true, false}) {
    const auto result = Walk(plan, memory, fast);
    Check(result.ok &&
              result.snapshot.buffers[0].dwords[0] == 0xaaaa0001u &&
              result.snapshot.buffers[1].dwords[0] == 0xbbbb0002u,
          "an undecided branch did not keep both arms active");
  }
}

// The decoded-node evaluator memoizes per walk (generations) and keeps decoded roots per plan.
// Alternating it with the interpreter on one plan while guest memory and the branch change
// between walks must never return a value from an earlier walk (KYTY_SRT_VERIFY, fixed input).
void TestRepeatedWalksFollowMemory() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Block conditions;
  const auto plan =
      BranchPlan(FlagAddress, TakenAddress, UntakenAddress, conditions);
  FakeMemory memory;
  std::mt19937 random(0x5eed1234u);
  constexpr int Walks = 2000;
  for (int walk = 0; walk < Walks; walk++) {
    const uint32_t flag = random() & 1u;
    const uint32_t taken = static_cast<uint32_t>(random());
    const uint32_t untaken = static_cast<uint32_t>(random());
    memory.words[FlagAddress] = flag;
    memory.words[TakenAddress] = taken;
    memory.words[UntakenAddress] = untaken;
    const auto fast = Walk(plan, memory, true);
    const auto interpreted = Walk(plan, memory, false);
    if (!fast.ok || !SameResult(fast, interpreted)) {
      std::fprintf(stderr, "ResourceMaterializationTests: walk %d (seed 0x5eed1234)\n",
                   walk);
      Check(false, "decoded and interpreted walks differ after memory changed");
    }
    const auto expected0 = flag != 0 ? taken : 0u;
    const auto expected1 = flag != 0 ? 0u : untaken;
    if (fast.snapshot.buffers[0].dwords[0] != expected0 ||
        fast.snapshot.buffers[1].dwords[0] != expected1) {
      std::fprintf(stderr, "ResourceMaterializationTests: walk %d (seed 0x5eed1234)\n",
                   walk);
      Check(false, "walk returned a stale or wrong descriptor word");
    }
  }
  std::printf("ResourceMaterializationTests: %d differential walks matched\n", Walks);
}

void TestMappedSrtUsesDirectReaderByDefault() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const uint32_t dword = 0x12345678;
  auto plan = SrtPlan(reinterpret_cast<uint64_t>(&dword));
  uint32_t specialization_reads = 0;
  const SrtRuntime runtime{.userdata = &specialization_reads,
                           .read_specialization_memory =
                               RejectSpecializationRead};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mapped SRT stage materialization failed");
  Check(specialization_reads == 0,
        "ordinary SRT read used the specialization reader");
  Check(snapshot.flattened_srt.size() == 1 &&
            snapshot.flattened_srt[0] == dword,
        "cache rematerialization did not use the direct reader by default");
}

void TestIntegerRuntimeValueFollowsSrtReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = SrtPlan(0x10000);
  const auto root = plan.descriptor_sources.front().dwords[0];
  Check(ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT read was rejected");

  Block values;
  auto &comparison = values.AppendNewInst(ValueOpcode::FPOrdLessThanEqual32,
                                          {Value::F32(1.f), Value::F32(0.f)});
  auto &selection = values.AppendNewInst(
      ValueOpcode::SelectU32, {Value(&comparison), Value(1u), Value(0u)});
  plan.srt_reads[0].value = Value(&selection);
  Check(ValidateRuntimeValue(plan, root),
        "ordinary SRT validation rejected a floating-point dependency");
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT validation missed a hidden floating-point dependency");

  auto &first =
      values.AppendNewInst(ValueOpcode::ReadFirstLane, {root, Value(true)});
  Check(!ValidateRuntimeValue(plan, Value(&first), RuntimeValueType::Integer),
        "read-first-lane lost integer-only SRT validation");

  auto &active = values.AppendNewInst(ValueOpcode::ReadFirstLane,
                                      {Value(&selection), Value(&comparison)});
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point execution mask was accepted as integer-only");

  auto &lane = values.AppendNewInst(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)),
       Value(0u)});
  auto &mask =
      values.AppendNewInst(ValueOpcode::INotEqual32, {Value(&lane), Value(0u)});
  selection.SetArg(0, Value(&mask));
  active.SetArg(1, Value(&mask));
  Check(ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "nonuniform integer execution mask was rejected");
  auto &float_value =
      values.AppendNewInst(ValueOpcode::BitCastU32F32, {Value::F32(1.f)});
  selection.SetArg(2, Value(&float_value));
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point inactive arm was accepted as integer-only");

  plan.srt_reads[0].value = Value(&first);
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "cyclic SRT read-first-lane dependency was accepted");
}

void TestUnbasedFlatCacheHitMaterializes() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UnbasedFlatPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "unbased FLAT stage materialization failed");
  Check(snapshot.buffers.empty() && snapshot.images.empty(),
        "unbased FLAT plan produced unexpected descriptors");
}

void TestFailedMaterializationRejectsStage() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UserDataBufferPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(!MaterializeResources(plan, {}, snapshot, specialization),
        "missing runtime user data did not reject the cached stage");
}

void TestMixedSamplerDuplicatesTheCorrectSnapshot() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = MixedSamplerPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "mixed sampler materialization failed");
  Check(snapshot.samplers.size() == 3,
        "mixed sampler materialization appended unrelated samplers");
  Check(snapshot.samplers[2] == snapshot.samplers[1] &&
            snapshot.samplers[2] != snapshot.samplers[0],
        "point sampler variant duplicated the wrong runtime descriptor");
}

} // namespace

namespace Common {

int DbgExitHandler(const char *, int, std::string_view) { std::abort(); }

int DbgExitHandler(const char *, int, fmt::text_style, std::string_view) {
  std::abort();
}

int DbgExitIfHandler(const char *, const char *, int) { return 1; }

void DbgExit(int) { std::abort(); }

} // namespace Common

int main() {
  TestMappedSrtUsesDirectReaderByDefault();
  TestIntegerRuntimeValueFollowsSrtReads();
  TestUnbasedFlatCacheHitMaterializes();
  TestFailedMaterializationRejectsStage();
  TestMixedSamplerDuplicatesTheCorrectSnapshot();
  TestUntakenBranchPointerIsNotRead();
  TestTakenBranchReadFailureFailsTheWalk();
  TestRepeatedWalksFollowMemory();
  std::puts("ResourceMaterializationTests: all cases passed");
  return 0;
}

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
