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

struct Vertex {
	math::Vector3 position;
	math::Vector3 normal;
};

// Доп. задание 1: данные разделены на два набора дескрипторов.
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
};

// set = 1: матрица модели и материал одного объекта
struct ModelUniforms {
	math::Matrix4 model;    // 0
	math::Vector3 albedo;   // 64
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

// Доп. задание 2: точечный источник в storage-буфере. Раскладка std430 совпадает
// со struct PointLight в шейдере: vec3 выравнивается на 16 байт, размер структуры 32 байта.
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

// Доп. задание 3: прожекторный источник в storage-буфере, раскладка std430.
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

// Доп. задание 4: матрица и цвет фигуры-визуализации источника. Передаются push-константами
// прямо в командный буфер перед каждой отрисовкой, потому что число источников всё время меняется.
struct LightMarkerConstants {
	math::Matrix4 model; // 0
	float color[4];      // 64
};

// Геометрия в памяти видеокарты: вершины и индексы одной фигуры
struct Mesh {
	graphics::Buffer vertex_buffer;
	graphics::Buffer index_buffer;
	uint32_t index_count;
};

Mesh icosahedron_mesh;
Mesh floor_mesh;
Mesh cube_mesh;

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
float directional_light_intensity = 0.5f;

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

	// Материал по модели Блинна-Фонга
	math::Vector3 albedo;
	math::Vector3 specular;
	float shininess;

	ModelUniforms uniforms;
	graphics::Buffer uniform_buffer;
	VkDescriptorSet descriptor_set;
};

constexpr int object_count = 4;

// Три икосаэдра из ЛР1 и пол (квадрат 2x2, растянутый в 8 раз)
Object objects[object_count] = {
	{ .mesh = &icosahedron_mesh, .animated = true, .scale = { 0.6f, 0.6f, 0.6f },
	  .albedo = { 0.9f, 0.9f, 0.9f }, .specular = { 1.0f, 1.0f, 1.0f }, .shininess = 32.0f },
	{ .mesh = &icosahedron_mesh, .animated = true, .scale = { 0.6f, 0.6f, 0.6f },
	  .albedo = { 0.9f, 0.2f, 0.2f }, .specular = { 1.0f, 1.0f, 1.0f }, .shininess = 128.0f },
	{ .mesh = &icosahedron_mesh, .animated = true, .scale = { 0.6f, 0.6f, 0.6f },
	  .albedo = { 0.2f, 0.4f, 0.9f }, .specular = { 0.5f, 0.5f, 0.5f }, .shininess = 8.0f },
	{ .mesh = &floor_mesh, .animated = false, .position = { 0.0f, 2.0f, 0.0f }, .scale = { 8.0f, 1.0f, 8.0f },
	  .albedo = { 0.6f, 0.6f, 0.6f }, .specular = { 0.2f, 0.2f, 0.2f }, .shininess = 16.0f },
};

int selected_object = 0;

SceneUniforms scene_uniforms;
graphics::Buffer scene_uniform_buffer;
VkDescriptorSet scene_descriptor_set;

// Storage-буфер растёт вместе с числом источников, capacity — сколько источников в нём помещается
graphics::Buffer point_light_buffer;
uint32_t point_light_capacity = 4;

graphics::Buffer spot_light_buffer;
uint32_t spot_light_capacity = 4;

// Доп. задание 4: конусы прожекторов рисуются линиями, вершины пересчитываются каждый кадр
bool show_light_sources = true;
std::vector<Vertex> cone_vertices;
graphics::Buffer cone_vertex_buffer;
uint32_t cone_vertex_capacity = 256;

VkDescriptorSetLayout vk_scene_set_layout;
VkDescriptorSetLayout vk_model_set_layout;
VkDescriptorPool vk_descriptor_pool;

VkPipelineLayout vk_pipeline_layout;
VkPipeline vk_pipeline;

VkPipelineLayout vk_light_pipeline_layout;
VkPipeline vk_light_pipeline; // кубы источников
VkPipeline vk_line_pipeline;  // конусы прожекторов

