#include "application.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include <imgui.h>

#include "graphics.hpp"
#include "math.hpp"

namespace application {

namespace {

// ЛР3: у вершины, кроме позиции и нормали, есть текстурные координаты (u, v)
struct Vertex {
	math::Vector3 position;
	math::Vector3 normal;
	float uv[2];
};

// Данные разделены на два набора дескрипторов.
// Раскладки по std140 должны совпадать с блоками в шейдерах: vec3 выравнивается
// на 16 байт, поэтому после vec3 стоят 4 байта выравнивания, а float может
// поместиться сразу за vec3.

// set = 0: общие для всей сцены данные, один буфер на всю сцену
struct SceneUniforms {
	math::Matrix4 view_projection;             // 0
	math::Vector3 camera_position;             // 64
	float _pad0;
	math::Vector3 directional_light_direction; // 80
	float _pad1;
	math::Vector3 directional_light_color;     // 96
	float _pad2;
	math::Vector3 ambient_light;               // 112
	uint32_t point_light_count;                // 124
	uint32_t spot_light_count;                 // 128
	float _pad3[3];                            // mat4 выравнивается на 16: следующее смещение 144
	math::Matrix4 sun_shadow_matrix;           // 144: проекция * вид солнца, для карты теней
};

// set = 1, binding = 0: свойства одной модели. Свойства всех моделей лежат массивом в одном
// динамическом uniform-буфере, нужный элемент выбирается сдвигом при привязке набора.
// Альбедо освещаемых объектов теперь берётся из текстуры, а color — постоянный цвет
// фигур источников света (они рисуются без текстуры и без освещения).
struct ModelUniforms {
	math::Matrix4 model;    // 0
	math::Vector3 color;    // 64
	float _pad0;
	math::Vector3 specular; // 80
	float alpha;            // 92
};

const float phi = (1.0f + std::sqrt(5.0f)) / 2.0f;

const math::Vector3 icosahedron_positions[] = {
	{ -1.0f,  phi,  0.0f },
	{  1.0f,  phi,  0.0f },
	{ -1.0f, -phi,  0.0f },
	{  1.0f, -phi,  0.0f },

	{  0.0f, -1.0f,  phi },
	{  0.0f,  1.0f,  phi },
	{  0.0f, -1.0f, -phi },
	{  0.0f,  1.0f, -phi },

	{  phi,  0.0f, -1.0f },
	{  phi,  0.0f,  1.0f },
	{ -phi,  0.0f, -1.0f },
	{ -phi,  0.0f,  1.0f },
};

// Вершины каждой грани перечислены против часовой стрелки, если смотреть снаружи
const uint32_t icosahedron_indices[] = {
	0, 11, 5,   0, 5, 1,    0, 1, 7,    0, 7, 10,   0, 10, 11,
	1, 5, 9,    5, 11, 4,   11, 10, 2,  10, 7, 6,   7, 1, 8,
	3, 9, 4,    3, 4, 2,    3, 2, 6,    3, 6, 8,    3, 8, 9,
	4, 9, 5,    2, 4, 11,   6, 2, 10,   8, 6, 7,    9, 8, 1,
};

constexpr uint32_t icosahedron_index_count = sizeof(icosahedron_indices) / sizeof(icosahedron_indices[0]);

// Флаг и время для нетривиального сэмплирования текстуры. Передаются
// push-константами прямо в командный буфер перед отрисовкой объекта, а не через
// uniform-буфер свойств модели.
struct SamplingConstants {
	uint32_t enabled; // 0 — обычное чтение текстуры, 1 — нетривиальное сэмплирование
	float time;       // время в секундах, от него зависит эффект
};

// Точечный источник в storage-буфере. Раскладка std430 совпадает со struct PointLight
// в шейдере: vec3 выравнивается на 16 байт, размер структуры 32 байта.
struct PointLightData {
	math::Vector3 position; // 0
	float _pad0;
	math::Vector3 color;    // 16
	float _pad1;
};

// Параметры точечного источника, которые меняются в интерфейсе
struct PointLight {
	math::Vector3 position;
	math::Vector3 color;
	float intensity;
};

// Прожекторный источник в storage-буфере, раскладка std430.
// Вместо углов передаются их косинусы, чтобы не считать cos для каждого пикселя.
// Размер структуры округляется до кратного 16 (выравнивание vec3), поэтому 64 байта.
struct SpotLightData {
	math::Vector3 position;  // 0
	float _pad0;
	math::Vector3 color;     // 16
	float _pad1;
	math::Vector3 direction; // 32
	float inner_cutoff;      // 44
	float outer_cutoff;      // 48
	float _pad2[3];
};

// Параметры прожектора в интерфейсе. Углы — между осью прожектора и краем конуса, в градусах.
struct SpotLight {
	math::Vector3 position;
	math::Vector3 direction;
	math::Vector3 color;
	float intensity;
	float inner_angle;
	float outer_angle;
};

// Геометрия в памяти видеокарты (VRAM): вершины и индексы одной фигуры
struct Mesh {
	graphics::Buffer vertex_buffer;
	graphics::Buffer index_buffer;
	uint32_t index_count;
};

Mesh icosahedron_mesh;
Mesh floor_mesh;
Mesh cube_mesh;
Mesh cone_mesh;   // конус прожектора из линий: ось, образующие и окружность основания
Mesh circle_mesh; // окружность внутреннего угла прожектора

constexpr float z_near = 0.1f;
constexpr float z_far = 100.0f;

// Камера: позиция и поворот (в градусах). Ось Y направлена вниз, поэтому камера
// над полом имеет отрицательный y. Pitch > 0 — взгляд вверх.
math::Vector3 camera_position = { 0.0f, -3.5f, -10.0f };
float camera_yaw = 0.0f;
float camera_pitch = -20.0f;
float camera_speed = 3.0f;

int projection_type = 0;
float fov_degrees = 60.0f;
float ortho_size = 7.0f;

// Направленный источник: direction — куда идут лучи
math::Vector3 directional_light_direction = { 0.4f, 1.0f, 0.6f };
math::Vector3 directional_light_color = { 1.0f, 0.95f, 0.85f };
float directional_light_intensity = 0.8f;

math::Vector3 ambient_light_color = { 1.0f, 1.0f, 1.0f };
float ambient_light_intensity = 0.1f;

std::vector<PointLight> point_lights = {
	{ { -4.5f, 1.0f, -1.5f }, { 1.0f, 0.5f, 0.2f }, 4.0f },
	{ {  4.0f, 1.0f,  2.0f }, { 0.3f, 0.6f, 1.0f }, 4.0f },
};

std::vector<SpotLight> spot_lights = {
	{ { 0.0f, -3.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.95f, 0.8f }, 20.0f, 15.0f, 25.0f },
	{ { -4.0f, -1.0f, -3.0f }, { 0.6f, 0.6f, 0.5f }, { 0.4f, 1.0f, 0.4f }, 15.0f, 10.0f, 12.0f },
};

bool show_light_sources = true;

bool animation_playing = true;
float animation_speed = 1.0f;
float trajectory_radius = 2.5f;
float trajectory_height = 0.5f;
float animation_time = 0.0f;
double last_time = 0.0;

struct Object {
	const Mesh* mesh;
	bool animated; // летает ли объект по траектории из ЛР1

	math::Vector3 position;
	math::Vector3 rotation;
	math::Vector3 scale;

