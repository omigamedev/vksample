#version 450

layout (binding = 0) uniform ubo_t { 
	vec4 camera;
	vec4 light_pos;
} ubo;

layout (input_attachment_index = 0, binding = 1) uniform subpassInput g_depth;
layout (input_attachment_index = 1, binding = 2) uniform subpassInput g_pos;
layout (input_attachment_index = 2, binding = 3) uniform subpassInput g_nor;
layout (input_attachment_index = 3, binding = 4) uniform subpassInput g_alb;

layout (location = 0) out vec4 out_color;

void main()
{
	float depth = subpassLoad(g_depth).r;
	vec3 pos = subpassLoad(g_pos).rgb;
	vec3 nor = subpassLoad(g_nor).rgb;
	vec4 alb = subpassLoad(g_alb);
	if (gl_FragCoord.y < 300)
	{
		if (gl_FragCoord.x < 200)
			out_color = alb;
		else if (gl_FragCoord.x < 400)
			out_color = vec4(pos, 1);
		else if (gl_FragCoord.x < 600)
			out_color = vec4(nor * 0.5 + 0.5, 1);
		else
			out_color = vec4(vec3(pow(depth, 4.0)), 1);
	}
	else
	{
		float light = max(0.0, dot(nor, normalize(ubo.light_pos.xyz - pos)));
		out_color = vec4(alb * light * depth);
	}
}
