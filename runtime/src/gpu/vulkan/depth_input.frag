#version 450
#extension GL_GOOGLE_include_directive : require
#include "depth_quant.glsl"
layout(input_attachment_index=0,set=0,binding=0) uniform subpassInput source;
void main() { gl_FragDepth=ch_depth_unpack(ch_depth_pack(subpassLoad(source).x)); }
