#version 450

layout (input_attachment_index = 0, binding = 0) uniform subpassInput gbuf_pos;
layout (location = 0) out vec4 out_color;

void main()
{
	vec4 pos = subpassLoad(gbuf_pos);
    out_color = vec4(pos.rgb, pos.a);
}
