#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag;
layout(push_constant) uniform PC {
    vec4 rect;
    vec4 uv;
    vec4 tint;
    vec2 canvas;
    float angle;
    float pad;
    vec4 projection_x;
    vec4 projection_y;
    vec4 projection_w;
} pc;
layout(binding = 0) uniform sampler2D tex;
const uint ALPHA_PREMULTIPLY = 1u;
const uint ALPHA_UNPREMULTIPLY = 2u;
void main() {
    vec4 texel = texture(tex, v_uv);
    uint alpha_mode = uint(pc.pad);
    if ((alpha_mode & ALPHA_UNPREMULTIPLY) != 0u) {
        texel.rgb = texel.a > 0.0 ? texel.rgb / texel.a : vec3(0.0);
    }
    vec3 rgb = texel.rgb * pc.tint.rgb;
    float alpha = texel.a * pc.tint.a;
    frag = (alpha_mode & ALPHA_PREMULTIPLY) != 0u ? vec4(rgb * alpha, alpha) : vec4(rgb, alpha);
}
