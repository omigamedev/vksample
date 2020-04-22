#version 450

layout (binding = 1) uniform ubo_t { vec4 col; } ubo;

layout (location = 0) in vec3 f_nor;
layout (location = 1) in vec3 f_pos;

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_nor;
layout (location = 2) out vec4 out_alb;

void main()
{
    out_alb = ubo.col;
    out_pos = f_pos;
    out_nor = f_nor;
}
