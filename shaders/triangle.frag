#version 450

layout (binding = 1) uniform ubo_t { vec4 col; } ubo;
layout (location = 0) out vec4 out_color;

void main()
{
    out_color = ubo.col;
}
