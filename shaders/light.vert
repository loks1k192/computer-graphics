#version 450 core

// Фигуры-визуализации источников света (кубы и конусы прожекторов).
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
	mat4 sun_shadow_matrix; // проекция * вид солнца для карты теней
	mat4 spot_shadow_matrix; // проекция * вид прожектора, отбрасывающего тень
	int shadow_spot_index;   // индекс этого прожектора, -1 — тени от прожектора нет
} scene_uniforms;

// Матрица и цвет фигуры — элемент того же динамического uniform-буфера, что и у объектов
layout(std140, set = 1, binding = 0) uniform ModelUniform {
	mat4 model;
	vec3 color;
	vec3 specular;
	float alpha;
} model_uniforms;

void main() {
	gl_Position = scene_uniforms.view_projection * model_uniforms.model * vec4(in_position, 1);
	out_color = model_uniforms.color;
}