bool createMesh(const Vertex vertices[], uint32_t vertex_count,
                const uint32_t indices[], uint32_t index_count, Mesh& mesh) {
	if (!graphics::createBuffer(vertex_count * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                            mesh.vertex_buffer)) {
		return false;
	}
	memcpy(mesh.vertex_buffer.memory, vertices, vertex_count * sizeof(Vertex));

	if (!graphics::createBuffer(index_count * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
	                            mesh.index_buffer)) {
		return false;
	}
	memcpy(mesh.index_buffer.memory, indices, index_count * sizeof(uint32_t));

	mesh.index_count = index_count;
	return true;
}

void destroyMesh(const Mesh& mesh) {
	graphics::destroyBuffer(mesh.index_buffer);
	graphics::destroyBuffer(mesh.vertex_buffer);
}

bool createBuffers() {
	// Нормали икосаэдра считаются процедурно: нормаль грани — векторное произведение
	// двух её рёбер. Чтобы у каждой грани была своя нормаль, вершины между гранями
	// не делятся: 20 граней по 3 вершины.
	Vertex icosahedron_vertices[icosahedron_index_count];
	uint32_t indices[icosahedron_index_count];

	for (uint32_t i = 0; i < icosahedron_index_count; i += 3) {
		const math::Vector3 a = icosahedron_positions[icosahedron_indices[i + 0]];
		const math::Vector3 b = icosahedron_positions[icosahedron_indices[i + 1]];
		const math::Vector3 c = icosahedron_positions[icosahedron_indices[i + 2]];

		// Обход против часовой стрелки снаружи, поэтому нормаль смотрит наружу
		const math::Vector3 normal = math::normalize(math::cross(b - a, c - a));

		icosahedron_vertices[i + 0] = { a, normal };
		icosahedron_vertices[i + 1] = { b, normal };
		icosahedron_vertices[i + 2] = { c, normal };

		indices[i + 0] = i + 0;
		indices[i + 1] = i + 1;
		indices[i + 2] = i + 2;
	}

	if (!createMesh(icosahedron_vertices, icosahedron_index_count, indices, icosahedron_index_count,
	                icosahedron_mesh)) {
		return false;
	}

	// Пол — квадрат в плоскости y = 0. Ось Y направлена вниз, поэтому нормаль (0, -1, 0).
	const Vertex floor_vertices[] = {
		{ { -1.0f, 0.0f, -1.0f }, { 0.0f, -1.0f, 0.0f } },
		{ {  1.0f, 0.0f, -1.0f }, { 0.0f, -1.0f, 0.0f } },
		{ {  1.0f, 0.0f,  1.0f }, { 0.0f, -1.0f, 0.0f } },
		{ { -1.0f, 0.0f,  1.0f }, { 0.0f, -1.0f, 0.0f } },
	};

	const uint32_t floor_indices[] = { 0, 1, 2, 0, 2, 3 };

	if (!createMesh(floor_vertices, 4, floor_indices, 6, floor_mesh)) {
		return false;
	}

	// Доп. задание 4: куб для визуализации источников. Он не освещается, поэтому нормали не нужны.
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

	if (!graphics::createBuffer(cone_vertex_capacity * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                            cone_vertex_buffer)) {
		return false;
	}

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

	for (Object& object : objects) {
		if (!graphics::createBuffer(sizeof(ModelUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                            object.uniform_buffer)) {
			return false;
		}
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

	// set = 1: uniform-буфер модели. Вершинный шейдер берёт матрицу модели,
	// фрагментный — материал.
	const VkDescriptorSetLayoutBinding model_bindings[] = {
		{
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	};

	const VkDescriptorSetLayoutCreateInfo model_set_layout = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = sizeof(model_bindings) / sizeof(model_bindings[0]),
		.pBindings = model_bindings,
	};

	if (vkCreateDescriptorSetLayout(context.device, &model_set_layout, nullptr,
	                                &vk_model_set_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create model descriptor set layout\n";
		return false;
	}

	// Один набор сцены и по набору на каждый объект
	const VkDescriptorPoolSize descriptor_pool_sizes[] = {
		{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1 + object_count,
		},
		{
			.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 2,
		},
	};

	const VkDescriptorPoolCreateInfo descriptor_pool = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1 + object_count,
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

	for (Object& object : objects) {
		const VkDescriptorSetAllocateInfo model_set = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = vk_descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &vk_model_set_layout,
		};

		if (vkAllocateDescriptorSets(context.device, &model_set, &object.descriptor_set) != VK_SUCCESS) {
			std::cerr << "Failed to allocate model descriptor set\n";
			return false;
		}

		const VkDescriptorBufferInfo model_buffer_descriptor = {
			.buffer = object.uniform_buffer.buffer,
			.offset = 0,
			.range = sizeof(ModelUniforms),
		};

		const VkWriteDescriptorSet model_writes[] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = object.descriptor_set,
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo = &model_buffer_descriptor,
			},
		};

		vkUpdateDescriptorSets(context.device, sizeof(model_writes) / sizeof(model_writes[0]),
		                       model_writes, 0, nullptr);
	}

	return true;
}

