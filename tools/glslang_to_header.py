import os
import struct
import subprocess
import sys


if __name__ == "__main__":
  argv = sys.argv[1:]
  if not argv:
    raise RuntimeError("Missing glslangValidator executable")

  output_path = None
  for i, arg in enumerate(argv):
    if arg == "-o":
      output_path = argv[i + 1]
      break
    if arg.startswith("-o="):
      output_path = arg[3:]
      break

  if output_path is None:
    raise RuntimeError("Missing -o output path")

  os.makedirs(os.path.dirname(output_path), exist_ok=True)
  spv_path = f"{output_path}.spv"

  command = argv.copy()
  for i, arg in enumerate(command):
    if arg == "-o":
      command[i + 1] = spv_path
      break
    if arg.startswith("-o="):
      command[i] = f"-o={spv_path}"
      break

  proc = subprocess.run(command, check=True, env=os.environ.copy(), capture_output=True)
  stdout, stderr = proc.stdout, proc.stderr

  if not os.path.exists(spv_path):
    raise RuntimeError("\n".join([
      f"Output {spv_path} not found",
      f"stdout: {stdout.decode() if stdout else 'None'}",
      f"stderr: {stderr.decode() if stderr else 'None'}",
    ]))

  with open(spv_path, "rb") as f:
    spv = f.read()

  if len(spv) % 4 != 0:
    raise RuntimeError(f"SPIR-V byte size is not word-aligned: {len(spv)}")

  data = struct.unpack(f"<{len(spv) // 4}I", spv)
  filename = os.path.splitext(os.path.basename(output_path))[0]

  code = "#pragma once\n"
  code += "#include <cstdint>\n"
  code += f"const uint32_t {filename}[] = {{\n"
  for i in range(0, len(data), 8):
    words = [f"0x{x:08x}" for x in data[i:i + 8]]
    code += "    " + ", ".join(words) + ",\n"
  code += "};\n"

  with open(output_path, "w") as f:
    f.write(code)

  os.remove(spv_path)
