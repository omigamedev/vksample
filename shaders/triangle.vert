#version 450

const vec2 vertices[3] = {
    { 0.0, -0.5},
    {-0.5,  0.5},
    { 0.5,  0.5},
};

void main()
{
    gl_Position = vec4(vertices[gl_VertexIndex], 0, 1);
}
