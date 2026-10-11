#version 450 core

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_normal;

layout(location = 0) out vec4 out_color;

layout(std140, set = 0, binding = 0) uniform SceneUniforms {
	mat4 view_projection;
	vec3 camera_position;
	vec3 directional_light_direction;
	vec3 directional_light_color;
	vec3 ambient_light;
} scene_uniforms;

layout(std140, set = 1, binding = 0) uniform ModelUniform {
	mat4 model;
	vec3 albedo;
	vec3 specular;
	float alpha;
} model_uniforms;

// Модель Блинна-Фонга для одного источника (лекция 4):
// C = I * (cos(theta) * Kd + Ks * cos(phi)^alpha), cos(theta) = n * l, cos(phi) = n * h.
// n — нормаль, v — направление на камеру, l — направление на источник, light — его цвет I.
vec3 blinnPhong(vec3 n, vec3 v, vec3 l, vec3 light) {
	float cos_theta = dot(n, l);
	if (cos_theta <= 0.0) {
		return vec3(0.0); // источник позади поверхности
	}

	vec3 h = normalize(l + v);
	float cos_phi = max(dot(n, h), 0.0);

	vec3 diffuse = model_uniforms.albedo * cos_theta;
	vec3 specular = model_uniforms.specular * pow(cos_phi, model_uniforms.alpha);
	return light * (diffuse + specular);
}

void main() {
	// После интерполяции длина нормали уже не равна 1
	vec3 n = normalize(in_normal);
	vec3 v = normalize(scene_uniforms.camera_position - in_world_position);

	// Рассеянный свет: Ia * Ka, в качестве Ka берём альбедо
	vec3 color = scene_uniforms.ambient_light * model_uniforms.albedo;

	// Лучи направленного источника идут вдоль direction, а l — направление на источник
	color += blinnPhong(n, v, -scene_uniforms.directional_light_direction,
	                    scene_uniforms.directional_light_color);

	out_color = vec4(color, 1.0);
}
