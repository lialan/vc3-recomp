#!/usr/bin/env python3
"""Use the actual GL backend fixed vertex source in the GPU parity fixture."""
import pathlib, sys
source=pathlib.Path(sys.argv[1]).read_text()
a=source.index("static const char *VS_FF =")
b=source.index("/* Vertex programs on the GPU",a)
part=source[a:b].replace("VS_FF", "CH_FF_REFERENCE", 1)
part=part.replace('"#version 410\\n"','"#version 320 es\\n#extension GL_EXT_clip_cull_distance : require\\nprecision highp float; precision highp int;\\n"',1)
pathlib.Path(sys.argv[2]).write_text("/* Actual reference renderer source, adapted to GLES. */\n"+part)
