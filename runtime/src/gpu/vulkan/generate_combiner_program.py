#!/usr/bin/env python3
"""Generate specialization IDs and verify the scalar C instruction layout."""
import pathlib
import re
import sys

source, glsl, checks = map(pathlib.Path, sys.argv[1:])
structure = re.search(r"typedef struct\s*\{([^{}]*)\}\s*ChVkCombinerProgram;", source.read_text())
if not structure:
    raise SystemExit("Missing ChVkCombinerProgram instruction layout")
body = structure[1]
declarations = [s.strip() for s in body.split(";") if s.strip()]
constants, assignments = [], []
assertions = ["/* Generated specialization offsets. */", "#include <stddef.h>"]
word = 0
for decl in declarations:
    match = re.fullmatch(r"uint32_t (\w+)(?:\[(\d+)\])?", decl)
    if not match:
        raise SystemExit("Unsupported instruction field: " + decl)
    name, count = match.groups()
    assertions.append(f'_Static_assert(offsetof(ChVkCombinerProgram,{name})=={word*4},"specialization offset {name}");')
    for i in range(int(count) if count else 1):
        field = name + (f"[{i}]" if count else "")
        constants.append(f"layout(constant_id={word}) const uint ch_rc_word_{word}=0u;")
        assignments.append(f"    rc.{field}=ch_rc_word_{word};")
        word += 1
assertions.append(f'_Static_assert(sizeof(ChVkCombinerProgram)=={word*4},"specialization word count");')
glsl.write_text("\n".join(constants + ["ChRc ch_rc_program(ChRc rc) {"] + assignments + ["    return rc;", "}", ""]))
checks.write_text("\n".join(assertions) + "\n")
