"""Experiment: rewrites KytyPS5's bounds-guarded buffer loads in a SPIR-V disassembly into
branchless form, to measure what the branches cost the driver compiler (pipebench --asm).

  %c = OpULessThan %bool %i %len          %c = OpULessThan %bool %i %len
       OpSelectionMerge %m None     ->    %p = OpAccessChain ... %i
       OpBranchConditional %c %t %m       %v = OpLoad %uint %p
  %t = OpLabel                            %r = OpSelect %uint %c %v %uint_0
  %p = OpAccessChain ... %i
  %v = OpLoad %uint %p
       OpBranch %m
  %m = OpLabel
  %r = OpPhi %uint %v %t %uint_0 %h

The unguarded load may read out of bounds: valid with robustBufferAccess (always enabled by the
emulator), and the select discards the value. Later phis naming %m as a predecessor are renamed
to the block the diamond now lies in.

Usage: flatten_guarded_loads.py IN.spvasm OUT.spvasm
"""
import re
import sys

src = open(sys.argv[1]).read().split("\n")
out = []
rename = {}          # removed merge label -> label of the block it merged into
current_label = None
label_re = re.compile(r"^\s*(%\S+) = OpLabel\s*$")
i, flattened = 0, 0


def resolve(label):
    while label in rename:
        label = rename[label]
    return label


def fix_phi(line):
    if "OpPhi" not in line:
        return line
    head, _, rest = line.partition("OpPhi ")
    tokens = rest.split()
    for k in range(2, len(tokens), 2):  # type, then (value, parent) pairs
        tokens[k] = resolve(tokens[k])
    return head + "OpPhi " + " ".join(tokens)


while i < len(src):
    line = src[i]
    m = label_re.match(line)
    if m:
        current_label = m.group(1)
    window = src[i:i + 8]
    if (len(window) == 8 and "OpSelectionMerge" in window[0] and "OpBranchConditional" in window[1]
            and label_re.match(window[2]) and "OpAccessChain" in window[3] and "OpLoad" in window[4]
            and "OpBranch " in window[5] and label_re.match(window[6]) and "OpPhi" in window[7]):
        merge = window[0].split()[1]
        cond, true_label, false_label = window[1].split()[1:4]
        t = label_re.match(window[2]).group(1)
        mlabel = label_re.match(window[6]).group(1)
        load_id = window[4].split()[0]
        phi = window[7].split()
        # %r = OpPhi %type %v %t %zero %h
        if (merge == mlabel and false_label == mlabel and true_label == t and len(phi) == 8 and
                phi[4] == load_id and phi[5] == t and window[5].split()[1] == mlabel):
            out.append(window[3])
            out.append(window[4])
            out.append(f"{' ' * 8}{phi[0]} = OpSelect {phi[3]} {cond} {load_id} {phi[6]}")
            rename[mlabel] = current_label
            flattened += 1
            i += 8
            continue
    out.append(fix_phi(line))
    i += 1

open(sys.argv[2], "w").write("\n".join(out))
print(f"flattened {flattened} guarded loads")