	// Материал по модели Блинна-Фонга: альбедо берётся из текстуры
	int texture; // индекс текстуры (материала) из списка
	bool texture_effect; // включено ли нетривиальное сэмплирование
	math::Vector3 specular;
	float shininess;
};

constexpr int object_count = 4;

// Три икосаэдра из ЛР1 и пол (квадрат 2x2, растянутый в 8 раз)
Object objects[object_count] = {
	{ .mesh = &icosahedron_mesh, .animated = true, .scale = { 0.6f, 0.6f, 0.6f },
	  .texture = 0, .texture_effect = true, .specular = { 0.5f, 0.5f, 0.5f }, .shininess = 32.0f },
	{ .mesh = &icosahedron_mesh, .animated = true, .scale = { 0.6f, 0.6f, 0.6f },
	  .texture = 2, .specular = { 1.0f, 1.0f, 1.0f }, .shininess = 128.0f },
	{ .mesh = &icosahedron_mesh, .animated = true, .scale = { 0.6f, 0.6f, 0.6f },
	  .texture = 4, .specular = { 0.8f, 0.8f, 0.8f }, .shininess = 64.0f },
	{ .mesh = &floor_mesh, .animated = false, .position = { 0.0f, 2.0f, 0.0f }, .scale = { 8.0f, 1.0f, 8.0f },
	  .texture = 3, .specular = { 0.3f, 0.3f, 0.3f }, .shininess = 16.0f },
};

int selected_object = 0;

float effect_time = 0.0f; // время для эффекта, идёт всегда, даже на паузе анимации

SceneUniforms scene_uniforms;
graphics::Buffer scene_uniform_buffer;
VkDescriptorSet scene_descriptor_set;

// Storage-буфер растёт вместе с числом источников, capacity — сколько источников в нём помещается
graphics::Buffer point_light_buffer;
uint32_t point_light_capacity = 4;

graphics::Buffer spot_light_buffer;
uint32_t spot_light_capacity = 4;

// Динамический uniform-буфер: массив ModelUniforms, по элементу на каждую отрисовку
// (объекты, а затем кубы и конусы источников). Элементы лежат с шагом stride, кратным
// minUniformBufferOffsetAlignment, потому что сдвиг при привязке должен быть ему кратен.
std::vector<ModelUniforms> model_uniforms;
graphics::Buffer model_uniform_buffer;
uint32_t model_uniform_capacity = 16;
VkDeviceSize model_uniform_stride;

// Несколько текстур, у каждой (у каждого материала) свой набор дескрипторов
// set = 1: динамический uniform-буфер свойств моделей (общий) и своя текстура
struct Material {
	graphics::Texture texture;
	VkDescriptorSet descriptor_set;
};

constexpr int texture_count = 5;

const char* const texture_names[texture_count] = { "Bricks", "Wood", "Marble", "Tiles", "Rusty metal" };
const char* const texture_paths[texture_count] = {
	"textures/bricks.png",
	"textures/wood.png",
	"textures/marble.png",
	"textures/tiles.png",
	"textures/rusty_metal.png",
};

Material materials[texture_count];

// Сэмплер общий для всех текстур: как читать текстуру (фильтрация, повтор за краями)
VkSampler vk_sampler;

// ЛР4: карта теней — изображение глубины сцены с точки зрения солнца.
// Рисуется каждый кадр динамическим рендерингом, затем читается в основном фрагментном шейдере.
constexpr uint32_t shadow_map_size = 2048;
constexpr VkFormat shadow_map_format = VK_FORMAT_D32_SFLOAT;

graphics::Texture sun_shadow_map;

// Сэмплер со сравнением глубины: вместо значения глубины возвращает результат сравнения
// глубины фрагмента с глубиной в карте (1 — освещено, 0 — в тени)
VkSampler vk_shadow_sampler;

// Параметры тени из интерфейса: сторона квадрата, который покрывает ортографическая
// проекция солнца, и сдвиг глубины (depth bias) против «теневых прыщей» (shadow acne)
float sun_shadow_area = 20.0f;
float depth_bias_constant = 1.25f;
float depth_bias_slope = 1.75f;

VkDescriptorSetLayout vk_scene_set_layout;
VkDescriptorSetLayout vk_material_set_layout;
VkDescriptorPool vk_descriptor_pool;

// Один pipeline layout на все три конвейера, поэтому наборы не нужно перепривязывать
VkPipelineLayout vk_pipeline_layout;
VkPipeline vk_pipeline;       // освещаемые объекты
VkPipeline vk_light_pipeline; // кубы источников
VkPipeline vk_line_pipeline;  // конусы прожекторов
VkPipeline vk_shadow_pipeline; // ЛР4: карта теней, динамический рендеринг, только вершинный шейдер

// Вершины и индексы копируются в VRAM через staging-буфер (graphics::createDeviceBuffer)
bool createMesh(const Vertex vertices[], uint32_t vertex_count,
                const uint32_t indices[], uint32_t index_count, Mesh& mesh) {
	if (!graphics::createDeviceBuffer(vertices, vertex_count * sizeof(Vertex),
	                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mesh.vertex_buffer)) {
		return false;
	}

	if (!graphics::createDeviceBuffer(indices, index_count * sizeof(uint32_t),
	                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh.index_buffer)) {
		return false;
	}

	mesh.index_count = index_count;
	return true;
}

void destroyMesh(const Mesh& mesh) {
	graphics::destroyBuffer(mesh.index_buffer);
	graphics::destroyBuffer(mesh.vertex_buffer);
}

bool createMeshes() {
	// Нормали и текстурные координаты икосаэдра генерируются процедурно. Нормаль грани —
	// векторное произведение двух её рёбер. На каждую грань (правильный треугольник)
	// накладывается равносторонний треугольник из текстуры, поэтому картинка не искажается.
	// Вершины между гранями не делятся: 20 граней по 3 вершины.
	const float face_uv[3][2] = {
		{ 0.5f, 0.0f },
		{ 0.0f, 0.866f },
		{ 1.0f, 0.866f },
	};

	Vertex icosahedron_vertices[icosahedron_index_count];
	uint32_t indices[icosahedron_index_count];

	for (uint32_t i = 0; i < icosahedron_index_count; i += 3) {
		const math::Vector3 a = icosahedron_positions[icosahedron_indices[i + 0]];
		const math::Vector3 b = icosahedron_positions[icosahedron_indices[i + 1]];
		const math::Vector3 c = icosahedron_positions[icosahedron_indices[i + 2]];

		// Обход против часовой стрелки снаружи, поэтому нормаль смотрит наружу
		const math::Vector3 normal = math::normalize(math::cross(b - a, c - a));

		icosahedron_vertices[i + 0] = { a, normal, { face_uv[0][0], face_uv[0][1] } };
		icosahedron_vertices[i + 1] = { b, normal, { face_uv[1][0], face_uv[1][1] } };
		icosahedron_vertices[i + 2] = { c, normal, { face_uv[2][0], face_uv[2][1] } };

		indices[i + 0] = i + 0;
		indices[i + 1] = i + 1;
		indices[i + 2] = i + 2;
	}

	if (!createMesh(icosahedron_vertices, icosahedron_index_count, indices, icosahedron_index_count,
	                icosahedron_mesh)) {
		return false;
	}

	// Пол — квадрат в плоскости y = 0. Ось Y направлена вниз, поэтому нормаль (0, -1, 0).
	// Текстурные координаты от 0 до 4: при режиме REPEAT текстура повторится 4 раза.
	const Vertex floor_vertices[] = {
		{ { -1.0f, 0.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } },
		{ {  1.0f, 0.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, { 4.0f, 0.0f } },
		{ {  1.0f, 0.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, { 4.0f, 4.0f } },
		{ { -1.0f, 0.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 4.0f } },
	};

	const uint32_t floor_indices[] = { 0, 1, 2, 0, 2, 3 };

	if (!createMesh(floor_vertices, 4, floor_indices, 6, floor_mesh)) {
		return false;
	}

	// Куб для визуализации источников. Он не освещается и не текстурируется.
	const Vertex cube_vertices[] = {
		{ { -1.0f, -1.0f, -1.0f } }, { {  1.0f, -1.0f, -1.0f } },
		{ {  1.0f,  1.0f, -1.0f } }, { { -1.0f,  1.0f, -1.0f } },
		{ { -1.0f, -1.0f,  1.0f } }, { {  1.0f, -1.0f,  1.0f } },
		{ {  1.0f,  1.0f,  1.0f } }, { { -1.0f,  1.0f,  1.0f } },
	};

	const uint32_t cube_indices[] = {
		0, 1, 2,  0, 2, 3,
		4, 6, 5,  4, 7, 6,
		0, 4, 5,  0, 5, 1,
		3, 2, 6,  3, 6, 7,
		0, 3, 7,  0, 7, 4,
		1, 5, 6,  1, 6, 2,
	};

	if (!createMesh(cube_vertices, 8, cube_indices, 36, cube_mesh)) {
		return false;
	}

	// Конус и окружность для прожекторов (топология LINE_LIST, каждая пара индексов — отрезок).
	// Вершина конуса в (0, 0, 0), ось вдоль +Z, окружность радиуса 1 в плоскости z = 1.
	// Матрица модели поворачивает ось вдоль направления прожектора и растягивает радиус
	// до длина * tg(угла), поэтому сами вершины не меняются и лежат в VRAM.
	const int segments = 16;

	std::vector<Vertex> cone_vertices = { { { 0.0f, 0.0f, 0.0f } }, { { 0.0f, 0.0f, 1.0f } } };
	std::vector<uint32_t> cone_indices = { 0, 1 };
	std::vector<Vertex> circle_vertices;
	std::vector<uint32_t> circle_indices;

	for (int k = 0; k < segments; ++k) {
		const float angle = 2.0f * math::pi * float(k) / float(segments);
		const math::Vector3 point = { std::cos(angle), std::sin(angle), 1.0f };
		const uint32_t next = uint32_t((k + 1) % segments);

		cone_vertices.push_back({ point });
		circle_vertices.push_back({ point });

		// окружность основания: отрезок к следующей точке
		cone_indices.push_back(2 + k);
		cone_indices.push_back(2 + next);
		circle_indices.push_back(k);
		circle_indices.push_back(next);

		// 8 образующих: от вершины конуса к каждой второй точке окружности
		if (k % 2 == 0) {
			cone_indices.push_back(0);
			cone_indices.push_back(2 + k);
		}
	}

	if (!createMesh(cone_vertices.data(), uint32_t(cone_vertices.size()),
	                cone_indices.data(), uint32_t(cone_indices.size()), cone_mesh)) {
		return false;
	}

	if (!createMesh(circle_vertices.data(), uint32_t(circle_vertices.size()),
	                circle_indices.data(), uint32_t(circle_indices.size()), circle_mesh)) {
		return false;
	}

	return true;
}

bool createBuffers() {
	auto& context = graphics::internal::context;

	if (!graphics::createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	                            scene_uniform_buffer)) {
		return false;
	}

	if (!graphics::createBuffer(point_light_capacity * sizeof(PointLightData),
	                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, point_light_buffer)) {
		return false;
	}

	if (!graphics::createBuffer(spot_light_capacity * sizeof(SpotLightData),
	                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, spot_light_buffer)) {
		return false;
	}

	// Шаг элементов динамического uniform-буфера: размер ModelUniforms (96 байт),
	// округлённый вверх до minUniformBufferOffsetAlignment (на Intel Iris Xe — 64, шаг 128)
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(context.physical_device, &properties);
	const VkDeviceSize alignment = properties.limits.minUniformBufferOffsetAlignment;
	model_uniform_stride = (sizeof(ModelUniforms) + alignment - 1) / alignment * alignment;

	if (!graphics::createBuffer(model_uniform_capacity * model_uniform_stride,
	                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, model_uniform_buffer)) {
		return false;
	}

	return true;
}

bool createTextures() {
	auto& context = graphics::internal::context;

	for (int i = 0; i < texture_count; ++i) {
		if (!graphics::loadTexture(texture_paths[i], materials[i].texture)) {
			return false;
		}
	}

	// Линейная фильтрация сглаживает текстуру при увеличении и уменьшении,
	// REPEAT повторяет текстуру, если координаты выходят за [0, 1]
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.anisotropyEnable = VK_FALSE,
		.compareEnable = VK_FALSE,
		.minLod = 0.0f,
		.maxLod = 0.0f,
		.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
		.unnormalizedCoordinates = VK_FALSE,
	};

	if (vkCreateSampler(context.device, &sampler_info, nullptr, &vk_sampler) != VK_SUCCESS) {
		std::cerr << "Failed to create sampler\n";
		return false;
	}

	return true;
}

// ЛР4: изображение карты теней и сэмплер со сравнением глубины
bool createShadowResources() {
	auto& context = graphics::internal::context;

	if (!graphics::createDepthTexture(shadow_map_size, shadow_map_size, shadow_map_format, sun_shadow_map)) {
		return false;
	}

	// compareEnable + compareOp = LESS: texture() в шейдере сравнивает глубину фрагмента
	// с глубиной из карты, «глубина фрагмента < глубины в карте» — точка освещена.
	// Линейная фильтрация усредняет результаты сравнения для 4 соседних текселей,
	// поэтому край тени получается мягче. За краем карты — CLAMP_TO_EDGE.
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.anisotropyEnable = VK_FALSE,
		.compareEnable = VK_TRUE,
		.compareOp = VK_COMPARE_OP_LESS,
		.minLod = 0.0f,
		.maxLod = 0.0f,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
		.unnormalizedCoordinates = VK_FALSE,
	};

