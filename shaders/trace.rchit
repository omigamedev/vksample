#version 460
#extension GL_NV_ray_tracing : require
#extension GL_GOOGLE_include_directive : enable

layout (location = 0) rayPayloadInNV vec3 hitValue;
hitAttributeNV vec3 attribs;

void main()
{
  const vec3 barycentricCoords = vec3(1.0f - attribs.x - attribs.y, attribs.x, attribs.y);
  hitValue = barycentricCoords;
}
