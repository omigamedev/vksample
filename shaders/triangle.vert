#version 450

layout (binding = 0) uniform ubo_t { 
    mat4 model;
    mat4 view;
    mat4 proj; 
} ubo;

layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec3 v_nor;

layout (location = 0) out vec3 f_nor;
layout (location = 1) out vec3 f_pos;

void main()
{
    gl_Position = ubo.proj * ubo.view * ubo.model * vec4(v_pos, 1);
    f_pos = vec3(ubo.model * vec4(v_pos, 1));
    f_nor = normalize(v_nor);
}
