#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : enable

layout (binding = 0, set = 0) uniform accelerationStructureEXT tlas;
layout (binding = 2, set = 0) uniform ubo_t { 
    vec4 light_pos; 
} ubo;

layout (location = 0) rayPayloadInEXT vec3 hitValue;
hitAttributeEXT vec3 attribs;

void main()
{
  const vec3 barycentricCoords = vec3(1.0f - attribs.x - attribs.y, attribs.x, attribs.y);
  hitValue = vec3(gl_PrimitiveID / 1000.0);
}
