#version 450
layout(location=0) in vec4 position;
layout(location=1) in vec4 color;
layout(location=2) in vec2 texcoord;
layout(location=1) out vec2 uv;
layout(location=3) in vec4 specular;
layout(location=4) in float fog_factor;
layout(location=5) in vec4 tc0;
layout(location=6) in vec4 tc1;
layout(location=7) in vec4 tc2;
layout(location=8) in vec4 tc3;
layout(location=2) out vec4 spec;
layout(location=3) out float fog;
layout(location=4) out vec4 tc[4];
layout(location=0) out vec4 diffuse;
void main() { gl_Position=vec4(position.xyz,position.w == 0 ? 1 : position.w); diffuse=color; uv=texcoord; spec=specular; fog=fog_factor; tc[0]=tc0; tc[1]=tc1; tc[2]=tc2; tc[3]=tc3; }