	if (vkCreateSampler(context.device, &sampler_info, nullptr, &vk_shadow_sampler) != VK_SUCCESS) {
		std::cerr << "Failed to create shadow sampler\n";
		return false;
	}

	return true;
}

// Если элементов стало больше, чем помещается в буфер, создаём новый буфер в 2 раза больше.
// Возвращает true, если буфер был пересоздан.
bool growBuffer(graphics::Buffer& buffer, uint32_t& capacity, size_t count,
                VkDeviceSize element_size, VkBufferUsageFlags usage) {
	if (count <= capacity) {
		return false;
	}

	graphics::destroyBuffer(buffer);

	while (capacity < count) {
		capacity *= 2;
	}

	if (!graphics::createBuffer(capacity * element_size, usage, buffer)) {
		std::cerr << "Failed to grow buffer\n";
	}

	return true;
}

// Записывает в набор сцены дескриптор storage-буфера с источниками света
void writeStorageDescriptor(uint32_t binding, const graphics::Buffer& buffer) {
	const VkDescriptorBufferInfo buffer_descriptor = {
		.buffer = buffer.buffer,
		.offset = 0,
		.range = VK_WHOLE_SIZE,
	};

	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = scene_descriptor_set,
		.dstBinding = binding,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &buffer_descriptor,
	};

	vkUpdateDescriptorSets(graphics::internal::context.device, 1, &write, 0, nullptr);
}

// Записывает в набор материала динамический uniform-буфер свойств моделей.
// range — размер одного элемента: шейдер видит один ModelUniforms по сдвигу из привязки.
void writeModelDescriptor(VkDescriptorSet descriptor_set) {
	const VkDescriptorBufferInfo buffer_descriptor = {
		.buffer = model_uniform_buffer.buffer,
		.offset = 0,
		.range = sizeof(ModelUniforms),
	};

	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptor_set,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		.pBufferInfo = &buffer_descriptor,
	};

	vkUpdateDescriptorSets(graphics::internal::context.device, 1, &write, 0, nullptr);
}

