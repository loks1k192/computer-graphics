#version 450 core

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

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

// Свойства модели из динамического uniform-буфера: в шейдере это обычный uniform-блок,
// а нужный элемент массива выбирается сдвигом при привязке набора дескрипторов
layout(std140, set = 1, binding = 0) uniform ModelUniform {
	mat4 model;
	vec3 color;
	vec3 specular;
	float alpha;
} model_uniforms;

// Текстура альбедо вместе с сэмплером (combined image sampler)
layout(set = 1, binding = 1) uniform sampler2D albedo_texture;

// Доп. задание 2: включение нетривиального сэмплирования и время приходят в push-константах
layout(push_constant) uniform SamplingConstants {
	uint enabled;
	float time;
} sampling;

// Доп. задание 2: нетривиальное сэмплирование. Та же текстура читается дважды с разными
// текстурными координатами (первая выборка ползёт по u, вторая повёрнута и уменьшена вдвое),
// выборки смешиваются, а результат умножается на цвет, плавно меняющий оттенок.
// Всё зависит от времени, поэтому картинка на объекте постоянно движется.
vec3 sampleAlbedo(vec2 uv) {
	if (sampling.enabled == 0) {
		return texture(albedo_texture, uv).rgb;
	}

	float t = sampling.time;

	vec2 uv1 = uv + vec2(0.2 * t, 0.0);

	float c = cos(0.5 * t);
	float s = sin(0.5 * t);
	vec2 uv2 = mat2(c, s, -s, c) * (uv - 0.5) * 2.0 + 0.5;

	vec3 first = texture(albedo_texture, uv1).rgb;
	vec3 second = texture(albedo_texture, uv2).rgb;
	vec3 mixed = mix(first, second, 0.5 + 0.5 * sin(t));

	// Модуляция цвета: каждый канал колеблется со своим сдвигом фазы (0°, 120°, 240°)
	vec3 tint = 0.6 + 0.4 * cos(t + vec3(0.0, 2.094, 4.189));
	return mixed * tint;
}

// Массив точечных источников в storage-буфере (ключевое слово buffer).
// Размер массива не задаётся: шейдер берёт число источников из point_light_count.
struct PointLight {
	vec3 position;
	vec3 color;
};

layout(std430, set = 0, binding = 1) readonly buffer PointLights {
	PointLight point_lights[];
};

// Массив прожекторов. inner_cutoff и outer_cutoff — косинусы
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
// n — нормаль, v — направление на камеру, l — направление на источник, light — его цвет I,
// albedo — Kd, цвет из текстуры.
vec3 blinnPhong(vec3 n, vec3 v, vec3 l, vec3 light, vec3 albedo) {
	float cos_theta = dot(n, l);
	if (cos_theta <= 0.0) {
		return vec3(0.0); // источник позади поверхности
	}

	vec3 h = normalize(l + v);
	float cos_phi = max(dot(n, h), 0.0);

	vec3 diffuse = albedo * cos_theta;
	vec3 specular = model_uniforms.specular * pow(cos_phi, model_uniforms.alpha);
	return light * (diffuse + specular);
}

void main() {
	// После интерполяции длина нормали уже не равна 1
	vec3 n = normalize(in_normal);
	vec3 v = normalize(scene_uniforms.camera_position - in_world_position);

	// Альбедо (Kd) — цвет текстуры в точке с текстурными координатами in_uv.
	// Текстура в формате SRGB, поэтому texture() сразу возвращает линейный цвет.
	vec3 albedo = sampleAlbedo(in_uv);

	// Рассеянный свет: Ia * Ka, в качестве Ka берём альбедо
	vec3 color = scene_uniforms.ambient_light * albedo;

	// Лучи направленного источника идут вдоль direction, а l — направление на источник
	color += blinnPhong(n, v, -scene_uniforms.directional_light_direction,
	                    scene_uniforms.directional_light_color, albedo);

	for (uint i = 0; i < scene_uniforms.point_light_count; ++i) {
		vec3 to_light = point_lights[i].position - in_world_position;
		float distance = length(to_light);

		// Закон обратных квадратов: освещённость падает как 1 / D^2
		float attenuation = 1.0 / (distance * distance);

		color += blinnPhong(n, v, to_light / distance, point_lights[i].color * attenuation, albedo);
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

		color += blinnPhong(n, v, l, spot_lights[i].color * attenuation * spot, albedo);
	}

	out_color = vec4(color, 1.0);
}
