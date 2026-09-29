"""Opcode histogram of a SPIR-V binary: python spv_hist.py FILE.spv [TOP]"""
import struct, sys, collections
NAMES = {1:"Undef",5:"Name",6:"MemberName",15:"EntryPoint",17:"Capability",19:"TypeVoid",21:"TypeInt",22:"TypeFloat",
 23:"TypeVector",32:"TypePointer",33:"TypeFunction",43:"Constant",44:"ConstantComposite",54:"Function",55:"FunctionParameter",56:"FunctionEnd",
 57:"FunctionCall",59:"Variable",61:"Load",62:"Store",65:"AccessChain",71:"Decorate",72:"MemberDecorate",77:"VectorExtractDynamic",
 79:"VectorShuffle",80:"CompositeConstruct",81:"CompositeExtract",82:"CompositeInsert",86:"SampledImage",87:"ImageSampleImplicitLod",
 88:"ImageSampleExplicitLod",95:"ImageFetch",96:"ImageGather",98:"ImageRead",99:"ImageWrite",100:"Image",103:"ImageQuerySizeLod",
 109:"ConvertFToU",110:"ConvertFToS",111:"ConvertSToF",112:"ConvertUToF",113:"UConvert",114:"SConvert",115:"FConvert",124:"Bitcast",
 126:"SNegate",127:"FNegate",128:"IAdd",129:"FAdd",130:"ISub",131:"FSub",132:"IMul",133:"FMul",134:"UDiv",135:"SDiv",136:"FDiv",
 137:"UMod",141:"FMod",142:"VectorTimesScalar",148:"Dot",149:"IAddCarry",150:"ISubBorrow",151:"UMulExtended",152:"SMulExtended",
 154:"Any",155:"All",156:"IsNan",157:"IsInf",164:"LogicalEqual",165:"LogicalNotEqual",166:"LogicalOr",167:"LogicalAnd",168:"LogicalNot",169:"Select",
 170:"IEqual",171:"INotEqual",172:"UGreaterThan",173:"SGreaterThan",174:"UGreaterThanEqual",175:"SGreaterThanEqual",176:"ULessThan",
 177:"SLessThan",178:"ULessThanEqual",179:"SLessThanEqual",180:"FOrdEqual",181:"FUnordEqual",182:"FOrdNotEqual",183:"FUnordNotEqual",
 184:"FOrdLessThan",185:"FUnordLessThan",186:"FOrdGreaterThan",187:"FUnordGreaterThan",188:"FOrdLessThanEqual",190:"FOrdGreaterThanEqual",
 194:"ShiftRightLogical",195:"ShiftRightArithmetic",196:"ShiftLeftLogical",197:"BitwiseOr",198:"BitwiseXor",199:"BitwiseAnd",200:"Not",
 201:"BitFieldInsert",202:"BitFieldSExtract",203:"BitFieldUExtract",204:"BitReverse",205:"BitCount",207:"DPdx",208:"DPdy",
 227:"AtomicLoad",228:"AtomicStore",229:"AtomicExchange",232:"AtomicIIncrement",234:"AtomicIAdd",241:"AtomicUMax",242:"AtomicAnd",243:"AtomicOr",
 245:"Phi",246:"LoopMerge",247:"SelectionMerge",248:"Label",249:"Branch",250:"BranchConditional",251:"Switch",252:"Kill",253:"Return",254:"ReturnValue",255:"Unreachable",
 12:"ExtInst",224:"ControlBarrier",225:"MemoryBarrier",4421:"SubgroupBallotKHR",4422:"SubgroupFirstInvocationKHR",333:"GroupNonUniformElect",
 334:"GroupNonUniformAll",335:"GroupNonUniformAny",337:"GroupNonUniformBroadcast",338:"GroupNonUniformBroadcastFirst",339:"GroupNonUniformBallot",
 341:"GroupNonUniformBallotBitExtract",342:"GroupNonUniformBallotBitCount",343:"GroupNonUniformBallotFindLSB",345:"GroupNonUniformShuffle",
 346:"GroupNonUniformShuffleXor",349:"GroupNonUniformIAdd",5341:"TypeAccelerationStructure",4450:"PtrAccessChain" if False else "?"}
d = open(sys.argv[1], "rb").read()
w = struct.unpack("<%dI" % (len(d) // 4), d)
i = 5
h = collections.Counter(); hw = collections.Counter(); funcs = 0; labels = 0
while i < len(w):
    n, op = w[i] >> 16, w[i] & 0xffff
    if n == 0: break
    h[op] += 1; hw[op] += n
    i += n
tot = sum(hw.values())
print(f"{sys.argv[1].split('/')[-1]}: {len(w)} words, {sum(h.values())} instructions, functions={h[54]} labels={h[248]} calls={h[57]}")
for op, c in hw.most_common(int(sys.argv[2]) if len(sys.argv) > 2 else 25):
    print(f"  {NAMES.get(op, op)!s:28} n={h[op]:7d} words={c:7d} {100*c/tot:5.1f}%")