bool createDescriptorSets() {
	auto& context = graphics::internal::context;

	// set = 0: uniform-буфер сцены и storage-буферы с массивами точечных и прожекторных источников.
	// Вершинный шейдер берёт из uniform-буфера view_projection, фрагментный — камеру и свет.
	const VkDescriptorSetLayoutBinding scene_bindings[] = {
		{
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		},
		{
			.binding = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
		{
			.binding = 2,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
		// ЛР4: карта теней солнца вместе с сэмплером сравнения (в шейдере sampler2DShadow)
		{
			.binding = 3,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	};

	const VkDescriptorSetLayoutCreateInfo scene_set_layout = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = sizeof(scene_bindings) / sizeof(scene_bindings[0]),
		.pBindings = scene_bindings,
	};

	if (vkCreateDescriptorSetLayout(context.device, &scene_set_layout, nullptr,
	                                &vk_scene_set_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create scene descriptor set layout\n";
		return false;
	}

	// set = 1: binding 0 — динамический uniform-буфер со свойствами моделей,
	// binding 1 — текстура альбедо вместе с сэмплером (combined image sampler)
	const VkDescriptorSetLayoutBinding material_bindings[] = {
		{
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		},
		{
			.binding = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	};

	const VkDescriptorSetLayoutCreateInfo material_set_layout = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = sizeof(material_bindings) / sizeof(material_bindings[0]),
		.pBindings = material_bindings,
	};

	if (vkCreateDescriptorSetLayout(context.device, &material_set_layout, nullptr,
	                                &vk_material_set_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create material descriptor set layout\n";
		return false;
	}

	// Один набор сцены и по набору на каждый материал (текстуру)
	const VkDescriptorPoolSize descriptor_pool_sizes[] = {
		{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
		},
		{
			.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 2,
		},
		{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
			.descriptorCount = texture_count,
		},
		{
			.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = texture_count + 1, // текстуры материалов + карта теней
		},
	};

	const VkDescriptorPoolCreateInfo descriptor_pool = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1 + texture_count,
		.poolSizeCount = sizeof(descriptor_pool_sizes) / sizeof(descriptor_pool_sizes[0]),
		.pPoolSizes = descriptor_pool_sizes,
	};

	if (vkCreateDescriptorPool(context.device, &descriptor_pool, nullptr,
	                           &vk_descriptor_pool) != VK_SUCCESS) {
		std::cerr << "Failed to create descriptor pool\n";
		return false;
	}

	const VkDescriptorSetAllocateInfo scene_set = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = vk_descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &vk_scene_set_layout,
	};

	if (vkAllocateDescriptorSets(context.device, &scene_set, &scene_descriptor_set) != VK_SUCCESS) {
		std::cerr << "Failed to allocate scene descriptor set\n";
		return false;
	}

	const VkDescriptorBufferInfo scene_buffer_descriptor = {
		.buffer = scene_uniform_buffer.buffer,
		.offset = 0,
		.range = sizeof(SceneUniforms),
	};

	const VkWriteDescriptorSet scene_writes[] = {
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene_descriptor_set,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &scene_buffer_descriptor,
		},
	};

	vkUpdateDescriptorSets(context.device, sizeof(scene_writes) / sizeof(scene_writes[0]),
	                       scene_writes, 0, nullptr);

	writeStorageDescriptor(1, point_light_buffer);
	writeStorageDescriptor(2, spot_light_buffer);

	// Карта теней связана с сэмплером сравнения. Шейдер читает её в раскладке
	// SHADER_READ_ONLY_OPTIMAL: в неё карта переводится барьером после отрисовки.
	const VkDescriptorImageInfo shadow_map_descriptor = {
		.sampler = vk_shadow_sampler,
		.imageView = sun_shadow_map.view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	const VkWriteDescriptorSet shadow_map_write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = scene_descriptor_set,
		.dstBinding = 3,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &shadow_map_descriptor,
	};

	vkUpdateDescriptorSets(context.device, 1, &shadow_map_write, 0, nullptr);

	// Наборы материалов отличаются только текстурой, динамический uniform-буфер у них общий
	for (Material& material : materials) {
		const VkDescriptorSetAllocateInfo material_set = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = vk_descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &vk_material_set_layout,
		};

		if (vkAllocateDescriptorSets(context.device, &material_set, &material.descriptor_set) != VK_SUCCESS) {
			std::cerr << "Failed to allocate material descriptor set\n";
			return false;
		}

		writeModelDescriptor(material.descriptor_set);

		// Кортеж изображение-сэмплер: шейдер читает текстуру в раскладке SHADER_READ_ONLY_OPTIMAL
		const VkDescriptorImageInfo image_descriptor = {
			.sampler = vk_sampler,
			.imageView = material.texture.view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};

		const VkWriteDescriptorSet image_write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = material.descriptor_set,
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image_descriptor,
		};

		vkUpdateDescriptorSets(context.device, 1, &image_write, 0, nullptr);
	}

	return true;
}

// Конвейеров три. Они отличаются шейдерами, топологией (треугольники или линии)
// и отбрасыванием граней, остальное у них общее.
bool createPipeline(const char vertex_shader_path[], const char fragment_shader_path[],
                    VkPrimitiveTopology topology, VkCullModeFlags cull_mode, VkPipeline& result) {
	auto& context = graphics::internal::context;

	VkShaderModule vertex_shader = graphics::loadShaderModule(vertex_shader_path);
	VkShaderModule fragment_shader = graphics::loadShaderModule(fragment_shader_path);

	const VkPipelineShaderStageCreateInfo stages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertex_shader,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragment_shader,
			.pName = "main",
		},
	};

	const VkVertexInputBindingDescription vertex_bindings[] = {
		{
			.binding = 0,
			.stride = sizeof(Vertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		},
	};

	const VkVertexInputAttributeDescription vertex_attributes[] = {
		{
			.location = 0,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Vertex, position),
		},
		{
			.location = 1,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Vertex, normal),
		},
		{
			.location = 2,
			.binding = 0,
			.format = VK_FORMAT_R32G32_SFLOAT,
			.offset = offsetof(Vertex, uv),
		},
	};

	const VkPipelineVertexInputStateCreateInfo vertex_input_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = sizeof(vertex_bindings) / sizeof(vertex_bindings[0]),
		.pVertexBindingDescriptions = vertex_bindings,
		.vertexAttributeDescriptionCount = sizeof(vertex_attributes) / sizeof(vertex_attributes[0]),
		.pVertexAttributeDescriptions = vertex_attributes,
	};

	const VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = topology,
	};

	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	const VkPipelineRasterizationStateCreateInfo rasterization_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = cull_mode,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};

	const VkPipelineMultisampleStateCreateInfo multisample_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	const VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};

	const VkPipelineColorBlendAttachmentState color_blend_attachment = {
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};

	const VkPipelineColorBlendStateCreateInfo color_blend_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &color_blend_attachment,
	};

	const VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};

	const VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]),
		.pDynamicStates = dynamic_states,
	};

	const VkGraphicsPipelineCreateInfo pipeline = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = sizeof(stages) / sizeof(stages[0]),
		.pStages = stages,
		.pVertexInputState = &vertex_input_state,
		.pInputAssemblyState = &input_assembly_state,
		.pViewportState = &viewport_state,
		.pRasterizationState = &rasterization_state,
		.pMultisampleState = &multisample_state,
		.pDepthStencilState = &depth_stencil_state,
		.pColorBlendState = &color_blend_state,
		.pDynamicState = &dynamic_state,
		.layout = vk_pipeline_layout,
		.renderPass = context.render_pass,
		.subpass = 0,
	};

	const VkResult status = vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &pipeline,
	                                                  nullptr, &result);

	vkDestroyShaderModule(context.device, vertex_shader, nullptr);
	vkDestroyShaderModule(context.device, fragment_shader, nullptr);

	if (status != VK_SUCCESS) {
		std::cerr << "Failed to create graphics pipeline\n";
		return false;
	}

	return true;
}

