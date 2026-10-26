#!/usr/bin/env python3
# Extract the C string literals of runtime/nvn/vulkan/shaders.c's builtin_src() blocks into .glsl files,
# so they can be checked with glslangValidator (a syntax error there is a FATAL at runtime).
#   python tools/extract_builtin_shaders.py <outdir>
import os
import re
import sys

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runtime", "nvn", "vulkan", "shaders.c")


def literals(text):
    """Yield (name, glsl) for every `if (!strcmp(name, "<n>")) { ... return "<...>"; }` block."""
    for m in re.finditer(r'!strcmp\(name, "([^"]+)"\)', text):
        name = m.group(1)
        rest = text[m.end():]
        # the first `return` in the block, then its concatenated string literals up to the `;`
        r = rest.find("return ")
        if r < 0:
            continue
        seg = rest[r + len("return "):]
        end = seg.find(";\n")
        if end < 0:
            continue
        body = seg[:end]
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
        if not parts:
            continue
        glsl = "".join(p.encode().decode("unicode_escape") for p in parts)
        yield name, glsl


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    text = open(SRC, encoding="utf-8", errors="replace").read()
    for name, glsl in literals(text):
        path = os.path.join(out, name + ".glsl")
        open(path, "w", encoding="utf-8", newline="\n").write(glsl)
        print("%s: %d bytes" % (path, len(glsl)))


if __name__ == "__main__":
    main()
