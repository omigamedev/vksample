#version 450

const vec2 vertices[6] = {
    {-1.0,  1.0},
    {-1.0, -1.0},
    { 1.0, -1.0},

    {-1.0,  1.0},
    { 1.0, -1.0},
    { 1.0,  1.0},
};

void main()
{
    gl_Position = vec4(vertices[gl_VertexIndex], 0, 1);
}