// ЛР4: конвейер карты теней. Рисует без VkRenderPass и VkFramebuffer (динамический рендеринг):
// вместо render pass в конвейере указан формат вложения глубины (VkPipelineRenderingCreateInfo).
// Шейдер только вершинный — он проецирует геометрию с точки зрения источника, а записывается
// только глубина, поэтому фрагментный шейдер и цветовые вложения не нужны.
bool createShadowPipeline() {
	auto& context = graphics::internal::context;

	VkShaderModule vertex_shader = graphics::loadShaderModule("shaders/shadow.vert.spv");

	const VkPipelineShaderStageCreateInfo stage = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_VERTEX_BIT,
		.module = vertex_shader,
		.pName = "main",
	};

	const VkVertexInputBindingDescription vertex_binding = {
		.binding = 0,
		.stride = sizeof(Vertex),
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};

	// Для тени нужна только позиция вершины
	const VkVertexInputAttributeDescription vertex_attribute = {
		.location = 0,
		.binding = 0,
		.format = VK_FORMAT_R32G32B32_SFLOAT,
		.offset = offsetof(Vertex, position),
	};

	const VkPipelineVertexInputStateCreateInfo vertex_input_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &vertex_binding,
		.vertexAttributeDescriptionCount = 1,
		.pVertexAttributeDescriptions = &vertex_attribute,
	};

	const VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};

	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	// depthBiasEnable: к глубине каждого треугольника добавляется сдвиг
	// constant * (минимальный шаг глубины) + slope * (наклон треугольника)
	const VkPipelineRasterizationStateCreateInfo rasterization_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.depthBiasEnable = VK_TRUE,
		.lineWidth = 1.0f,
	};

	const VkPipelineMultisampleStateCreateInfo multisample_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	const VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};

	// Цветовых вложений нет
	const VkPipelineColorBlendStateCreateInfo color_blend_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 0,
	};

	// Сдвиг глубины задаётся командой vkCmdSetDepthBias, чтобы менять его из интерфейса
	const VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_DEPTH_BIAS,
	};

	const VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]),
		.pDynamicStates = dynamic_states,
	};

	// Динамический рендеринг: формат вложения глубины вместо render pass
	const VkPipelineRenderingCreateInfo rendering_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 0,
		.depthAttachmentFormat = shadow_map_format,
	};

	const VkGraphicsPipelineCreateInfo pipeline = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering_info,
		.stageCount = 1,
		.pStages = &stage,
		.pVertexInputState = &vertex_input_state,
		.pInputAssemblyState = &input_assembly_state,
		.pViewportState = &viewport_state,
		.pRasterizationState = &rasterization_state,
		.pMultisampleState = &multisample_state,
		.pDepthStencilState = &depth_stencil_state,
		.pColorBlendState = &color_blend_state,
		.pDynamicState = &dynamic_state,
		.layout = vk_pipeline_layout,
		.renderPass = VK_NULL_HANDLE,
	};

	const VkResult status = vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &pipeline,
	                                                  nullptr, &vk_shadow_pipeline);

	vkDestroyShaderModule(context.device, vertex_shader, nullptr);

	if (status != VK_SUCCESS) {
		std::cerr << "Failed to create shadow pipeline\n";
		return false;
	}

	return true;
}

bool createPipelines() {
	auto& context = graphics::internal::context;

	// Номер описания в массиве — это номер set в шейдере
	const VkDescriptorSetLayout set_layouts[] = {
		vk_scene_set_layout,
		vk_material_set_layout,
	};

	// Диапазон push-констант для фрагментного шейдера (8 байт)
	const VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(SamplingConstants),
	};

	const VkPipelineLayoutCreateInfo pipeline_layout = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = sizeof(set_layouts) / sizeof(set_layouts[0]),
		.pSetLayouts = set_layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range,
	};

	if (vkCreatePipelineLayout(context.device, &pipeline_layout, nullptr,
	                           &vk_pipeline_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create pipeline layout\n";
		return false;
	}

	if (!createPipeline("shaders/basic.vert.spv", "shaders/basic.frag.spv",
	                    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_CULL_MODE_BACK_BIT, vk_pipeline)) {
		return false;
	}

	if (!createPipeline("shaders/light.vert.spv", "shaders/light.frag.spv",
	                    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_CULL_MODE_NONE, vk_light_pipeline)) {
		return false;
	}

	if (!createPipeline("shaders/light.vert.spv", "shaders/light.frag.spv",
	                    VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_CULL_MODE_NONE, vk_line_pipeline)) {
		return false;
	}

	if (!createShadowPipeline()) {
		return false;
	}

	return true;
}

// Нормирует направление источника. Нулевой вектор из интерфейса заменяется направлением вниз.
math::Vector3 normalizeDirection(math::Vector3 direction) {
	if (math::length(direction) < 0.001f) {
		return { 0.0f, 1.0f, 0.0f };
	}
	return math::normalize(direction);
}

// Матрица поворота, переводящая ось +Z в направление прожектора. Её столбцы —
// два перпендикулярных оси вектора и само направление.
math::Matrix4 spotRotation(math::Vector3 direction) {
	// cross параллельных векторов равен нулю, поэтому для почти вертикальной оси берём другой helper
	const math::Vector3 helper = std::fabs(direction.y) < 0.99f ? math::Vector3{ 0.0f, 1.0f, 0.0f }
	                                                            : math::Vector3{ 1.0f, 0.0f, 0.0f };
	const math::Vector3 side = math::normalize(math::cross(direction, helper));
	const math::Vector3 up = math::cross(direction, side);

	math::Matrix4 result = math::identity();
	result.at(0, 0) = side.x;      result.at(1, 0) = side.y;      result.at(2, 0) = side.z;
	result.at(0, 1) = up.x;        result.at(1, 1) = up.y;        result.at(2, 1) = up.z;
	result.at(0, 2) = direction.x; result.at(1, 2) = direction.y; result.at(2, 2) = direction.z;
	return result;
}

// Движение камеры с клавиатуры: WASD — вперёд/влево/назад/вправо, Q/E — вниз/вверх,
// стрелки — поворот. Направления берутся из матрицы поворота камеры Ry(yaw) * Rx(pitch).
void moveCamera(float delta_time) {
	if (ImGui::GetIO().WantCaptureKeyboard) {
		return;
	}

	const float yaw = math::radians(camera_yaw);
	const float pitch = math::radians(camera_pitch);

	const math::Vector3 forward = {
		std::sin(yaw) * std::cos(pitch),
		-std::sin(pitch),
		std::cos(yaw) * std::cos(pitch),
	};
	const math::Vector3 right = { std::cos(yaw), 0.0f, -std::sin(yaw) };
	const math::Vector3 up = { 0.0f, -1.0f, 0.0f };

	math::Vector3 move = { 0.0f, 0.0f, 0.0f };
	if (ImGui::IsKeyDown(ImGuiKey_W)) move = move + forward;
	if (ImGui::IsKeyDown(ImGuiKey_S)) move = move - forward;
	if (ImGui::IsKeyDown(ImGuiKey_D)) move = move + right;
	if (ImGui::IsKeyDown(ImGuiKey_A)) move = move - right;
	if (ImGui::IsKeyDown(ImGuiKey_E)) move = move + up;
	if (ImGui::IsKeyDown(ImGuiKey_Q)) move = move - up;

	camera_position = camera_position + move * (camera_speed * delta_time);

	const float rotation_speed = 90.0f; // градусов в секунду
	if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) camera_yaw -= rotation_speed * delta_time;
	if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) camera_yaw += rotation_speed * delta_time;
	if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) camera_pitch += rotation_speed * delta_time;
	if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) camera_pitch -= rotation_speed * delta_time;

	if (camera_yaw > 180.0f) camera_yaw -= 360.0f;
	if (camera_yaw < -180.0f) camera_yaw += 360.0f;
	camera_pitch = std::fmin(std::fmax(camera_pitch, -89.0f), 89.0f);
}

