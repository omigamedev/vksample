#version 450

layout (location = 0) in vec3 v_pos;
layout (binding = 0) uniform ubo_t { mat4 mvp; } ubo;

void main()
{
    gl_Position = ubo.mvp * vec4(v_pos, 1);
}
