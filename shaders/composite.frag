#version 450

layout (input_attachment_index = 0, binding = 0) uniform subpassInput gbuf_pos;
layout (input_attachment_index = 1, binding = 1) uniform subpassInput depth;
layout (location = 0) out vec4 out_color;

void main()
{
	vec4 d = subpassLoad(depth);
	vec4 pos = subpassLoad(gbuf_pos);
	if (gl_FragCoord.x > 400)
		out_color = vec4(vec3(pow(d.r, 4.0)), pos.a);
    else
		out_color = vec4(pos.rgb, pos.a);
}
