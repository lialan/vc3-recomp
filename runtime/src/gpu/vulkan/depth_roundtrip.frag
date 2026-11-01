#version 450
#extension GL_GOOGLE_include_directive : require
#include "depth_quant.glsl"
layout(std430,set=0,binding=0) readonly buffer Depth { float values[]; } source;
layout(push_constant) uniform Size { uint width; } size;
void main() {
    uint i=uint(gl_FragCoord.y)*size.width+uint(gl_FragCoord.x);
    gl_FragDepth=ch_depth_unpack(ch_depth_pack(source.values[i]));
}
