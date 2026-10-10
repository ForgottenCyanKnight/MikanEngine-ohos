"""Inject global highp precision into every shader source.

- rhi_gles.cpp embedded GLSL ES (#version 300 es): replace/insert a canonical
  precision block (float/int/sampler2D/samplerCube all highp). Fragment
  shaders on mobile GLES default to mediump float and lowp sampler2D, which
  quantizes depth reconstruction / shadow coords -> wave-like shadow flicker.
- shaders/*.vert|*.frag (GLSL 450 -> SPIR-V): insert float/int highp block
  after #version for lockstep (no-op for Vulkan fp32, keeps sources aligned).
"""
import io
import re
import sys

GLES_BLOCK = (
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "precision highp samplerCube;\n"
)
GLSL450_BLOCK = "precision highp float;\nprecision highp int;\n"

def patch_gles(path):
    with io.open(path, "r", encoding="utf-8") as f:
        src = f.read()
    # Split on each embedded shader start and rebuild.
    parts = src.split('#version 300 es\n')
    if len(parts) == 1:
        print("no GLES shaders in", path)
        return
    out = [parts[0]]
    count = 0
    for chunk in parts[1:]:
        # Strip any existing precision statements at the top of the shader.
        lines = chunk.split("\n")
        i = 0
        while i < len(lines) and re.match(r"\s*precision\s+\w+\s+\w+\s*;", lines[i]):
            i += 1
        chunk = "\n".join(lines[i:])
        out.append(GLES_BLOCK + chunk)
        count += 1
    result = '#version 300 es\n'.join(out)
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write(result)
    print("patched %d GLES shaders in %s" % (count, path))

def patch_glsl450(path):
    with io.open(path, "r", encoding="utf-8") as f:
        src = f.read()
    lines = src.split("\n")
    if not lines or not lines[0].startswith("#version 450"):
        print("skip (no #version 450):", path)
        return
    i = 1
    while i < len(lines) and re.match(r"\s*precision\s+\w+\s+\w+\s*;", lines[i]):
        i += 1
    body = "\n".join(lines[i:])
    result = lines[0] + "\n" + GLSL450_BLOCK + body
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write(result)
    print("patched GLSL450:", path)

if __name__ == "__main__":
    patch_gles(sys.argv[1])
    for p in sys.argv[2:]:
        patch_glsl450(p)