// Доп. задание 4: конвейеров теперь три. Они отличаются шейдерами, pipeline layout,
// топологией (треугольники или линии) и отбрасыванием граней, остальное у них общее.
bool createPipeline(const char vertex_shader_path[], const char fragment_shader_path[],
                    VkPipelineLayout layout, VkPrimitiveTopology topology,
                    VkCullModeFlags cull_mode, VkPipeline& result) {
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
		.layout = layout,
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

bool createPipelines() {
	auto& context = graphics::internal::context;

	// Номер описания в массиве — это номер set в шейдере
	const VkDescriptorSetLayout set_layouts[] = {
		vk_scene_set_layout,
		vk_model_set_layout,
	};

	const VkPipelineLayoutCreateInfo pipeline_layout = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = sizeof(set_layouts) / sizeof(set_layouts[0]),
		.pSetLayouts = set_layouts,
	};

	if (vkCreatePipelineLayout(context.device, &pipeline_layout, nullptr,
	                           &vk_pipeline_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create pipeline layout\n";
		return false;
	}

	// Доп. задание 4: визуализации источников нужен только set = 0 (view_projection),
	// а матрица и цвет фигуры приходят в push-константах
	const VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0,
		.size = sizeof(LightMarkerConstants),
	};

	const VkPipelineLayoutCreateInfo light_pipeline_layout = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &vk_scene_set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range,
	};

	if (vkCreatePipelineLayout(context.device, &light_pipeline_layout, nullptr,
	                           &vk_light_pipeline_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create light pipeline layout\n";
		return false;
	}

	if (!createPipeline("shaders/basic.vert.spv", "shaders/basic.frag.spv", vk_pipeline_layout,
	                    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_CULL_MODE_BACK_BIT, vk_pipeline)) {
		return false;
	}

	if (!createPipeline("shaders/light.vert.spv", "shaders/light.frag.spv", vk_light_pipeline_layout,
	                    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_CULL_MODE_NONE, vk_light_pipeline)) {
		return false;
	}

	if (!createPipeline("shaders/light.vert.spv", "shaders/light.frag.spv", vk_light_pipeline_layout,
	                    VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_CULL_MODE_NONE, vk_line_pipeline)) {
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

// Доп. задание 4: маленький куб цвета источника в его позиции
void drawLightCube(VkCommandBuffer command_buffer, math::Vector3 position, math::Vector3 color) {
	const LightMarkerConstants constants = {
		.model = math::translation(position.x, position.y, position.z) * math::scale(0.15f, 0.15f, 0.15f),
		.color = { color.x, color.y, color.z, 1.0f },
	};

	vkCmdPushConstants(command_buffer, vk_light_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
	                   sizeof(constants), &constants);
	vkCmdDrawIndexed(command_buffer, cube_mesh.index_count, 1, 0, 0, 0);
}

} // namespace

bool initialize() {
	if (!createBuffers()) {
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

	vkDestroyPipeline(context.device, vk_line_pipeline, nullptr);
	vkDestroyPipeline(context.device, vk_light_pipeline, nullptr);
	vkDestroyPipeline(context.device, vk_pipeline, nullptr);
	vkDestroyPipelineLayout(context.device, vk_light_pipeline_layout, nullptr);
	vkDestroyPipelineLayout(context.device, vk_pipeline_layout, nullptr);

	vkDestroyDescriptorPool(context.device, vk_descriptor_pool, nullptr);
	vkDestroyDescriptorSetLayout(context.device, vk_model_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(context.device, vk_scene_set_layout, nullptr);

	for (const Object& object : objects) {
		graphics::destroyBuffer(object.uniform_buffer);
	}
	graphics::destroyBuffer(cone_vertex_buffer);
	graphics::destroyBuffer(spot_light_buffer);
	graphics::destroyBuffer(point_light_buffer);
	graphics::destroyBuffer(scene_uniform_buffer);
	destroyMesh(cube_mesh);
	destroyMesh(floor_mesh);
	destroyMesh(icosahedron_mesh);
}

void update(double time) {
	const float delta_time = float(time - last_time);
	last_time = time;

	ImGui::SetNextWindowSize(ImVec2(420.0f, 640.0f), ImGuiCond_FirstUseEver);
	ImGui::Begin("Lab 2");

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

	// Доп. задание 4: показать или скрыть фигуры источников
	ImGui::Checkbox("Show light sources", &show_light_sources);

	// Доп. задание 2: добавление, удаление и параметры точечных источников
	if (ImGui::CollapsingHeader("Point lights", ImGuiTreeNodeFlags_DefaultOpen)) {
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

	// Доп. задание 3: добавление, удаление и параметры прожекторов
	if (ImGui::CollapsingHeader("Spot lights", ImGuiTreeNodeFlags_DefaultOpen)) {
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

		ImGui::ColorEdit3("Albedo", &selected.albedo.x);
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

	if (animation_playing) {
		animation_time += delta_time * animation_speed;
	}

	// Матрица вида — обратная к матрице камеры C = T(position) * Ry(yaw) * Rx(pitch),
	// поэтому множители идут в обратном порядке и с противоположными знаками (лекция 4)
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

	for (int i = 0; i < object_count; ++i) {
		Object& object = objects[i];

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

		object.uniforms.model = model;
		object.uniforms.albedo = object.albedo;
		object.uniforms.specular = object.specular;
		object.uniforms.alpha = object.shininess;
	}

	// Доп. задание 4: конусы прожекторов из белых линий. Вершины считаются сразу в мировых
	// координатах: ось конуса, 8 образующих и окружности внешнего и внутреннего углов.
	cone_vertices.clear();

	for (const SpotLight& light : spot_lights) {
		const math::Vector3 direction = normalizeDirection(light.direction);

		// Два вектора, перпендикулярных оси, задают плоскость окружности основания конуса
		const math::Vector3 helper = std::fabs(direction.y) < 0.99f ? math::Vector3{ 0.0f, 1.0f, 0.0f }
		                                                            : math::Vector3{ 1.0f, 0.0f, 0.0f };
		const math::Vector3 side = math::normalize(math::cross(direction, helper));
		const math::Vector3 up = math::cross(direction, side);

		const float cone_length = 1.5f;
		const math::Vector3 center = light.position + direction * cone_length;
		const float outer_radius = cone_length * std::tan(math::radians(light.outer_angle));
		const float inner_radius = cone_length * std::tan(math::radians(light.inner_angle));

		cone_vertices.push_back({ light.position });
		cone_vertices.push_back({ center });

		const int segments = 16;
		for (int k = 0; k < segments; ++k) {
			const float angle0 = 2.0f * math::pi * float(k) / float(segments);
			const float angle1 = 2.0f * math::pi * float(k + 1) / float(segments);
			const math::Vector3 offset0 = side * std::cos(angle0) + up * std::sin(angle0);
			const math::Vector3 offset1 = side * std::cos(angle1) + up * std::sin(angle1);

			cone_vertices.push_back({ center + offset0 * outer_radius });
			cone_vertices.push_back({ center + offset1 * outer_radius });

			cone_vertices.push_back({ center + offset0 * inner_radius });
			cone_vertices.push_back({ center + offset1 * inner_radius });

			if (k % 2 == 0) {
				cone_vertices.push_back({ light.position });
				cone_vertices.push_back({ center + offset0 * outer_radius });
			}
		}
	}
}

void render(const graphics::internal::FrameData& fd) {
	auto& context = graphics::internal::context;

	memcpy(scene_uniform_buffer.memory, &scene_uniforms, sizeof(scene_uniforms));

	// Доп. задание 2: если источников больше, чем помещается в storage-буфер, буфер пересоздаётся
	// и набор сцены переписывается. Это безопасно: prepare() уже дождался, пока GPU закончит
	// прошлый кадр, и старый буфер никто не читает.
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

	// Доп. задание 3: то же самое для прожекторов
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

	// Доп. задание 4: вершины конусов, буфер растёт так же, как storage-буферы
	if (!cone_vertices.empty()) {
		growBuffer(cone_vertex_buffer, cone_vertex_capacity, cone_vertices.size(), sizeof(Vertex),
		           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
		memcpy(cone_vertex_buffer.memory, cone_vertices.data(), cone_vertices.size() * sizeof(Vertex));
	}

	for (const Object& object : objects) {
		memcpy(object.uniform_buffer.memory, &object.uniforms, sizeof(object.uniforms));
	}

	vkResetCommandBuffer(fd.command_buffer, 0);

	const VkCommandBufferBeginInfo command_buffer_begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	vkBeginCommandBuffer(fd.command_buffer, &command_buffer_begin);

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

	for (const Object& object : objects) {
		const VkDeviceSize vertex_buffer_offset = 0;
		vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &object.mesh->vertex_buffer.buffer,
		                       &vertex_buffer_offset);
		vkCmdBindIndexBuffer(fd.command_buffer, object.mesh->index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

		// set = 1 меняется перед каждым объектом, set = 0 при этом остаётся привязанным
		vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout,
		                        1, 1, &object.descriptor_set, 0, nullptr);

		vkCmdDrawIndexed(fd.command_buffer, object.mesh->index_count, 1, 0, 0, 0);
	}

	// Доп. задание 4: визуализация источников отдельным конвейером без освещения
	if (show_light_sources) {
		vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_light_pipeline);

		// У этого конвейера другой pipeline layout (с push-константами), поэтому
		// набор сцены нужно привязать заново
		vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_light_pipeline_layout,
		                        0, 1, &scene_descriptor_set, 0, nullptr);

		const VkDeviceSize vertex_buffer_offset = 0;
		vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &cube_mesh.vertex_buffer.buffer, &vertex_buffer_offset);
		vkCmdBindIndexBuffer(fd.command_buffer, cube_mesh.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

		for (const PointLight& light : point_lights) {
			drawLightCube(fd.command_buffer, light.position, light.color);
		}

		for (const SpotLight& light : spot_lights) {
			drawLightCube(fd.command_buffer, light.position, light.color);
		}

		if (!cone_vertices.empty()) {
			vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_line_pipeline);

			// Вершины конусов уже в мировых координатах, поэтому матрица модели единичная
			const LightMarkerConstants constants = {
				.model = math::identity(),
				.color = { 1.0f, 1.0f, 1.0f, 1.0f },
			};

			vkCmdPushConstants(fd.command_buffer, vk_light_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
			                   sizeof(constants), &constants);

			vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &cone_vertex_buffer.buffer, &vertex_buffer_offset);
			vkCmdDraw(fd.command_buffer, uint32_t(cone_vertices.size()), 1, 0, 0);
		}
	}

	vkCmdEndRenderPass(fd.command_buffer);

	vkEndCommandBuffer(fd.command_buffer);
}

} // namespace application