// Привязывает набор материала со сдвигом на элемент index динамического uniform-буфера
void bindMaterial(VkCommandBuffer command_buffer, VkDescriptorSet descriptor_set, uint32_t index) {
	const uint32_t dynamic_offset = uint32_t(index * model_uniform_stride);
	vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout,
	                        1, 1, &descriptor_set, 1, &dynamic_offset);
}

void bindMesh(VkCommandBuffer command_buffer, const Mesh& mesh) {
	const VkDeviceSize vertex_buffer_offset = 0;
	vkCmdBindVertexBuffers(command_buffer, 0, 1, &mesh.vertex_buffer.buffer, &vertex_buffer_offset);
	vkCmdBindIndexBuffer(command_buffer, mesh.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
}

// ЛР4: отрисовка карты теней динамическим рендерингом (без VkRenderPass и VkFramebuffer)
void renderShadowMap(VkCommandBuffer command_buffer, const graphics::Texture& shadow_map) {
	const VkImageSubresourceRange depth_range = {
		.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};

	// Барьер 1: прошлое содержимое не нужно (UNDEFINED), карта становится вложением глубины.
	// Запись глубины начнётся только после того, как прошлый кадр закончил её читать.
	const VkImageMemoryBarrier to_attachment = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = shadow_map.image,
		.subresourceRange = depth_range,
	};
	vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &to_attachment);

	// Вложение глубины для динамического рендеринга: очистить до 1.0 и сохранить результат
	const VkRenderingAttachmentInfo depth_attachment = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = shadow_map.view,
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = { .depthStencil = { 1.0f, 0 } },
	};

	const VkRenderingInfo rendering_info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = { .extent = { shadow_map_size, shadow_map_size } },
		.layerCount = 1,
		.colorAttachmentCount = 0,
		.pDepthAttachment = &depth_attachment,
	};

	vkCmdBeginRendering(command_buffer, &rendering_info);

	const VkViewport viewport = {
		.x = 0.0f,
		.y = 0.0f,
		.width = float(shadow_map_size),
		.height = float(shadow_map_size),
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	const VkRect2D scissor = { .extent = { shadow_map_size, shadow_map_size } };

	vkCmdSetViewport(command_buffer, 0, 1, &viewport);
	vkCmdSetScissor(command_buffer, 0, 1, &scissor);
	vkCmdSetDepthBias(command_buffer, depth_bias_constant, 0.0f, depth_bias_slope);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_shadow_pipeline);
	vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout,
	                        0, 1, &scene_descriptor_set, 0, nullptr);

	// Та же сцена, что и в основном проходе: объекты с теми же элементами динамического буфера.
	// Фигуры источников в карту теней не рисуются.
	for (int i = 0; i < object_count; ++i) {
		bindMesh(command_buffer, *objects[i].mesh);
		bindMaterial(command_buffer, materials[objects[i].texture].descriptor_set, uint32_t(i));
		vkCmdDrawIndexed(command_buffer, objects[i].mesh->index_count, 1, 0, 0, 0);
	}

	vkCmdEndRendering(command_buffer);

	// Барьер 2: запись глубины закончена, карту будет читать фрагментный шейдер основного прохода
	const VkImageMemoryBarrier to_shader_read = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = shadow_map.image,
		.subresourceRange = depth_range,
	};
	vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &to_shader_read);
}

} // namespace

bool initialize() {
	if (!createMeshes()) {
		return false;
	}

	if (!createBuffers()) {
		return false;
	}

	if (!createTextures()) {
		return false;
	}

	if (!createShadowResources()) {
		return false;
	}

	if (!createDescriptorSets()) {
		return false;
	}

	if (!createPipelines()) {
		return false;
	}

	return true;
}

