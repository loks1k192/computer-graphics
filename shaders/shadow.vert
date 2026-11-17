#version 450 core

// ЛР4: шейдер карты теней. Это единственный шейдер конвейера теней: он только проецирует
// геометрию с точки зрения источника света. Фрагментного шейдера нет — в карту теней
// записывается только глубина.

layout(location = 0) in vec3 in_position;

layout(std140, set = 0, binding = 0) uniform SceneUniforms {
	mat4 view_projection;
	vec3 camera_position;
	vec3 directional_light_direction;
	vec3 directional_light_color;
	vec3 ambient_light;
	uint point_light_count;
	uint spot_light_count;
	mat4 sun_shadow_matrix;
	mat4 spot_shadow_matrix;
	int shadow_spot_index;
} scene_uniforms;

layout(std140, set = 1, binding = 0) uniform ModelUniform {
	mat4 model;
	vec3 color;
	vec3 specular;
	float alpha;
} model_uniforms;

// Доп. задание: с точки зрения какого источника рисуется карта (0 — солнце, 1 — прожектор).
// Диапазон push-констант вершинного шейдера начинается со смещения 8.
layout(push_constant) uniform ShadowConstants {
	layout(offset = 8) uint light_index;
} shadow;

void main() {
	mat4 light_matrix = shadow.light_index == 0 ? scene_uniforms.sun_shadow_matrix
	                                            : scene_uniforms.spot_shadow_matrix;
	gl_Position = light_matrix * model_uniforms.model * vec4(in_position, 1);
}
