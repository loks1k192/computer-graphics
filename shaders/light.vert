#version 450 core

// Доп. задание 4: фигуры-визуализации источников света (кубы и конусы прожекторов).
// Они не освещаются по Блинну-Фонгу, а рисуются постоянным цветом.

layout(location = 0) in vec3 in_position;

layout(location = 0) out vec3 out_color;

layout(std140, set = 0, binding = 0) uniform SceneUniforms {
	mat4 view_projection;
	vec3 camera_position;
	vec3 directional_light_direction;
	vec3 directional_light_color;
	vec3 ambient_light;
	uint point_light_count;
	uint spot_light_count;
} scene_uniforms;

// Матрица и цвет фигуры передаются push-константами перед каждой отрисовкой
layout(push_constant) uniform LightMarker {
	mat4 model;
	vec4 color;
} light_marker;

void main() {
	gl_Position = scene_uniforms.view_projection * light_marker.model * vec4(in_position, 1);
	out_color = light_marker.color.rgb;
}
