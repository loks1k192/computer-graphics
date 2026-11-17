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
	mat4 sun_shadow_matrix; // проекция * вид солнца для карты теней
} scene_uniforms;

// Свойства модели из динамического uniform-буфера: в шейдере это обычный uniform-блок,
// а нужный элемент массива выбирается сдвигом при привязке набора дескрипторов
layout(std140, set = 1, binding = 0) uniform ModelUniform {
	mat4 model;
	vec3 color;
	vec3 specular;
	float alpha;
} model_uniforms;

// ЛР4: карта теней солнца вместе с сэмплером сравнения глубины. texture() для sampler2DShadow
// принимает (u, v, глубина фрагмента) и возвращает результат сравнения с глубиной из карты:
// 1 — фрагмент ближе к солнцу, чем записанная глубина (освещён), 0 — за препятствием (в тени).
layout(set = 0, binding = 3) uniform sampler2DShadow sun_shadow_map;

// Текстура альбедо вместе с сэмплером (combined image sampler)
layout(set = 1, binding = 1) uniform sampler2D albedo_texture;

// Включение нетривиального сэмплирования и время приходят в push-константах
layout(push_constant) uniform SamplingConstants {
	uint enabled;
	float time;
} sampling;

// Нетривиальное сэмплирование. Та же текстура читается дважды с разными
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

// ЛР4: «затенение» точки: 1 — освещена, 0 — в тени, между ними — на мягком краю.
// Точка переводится матрицей источника в его пространство отсечения, после деления на w
// x, y в [-1, 1] переводятся в текстурные координаты [0, 1], а z — глубина для сравнения.
float shadowFactor(sampler2DShadow shadow_map, mat4 light_matrix) {
	vec4 position = light_matrix * vec4(in_world_position, 1.0);
	vec3 projected = position.xyz / position.w;
	vec2 uv = projected.xy * 0.5 + 0.5;

	// Точки вне карты теней считаются освещёнными
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projected.z > 1.0) {
		return 1.0;
	}

	return texture(shadow_map, vec3(uv, projected.z));
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

	// Лучи направленного источника идут вдоль direction, а l — направление на источник.
	// ЛР4: вклад солнца умножается на затенение из карты теней.
	float sun_shadow = shadowFactor(sun_shadow_map, scene_uniforms.sun_shadow_matrix);
	color += sun_shadow * blinnPhong(n, v, -scene_uniforms.directional_light_direction,
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
