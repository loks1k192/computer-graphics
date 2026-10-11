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
	uint point_light_count;
	uint spot_light_count;
} scene_uniforms;

layout(std140, set = 1, binding = 0) uniform ModelUniform {
	mat4 model;
	vec3 albedo;
	vec3 specular;
	float alpha;
} model_uniforms;

// Доп. задание 2: массив точечных источников в storage-буфере (ключевое слово buffer).
// Размер массива не задаётся: шейдер берёт число источников из point_light_count.
struct PointLight {
	vec3 position;
	vec3 color;
};

layout(std430, set = 0, binding = 1) readonly buffer PointLights {
	PointLight point_lights[];
};

// Доп. задание 3: массив прожекторов. inner_cutoff и outer_cutoff — косинусы
// внутреннего и внешнего углов конуса.
struct SpotLight {
	vec3 position;
	vec3 color;
	vec3 direction;
	float inner_cutoff;
	float outer_cutoff;
};

layout(std430, set = 0, binding = 2) readonly buffer SpotLights {
	SpotLight spot_lights[];
};

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

	for (uint i = 0; i < scene_uniforms.point_light_count; ++i) {
		vec3 to_light = point_lights[i].position - in_world_position;
		float distance = length(to_light);

		// Закон обратных квадратов: освещённость падает как 1 / D^2
		float attenuation = 1.0 / (distance * distance);

		color += blinnPhong(n, v, to_light / distance, point_lights[i].color * attenuation);
	}

	for (uint i = 0; i < scene_uniforms.spot_light_count; ++i) {
		vec3 to_light = spot_lights[i].position - in_world_position;
		float distance = length(to_light);
		vec3 l = to_light / distance;

		float attenuation = 1.0 / (distance * distance);

		// Мягкие края: косинус угла между осью прожектора и лучом на точку сравнивается
		// с косинусами углов конуса. Внутри внутреннего конуса множитель 1, снаружи
		// внешнего — 0, между ними плавно убывает. Если углы равны, край резкий.
		float cos_angle = dot(-l, spot_lights[i].direction);
		float edge = max(spot_lights[i].inner_cutoff - spot_lights[i].outer_cutoff, 0.0001);
		float spot = clamp((cos_angle - spot_lights[i].outer_cutoff) / edge, 0.0, 1.0);

		color += blinnPhong(n, v, l, spot_lights[i].color * attenuation * spot);
	}

	out_color = vec4(color, 1.0);
}
