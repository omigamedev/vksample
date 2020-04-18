#version 450

const vec2 vertices[3] = {
    { 0.0, -0.5},
    {-0.5,  0.5},
    { 0.5,  0.5},
};

layout (binding = 0) uniform ubo_t { mat4 mvp; } ubo;

void main()
{
    gl_Position = ubo.mvp * vec4(vertices[gl_VertexIndex], 0, 1);
}
