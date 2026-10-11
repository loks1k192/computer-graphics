#version 450 core

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(location = 0) out vec3 out_world_position;
layout(location = 1) out vec3 out_normal;

layout(std140, set = 0, binding = 0) uniform SceneUniforms {
	mat4 view_projection;
	vec3 camera_position;
	vec3 directional_light_direction;
	vec3 directional_light_color;
	vec3 ambient_light;
	uint point_light_count;
	uint spot_light_count;
} scene_uniforms;

layout(std140, set = 1, binding = 0) uniform ModelUniform {
	mat4 model;
	vec3 albedo;
	vec3 specular;
	float alpha;
} model_uniforms;

void main() {
	vec4 world_position = model_uniforms.model * vec4(in_position, 1);
	gl_Position = scene_uniforms.view_projection * world_position;

	// Освещение считается в мировых координатах. Нормаль умножается на обратную
	// транспонированную матрицу: при неравномерном растяжении обычная матрица модели
	// сделала бы нормаль не перпендикулярной поверхности.
	out_world_position = world_position.xyz;
	out_normal = transpose(inverse(mat3(model_uniforms.model))) * in_normal;
}