void shutdown() {
	auto& context = graphics::internal::context;
	vkQueueWaitIdle(context.graphics_queue);

	vkDestroyPipeline(context.device, vk_shadow_pipeline, nullptr);
	vkDestroyPipeline(context.device, vk_line_pipeline, nullptr);
	vkDestroyPipeline(context.device, vk_light_pipeline, nullptr);
	vkDestroyPipeline(context.device, vk_pipeline, nullptr);
	vkDestroyPipelineLayout(context.device, vk_pipeline_layout, nullptr);

	vkDestroyDescriptorPool(context.device, vk_descriptor_pool, nullptr);
	vkDestroyDescriptorSetLayout(context.device, vk_material_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(context.device, vk_scene_set_layout, nullptr);

	vkDestroySampler(context.device, vk_shadow_sampler, nullptr);
	graphics::destroyTexture(sun_shadow_map);

	vkDestroySampler(context.device, vk_sampler, nullptr);
	for (const Material& material : materials) {
		graphics::destroyTexture(material.texture);
	}

	graphics::destroyBuffer(model_uniform_buffer);
	graphics::destroyBuffer(spot_light_buffer);
	graphics::destroyBuffer(point_light_buffer);
	graphics::destroyBuffer(scene_uniform_buffer);

	destroyMesh(circle_mesh);
	destroyMesh(cone_mesh);
	destroyMesh(cube_mesh);
	destroyMesh(floor_mesh);
	destroyMesh(icosahedron_mesh);
}

void update(double time) {
	const float delta_time = float(time - last_time);
	last_time = time;

	ImGui::SetNextWindowSize(ImVec2(420.0f, 640.0f), ImGuiCond_FirstUseEver);
	ImGui::Begin("Lab 4");

	if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat3("Position##camera", &camera_position.x, -20.0f, 20.0f);
		ImGui::SliderFloat("Yaw", &camera_yaw, -180.0f, 180.0f);
		ImGui::SliderFloat("Pitch", &camera_pitch, -89.0f, 89.0f);
		ImGui::SliderFloat("Speed##camera", &camera_speed, 0.5f, 10.0f);

		ImGui::RadioButton("Perspective", &projection_type, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Orthographic", &projection_type, 1);
		if (projection_type == 0) {
			ImGui::SliderFloat("FOV", &fov_degrees, 30.0f, 120.0f);
		} else {
			ImGui::SliderFloat("Size", &ortho_size, 2.0f, 20.0f);
		}

		ImGui::TextDisabled("WASD, Q/E - move, arrows - rotate");
	}

	if (ImGui::CollapsingHeader("Directional light", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat3("Direction##directional", &directional_light_direction.x, -1.0f, 1.0f);
		ImGui::ColorEdit3("Color##directional", &directional_light_color.x);
		ImGui::SliderFloat("Intensity##directional", &directional_light_intensity, 0.0f, 5.0f);
	}

	if (ImGui::CollapsingHeader("Ambient light", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::ColorEdit3("Color##ambient", &ambient_light_color.x);
		ImGui::SliderFloat("Intensity##ambient", &ambient_light_intensity, 0.0f, 1.0f);
	}

	// ЛР4: площадь, которую покрывает ортографическая проекция солнца, и сдвиг глубины
	if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Sun shadow area", &sun_shadow_area, 4.0f, 60.0f);
		ImGui::SliderFloat("Bias constant", &depth_bias_constant, 0.0f, 10.0f);
		ImGui::SliderFloat("Bias slope", &depth_bias_slope, 0.0f, 10.0f);
	}

	ImGui::Checkbox("Show light sources", &show_light_sources);

	if (ImGui::CollapsingHeader("Point lights")) {
		if (ImGui::Button("Add point light")) {
			point_lights.push_back({ { 0.0f, 0.5f, 0.0f }, { 1.0f, 1.0f, 1.0f }, 3.0f });
		}

		int remove_index = -1;

		for (int i = 0; i < int(point_lights.size()); ++i) {
			PointLight& light = point_lights[i];

			// PushID разделяет одинаковые надписи разных источников
			ImGui::PushID(i);
			if (ImGui::TreeNode("point", "Point light %d", i)) {
				ImGui::SliderFloat3("Position", &light.position.x, -10.0f, 10.0f);
				ImGui::ColorEdit3("Color", &light.color.x);
				ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 20.0f);
				if (ImGui::Button("Remove")) {
					remove_index = i;
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		if (remove_index >= 0) {
			point_lights.erase(point_lights.begin() + remove_index);
		}
	}

	if (ImGui::CollapsingHeader("Spot lights")) {
		if (ImGui::Button("Add spot light")) {
			spot_lights.push_back({ { 0.0f, -3.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 1.0f },
			                        20.0f, 15.0f, 25.0f });
		}

		int remove_index = -1;

		for (int i = 0; i < int(spot_lights.size()); ++i) {
			SpotLight& light = spot_lights[i];

			ImGui::PushID(i);
			if (ImGui::TreeNode("spot", "Spot light %d", i)) {
				ImGui::SliderFloat3("Position", &light.position.x, -10.0f, 10.0f);
				ImGui::SliderFloat3("Direction", &light.direction.x, -1.0f, 1.0f);
				ImGui::ColorEdit3("Color", &light.color.x);
				ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 50.0f);
				ImGui::SliderFloat("Inner angle", &light.inner_angle, 0.0f, 80.0f);
				ImGui::SliderFloat("Outer angle", &light.outer_angle, 0.0f, 80.0f);
				if (ImGui::Button("Remove")) {
					remove_index = i;
				}
				ImGui::TreePop();
			}
			ImGui::PopID();

			// Внешний угол не может быть меньше внутреннего. Если они равны, край будет резким.
			if (light.outer_angle < light.inner_angle) {
				light.outer_angle = light.inner_angle;
			}
		}

		if (remove_index >= 0) {
			spot_lights.erase(spot_lights.begin() + remove_index);
		}
	}

	if (ImGui::CollapsingHeader("Objects", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderInt("Selected", &selected_object, 0, object_count - 1,
		                 selected_object == object_count - 1 ? "Floor" : "Icosahedron %d");

		Object& selected = objects[selected_object];
		ImGui::SliderFloat3("Position##object", &selected.position.x, -5.0f, 5.0f);
		ImGui::SliderFloat3("Rotation", &selected.rotation.x, -180.0f, 180.0f);
		ImGui::SliderFloat3("Scale", &selected.scale.x, 0.1f, 10.0f);

		// Выбор текстуры для выбранного объекта из выпадающего списка
		ImGui::Combo("Texture", &selected.texture, texture_names, texture_count);

		// Включение нетривиального сэмплирования для выбранного объекта
		ImGui::Checkbox("Animated texture", &selected.texture_effect);
		ImGui::ColorEdit3("Specular", &selected.specular.x);
		ImGui::SliderFloat("Shininess", &selected.shininess, 1.0f, 1024.0f, "%.0f",
		                   ImGuiSliderFlags_Logarithmic);
	}

	if (ImGui::CollapsingHeader("Animation")) {
		if (ImGui::Button(animation_playing ? "Pause" : "Play")) {
			animation_playing = !animation_playing;
		}
		ImGui::SliderFloat("Speed##animation", &animation_speed, 0.0f, 5.0f);
		ImGui::SliderFloat("Radius", &trajectory_radius, 0.0f, 5.0f);
		ImGui::SliderFloat("Wave height", &trajectory_height, 0.0f, 2.0f);
	}

	ImGui::End();

	moveCamera(delta_time);

	effect_time = float(time);

	if (animation_playing) {
		animation_time += delta_time * animation_speed;
	}

	// Матрица вида — обратная к матрице камеры C = T(position) * Ry(yaw) * Rx(pitch),
	// поэтому множители идут в обратном порядке и с противоположными знаками
	const math::Matrix4 view =
		math::rotationX(-math::radians(camera_pitch)) *
		math::rotationY(-math::radians(camera_yaw)) *
		math::translation(-camera_position.x, -camera_position.y, -camera_position.z);

	const VkExtent2D extent = graphics::internal::context.swapchain_extent;
	const float aspect = float(extent.width) / float(extent.height);

	math::Matrix4 projection;
	if (projection_type == 0) {
		projection = math::perspective(math::radians(fov_degrees), aspect, z_near, z_far);
	} else {
		projection = math::orthographic(ortho_size * aspect, ortho_size, z_near, z_far);
	}

	// Направление света нормируем на CPU, чтобы не делать это для каждого пикселя
	scene_uniforms.view_projection = projection * view;
	scene_uniforms.camera_position = camera_position;
	scene_uniforms.directional_light_direction = normalizeDirection(directional_light_direction);
	scene_uniforms.directional_light_color = directional_light_color * directional_light_intensity;
	scene_uniforms.ambient_light = ambient_light_color * ambient_light_intensity;
	scene_uniforms.point_light_count = uint32_t(point_lights.size());
	scene_uniforms.spot_light_count = uint32_t(spot_lights.size());

	// ЛР4: матрица солнца для карты теней. У направленного источника нет позиции, поэтому
	// «камера» солнца ставится на расстоянии sun_distance от центра сцены против направления лучей
	// и смотрит на центр (матрица LookAt). Лучи параллельны, поэтому проекция ортографическая:
	// она покрывает квадрат sun_shadow_area x sun_shadow_area и глубину [0.1, 2 * sun_distance].
	const float sun_distance = 25.0f;
	const math::Vector3 scene_center = { 0.0f, 0.0f, 0.0f };
	const math::Vector3 sun_direction = scene_uniforms.directional_light_direction;
	const math::Vector3 sun_position = scene_center - sun_direction * sun_distance;

	// «Вверх» не должен совпадать с направлением взгляда: для вертикального солнца берём ось Z
	const math::Vector3 sun_up = std::fabs(sun_direction.y) < 0.99f ? math::Vector3{ 0.0f, -1.0f, 0.0f }
	                                                                : math::Vector3{ 0.0f, 0.0f, 1.0f };

	scene_uniforms.sun_shadow_matrix =
		math::orthographic(sun_shadow_area, sun_shadow_area, 0.1f, 2.0f * sun_distance) *
		math::lookAt(sun_position, scene_center, sun_up);

	// Свойства всех моделей собираются в один массив. Порядок: объекты, кубы точечных
	// источников, кубы прожекторов, затем для каждого прожектора конус и окружность.
	// В render() отрисовки идут в том же порядке, индекс элемента задаёт сдвиг в буфере.
	model_uniforms.clear();

	for (int i = 0; i < object_count; ++i) {
		const Object& object = objects[i];

		math::Vector3 offset = { 0.0f, 0.0f, 0.0f };
		float t = 0.0f;

		if (object.animated) {
			t = animation_time + 2.0f * math::pi * float(i) / 3.0f;
			offset = {
				trajectory_radius * std::cos(t),
				trajectory_height * std::sin(3.0f * t),
				trajectory_radius * std::sin(t),
			};
		}

		const math::Vector3 position = object.position + offset;

		const math::Matrix4 model =
			math::translation(position.x, position.y, position.z) *
			math::rotationZ(math::radians(object.rotation.z)) *
			math::rotationY(math::radians(object.rotation.y) + t) *
			math::rotationX(math::radians(object.rotation.x) + t * 0.5f) *
			math::scale(object.scale.x, object.scale.y, object.scale.z);

		model_uniforms.push_back({ .model = model, .specular = object.specular, .alpha = object.shininess });
	}

	if (show_light_sources) {
		const math::Matrix4 cube_scale = math::scale(0.15f, 0.15f, 0.15f);

		for (const PointLight& light : point_lights) {
			const math::Vector3 p = light.position;
			model_uniforms.push_back({ .model = math::translation(p.x, p.y, p.z) * cube_scale, .color = light.color });
		}

		for (const SpotLight& light : spot_lights) {
			const math::Vector3 p = light.position;
			model_uniforms.push_back({ .model = math::translation(p.x, p.y, p.z) * cube_scale, .color = light.color });
		}

		// Конус длиной 1.5: радиус основания 1.5 * tg(угла). Цвет линий белый.
		const float cone_length = 1.5f;
		for (const SpotLight& light : spot_lights) {
			const math::Matrix4 placement =
				math::translation(light.position.x, light.position.y, light.position.z) *
				spotRotation(normalizeDirection(light.direction));
			const float outer_radius = cone_length * std::tan(math::radians(light.outer_angle));
			const float inner_radius = cone_length * std::tan(math::radians(light.inner_angle));

			model_uniforms.push_back({ .model = placement * math::scale(outer_radius, outer_radius, cone_length),
			                           .color = { 1.0f, 1.0f, 1.0f } });
			model_uniforms.push_back({ .model = placement * math::scale(inner_radius, inner_radius, cone_length),
			                           .color = { 1.0f, 1.0f, 1.0f } });
		}
	}
}

void render(const graphics::internal::FrameData& fd) {
	auto& context = graphics::internal::context;

	memcpy(scene_uniform_buffer.memory, &scene_uniforms, sizeof(scene_uniforms));

	// Если источников больше, чем помещается в storage-буфер, буфер пересоздаётся и набор сцены
	// переписывается. Это безопасно: prepare() уже дождался, пока GPU закончит прошлый кадр.
	if (growBuffer(point_light_buffer, point_light_capacity, point_lights.size(),
	               sizeof(PointLightData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
		writeStorageDescriptor(1, point_light_buffer);
	}

	// Цвет и интенсивность передаются одним вектором: color * intensity
	PointLightData* point_light_data = static_cast<PointLightData*>(point_light_buffer.memory);
	for (size_t i = 0; i < point_lights.size(); ++i) {
		point_light_data[i].position = point_lights[i].position;
		point_light_data[i].color = point_lights[i].color * point_lights[i].intensity;
	}

	if (growBuffer(spot_light_buffer, spot_light_capacity, spot_lights.size(),
	               sizeof(SpotLightData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
		writeStorageDescriptor(2, spot_light_buffer);
	}

	SpotLightData* spot_light_data = static_cast<SpotLightData*>(spot_light_buffer.memory);
	for (size_t i = 0; i < spot_lights.size(); ++i) {
		const SpotLight& light = spot_lights[i];

		spot_light_data[i].position = light.position;
		spot_light_data[i].color = light.color * light.intensity;
		spot_light_data[i].direction = normalizeDirection(light.direction);
		spot_light_data[i].inner_cutoff = std::cos(math::radians(light.inner_angle));
		spot_light_data[i].outer_cutoff = std::cos(math::radians(light.outer_angle));
	}

	// Динамический uniform-буфер тоже растёт; тогда в наборе материала переписывается дескриптор
	if (growBuffer(model_uniform_buffer, model_uniform_capacity, model_uniforms.size(),
	               model_uniform_stride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
		for (const Material& material : materials) {
			writeModelDescriptor(material.descriptor_set);
		}
	}

	// Элемент i лежит по сдвигу i * stride
	for (size_t i = 0; i < model_uniforms.size(); ++i) {
		memcpy(static_cast<char*>(model_uniform_buffer.memory) + i * model_uniform_stride,
		       &model_uniforms[i], sizeof(ModelUniforms));
	}

	vkResetCommandBuffer(fd.command_buffer, 0);

	const VkCommandBufferBeginInfo command_buffer_begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	vkBeginCommandBuffer(fd.command_buffer, &command_buffer_begin);

	// ЛР4: сначала карта теней (своим проходом динамического рендеринга), потом основная сцена
	renderShadowMap(fd.command_buffer, sun_shadow_map);

	const VkClearValue clear_values[] = {
		{ .color = { .float32 = { 0.02f, 0.02f, 0.03f, 1.0f } } },
		{ .depthStencil = { 1.0f, 0 } },
	};

	const VkRenderPassBeginInfo render_pass_begin = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = context.render_pass,
		.framebuffer = fd.framebuffer,
		.renderArea = { .extent = context.swapchain_extent },
		.clearValueCount = sizeof(clear_values) / sizeof(clear_values[0]),
		.pClearValues = clear_values,
	};

	vkCmdBeginRenderPass(fd.command_buffer, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);

	const VkViewport viewport = {
		.x = 0.0f,
		.y = 0.0f,
		.width = float(context.swapchain_extent.width),
		.height = float(context.swapchain_extent.height),
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};

	const VkRect2D scissor = { .extent = context.swapchain_extent };

	vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
	vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);

	vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);

	// set = 0 общий для всех объектов, поэтому привязывается один раз за кадр
	vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout,
	                        0, 1, &scene_descriptor_set, 0, nullptr);

	// Набор дескрипторов — материал (текстура) объекта, сдвиг — его элемент в динамическом буфере
	uint32_t index = 0;
	for (const Object& object : objects) {
		bindMesh(fd.command_buffer, *object.mesh);
		bindMaterial(fd.command_buffer, materials[object.texture].descriptor_set, index++);

		// Флаг эффекта и время записываются в командный буфер перед отрисовкой
		const SamplingConstants constants = {
			.enabled = object.texture_effect ? 1u : 0u,
			.time = effect_time,
		};
		vkCmdPushConstants(fd.command_buffer, vk_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
		                   sizeof(constants), &constants);

		vkCmdDrawIndexed(fd.command_buffer, object.mesh->index_count, 1, 0, 0, 0);
	}

	// Визуализация источников: отдельные конвейеры с постоянным цветом, без освещения.
	// Pipeline layout у всех конвейеров один, поэтому set = 0 остаётся привязанным.
	// Текстуру эти шейдеры не читают, поэтому подходит набор любого материала.
	if (show_light_sources) {
		vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_light_pipeline);
		bindMesh(fd.command_buffer, cube_mesh);

		for (size_t i = 0; i < point_lights.size() + spot_lights.size(); ++i) {
			bindMaterial(fd.command_buffer, materials[0].descriptor_set, index++);
			vkCmdDrawIndexed(fd.command_buffer, cube_mesh.index_count, 1, 0, 0, 0);
		}

		vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_line_pipeline);

		for (size_t i = 0; i < spot_lights.size(); ++i) {
			bindMesh(fd.command_buffer, cone_mesh);
			bindMaterial(fd.command_buffer, materials[0].descriptor_set, index++);
			vkCmdDrawIndexed(fd.command_buffer, cone_mesh.index_count, 1, 0, 0, 0);

			bindMesh(fd.command_buffer, circle_mesh);
			bindMaterial(fd.command_buffer, materials[0].descriptor_set, index++);
			vkCmdDrawIndexed(fd.command_buffer, circle_mesh.index_count, 1, 0, 0, 0);
		}
	}

	vkCmdEndRenderPass(fd.command_buffer);

	vkEndCommandBuffer(fd.command_buffer);
}

} // namespace application
