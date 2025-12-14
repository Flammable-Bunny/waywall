#!/usr/bin/env python3
import sys
import os

out = open(sys.argv[1], 'w')
out.write("// Auto-generated SPIR-V shader bytecode\n")
out.write("#ifndef SHADER_SPIRV_H\n")
out.write("#define SHADER_SPIRV_H\n\n")
out.write("#include <stdint.h>\n")
out.write("#include <stddef.h>\n\n")

for spv_path in sys.argv[2:]:
    name = os.path.basename(spv_path).replace('.', '_')
    with open(spv_path, 'rb') as f:
        data = f.read()

    out.write(f"static const uint32_t {name}[] = {{\n")
    words = []
    for i in range(0, len(data), 4):
        word = int.from_bytes(data[i:i+4], 'little')
        words.append(f"0x{word:08x}")

    for i in range(0, len(words), 8):
        out.write("    " + ", ".join(words[i:i+8]) + ",\n")

    out.write("};\n")
    out.write(f"static const size_t {name}_size = sizeof({name});\n\n")

out.write("#endif\n")
out.close()
