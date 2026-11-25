#version 450
#extension GL_ARB_shader_stencil_export : require
// Depth and stencil of a texture copy in one draw (nvn_vk.c zcopy_draw, where the device has VK_EXT_shader_stencil_export): the source texel under this pixel,
//   nearest, as zcopy_d.frag; its stencil written as the stencil reference (the pipeline's pass op REPLACE), in place of zcopy_s.frag's eight draws, one per bit
layout(set = 0, binding = 0) uniform sampler2D z;
layout(set = 0, binding = 1) uniform usampler2D s;
layout(push_constant) uniform P { vec4 m; ivec4 lim; } p;
void main() {
    ivec2 t = clamp(ivec2(floor(gl_FragCoord.xy * p.m.xy + p.m.zw)), ivec2(0), p.lim.xy);
    gl_FragDepth = texelFetch(z, t, 0).r;
    gl_FragStencilRefARB = int(texelFetch(s, t, 0).r & 0xffu);
}
