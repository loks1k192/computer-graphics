#include "application.hpp"

#include <cmath>
#include <cstring>
#include <iostream>

#include <imgui.h>

#include "graphics.hpp"
#include "math.hpp"

namespace application {

namespace {

struct Vertex {
	float position[3];
	float color[3];
};

// Данные одного объекта для шейдеров. Раскладка должна совпадать с uniform-блоком
// ObjectUniforms в shaders/basic.vert по правилам std140: mat4 занимает
// байты 0..63, vec4 — байты 64..79, поэтому выравнивающие байты не нужны.
// Для цвета взят vec4, а не vec3, чтобы не думать о выравнивании vec3 в std140.
struct ObjectUniforms {
	math::Matrix4 matrix; // projection * view * model
	float color[4];
};

// Правильный икосаэдр: 12 вершин и 20 граней (правильных треугольников).
// Его вершины — это углы трёх взаимно перпендикулярных "золотых" прямоугольников
// со сторонами 2 и 2 * phi, где phi — золотое сечение.
// Цвета вершин здесь не заданы, они считаются в createBuffers().
const float phi = (1.0f + std::sqrt(5.0f)) / 2.0f;

Vertex vertices[] = {
	{ { -1.0f,  phi,  0.0f } },
	{ {  1.0f,  phi,  0.0f } },
	{ { -1.0f, -phi,  0.0f } },
	{ {  1.0f, -phi,  0.0f } },

	{ {  0.0f, -1.0f,  phi } },
	{ {  0.0f,  1.0f,  phi } },
	{ {  0.0f, -1.0f, -phi } },
	{ {  0.0f,  1.0f, -phi } },

	{ {  phi,  0.0f, -1.0f } },
	{ {  phi,  0.0f,  1.0f } },
	{ { -phi,  0.0f, -1.0f } },
	{ { -phi,  0.0f,  1.0f } },
};

// Каждые три индекса — одна грань. Вершины каждой грани перечислены
// против часовой стрелки, если смотреть на грань снаружи фигуры.
const uint32_t indices[] = {
	0, 11, 5,   0, 5, 1,    0, 1, 7,    0, 7, 10,   0, 10, 11,
	1, 5, 9,    5, 11, 4,   11, 10, 2,  10, 7, 6,   7, 1, 8,
	3, 9, 4,    3, 4, 2,    3, 2, 6,    3, 6, 8,    3, 8, 9,
	4, 9, 5,    2, 4, 11,   6, 2, 10,   8, 6, 7,    9, 8, 1,
};

constexpr uint32_t index_count = sizeof(indices) / sizeof(indices[0]);

// Камера стоит в начале координат и смотрит вдоль +Z
constexpr float camera_distance = 6.0f;
constexpr float z_near = 0.1f;
constexpr float z_far = 100.0f;

// Параметры, которые меняются через интерфейс

// Доп. задание 1: тип проекции (0 — перспективная, 1 — ортографическая)
int projection_type = 0;
float fov_degrees = 60.0f; // вертикальный угол обзора перспективной проекции
float ortho_size = 7.0f;   // высота видимой области ортографической проекции

// Доп. задание 3: анимация движения по траектории
bool animation_playing = true;
float animation_speed = 1.0f;
float trajectory_radius = 2.5f;
float trajectory_height = 0.5f;
float animation_time = 0.0f; // собственное время анимации, на паузе не идёт
double last_time = 0.0;

// Доп. задание 6: на сцене несколько икосаэдров. Все они рисуются из одних и тех же
// вершинного и индексного буферов, но у каждого объекта свои параметры, свой
// uniform-буфер (матрица и цвет) и свой набор дескрипторов, который на него указывает.
// Один общий uniform-буфер не подойдёт: GPU выполнит команды позже, уже после их записи,
// и все объекты нарисовались бы с данными, которые были записаны в буфер последними.
struct Object {
	// Доп. задание 2: позиция, поворот (в градусах) и растяжение
	float position[3];
	float rotation[3];
	float scale[3];

	// Доп. задание 4: цвет
	float color[3];

	ObjectUniforms uniforms; // считаются в update(), копируются на GPU в render()
	graphics::Buffer uniform_buffer;
	VkDescriptorSet descriptor_set;
};

constexpr int object_count = 3;

Object objects[object_count] = {
	{ .scale = { 0.6f, 0.6f, 0.6f }, .color = { 1.0f, 1.0f, 1.0f } },
	{ .scale = { 0.6f, 0.6f, 0.6f }, .color = { 1.0f, 0.4f, 0.4f } },
	{ .scale = { 0.6f, 0.6f, 0.6f }, .color = { 0.4f, 0.6f, 1.0f } },
};

int selected_object = 0; // объект, параметры которого сейчас меняются в интерфейсе

graphics::Buffer vertex_buffer;
graphics::Buffer index_buffer;

VkDescriptorSetLayout vk_descriptor_set_layout;
VkDescriptorPool vk_descriptor_pool;

VkPipelineLayout vk_pipeline_layout;
VkPipeline vk_pipeline;

bool createBuffers() {
	// Доп. задание 5: цвет вершины зависит от её позиции в локальной системе координат.
	// Каждая координата переводится из [-radius, radius] в [0, 1] и становится R, G или B.
	const float radius = std::sqrt(1.0f + phi * phi); // расстояние от центра до вершины

	for (Vertex& vertex : vertices) {
		for (int i = 0; i < 3; ++i) {
			vertex.color[i] = vertex.position[i] / radius * 0.5f + 0.5f;
		}
	}

	if (!graphics::createBuffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer)) {
		return false;
	}
	memcpy(vertex_buffer.memory, vertices, sizeof(vertices));

	if (!graphics::createBuffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer)) {
		return false;
	}
	memcpy(index_buffer.memory, indices, sizeof(indices));

	// Доп. задание 6: у каждого объекта свой uniform-буфер, он заполняется каждый кадр
	for (Object& object : objects) {
		if (!graphics::createBuffer(sizeof(ObjectUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                            object.uniform_buffer)) {
			return false;
		}
	}

	return true;
}

bool createDescriptorSets() {
	auto& context = graphics::internal::context;

	// В наборе один дескриптор: uniform-буфер в binding = 0, его читает вершинный шейдер.
	// Наборы всех объектов устроены одинаково, поэтому описание (layout) у них одно.
	const VkDescriptorSetLayoutBinding descriptor_set_bindings[] = {
		{
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		},
	};

	const VkDescriptorSetLayoutCreateInfo descriptor_set_layout = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = sizeof(descriptor_set_bindings) / sizeof(descriptor_set_bindings[0]),
		.pBindings = descriptor_set_bindings,
	};

	if (vkCreateDescriptorSetLayout(context.device, &descriptor_set_layout, nullptr,
	                                &vk_descriptor_set_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create descriptor set layout\n";
		return false;
	}

	// Доп. задание 6: из пула выделяется object_count наборов, в каждом один uniform-буфер,
	// поэтому всего в пуле нужно object_count * 1 дескрипторов типа UNIFORM_BUFFER
	const VkDescriptorPoolSize descriptor_pool_sizes[] = {
		{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = object_count,
		},
	};

	const VkDescriptorPoolCreateInfo descriptor_pool = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = object_count,
		.poolSizeCount = sizeof(descriptor_pool_sizes) / sizeof(descriptor_pool_sizes[0]),
		.pPoolSizes = descriptor_pool_sizes,
	};

	if (vkCreateDescriptorPool(context.device, &descriptor_pool, nullptr,
	                           &vk_descriptor_pool) != VK_SUCCESS) {
		std::cerr << "Failed to create descriptor pool\n";
		return false;
	}

	// Каждому объекту — свой набор, который указывает на uniform-буфер этого объекта
	for (Object& object : objects) {
		const VkDescriptorSetAllocateInfo descriptor_set = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = vk_descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &vk_descriptor_set_layout,
		};

		if (vkAllocateDescriptorSets(context.device, &descriptor_set,
		                             &object.descriptor_set) != VK_SUCCESS) {
			std::cerr << "Failed to allocate descriptor set\n";
			return false;
		}

		// Связываем uniform-буфер объекта с binding = 0 его набора
		const VkDescriptorBufferInfo uniform_buffer_descriptor = {
			.buffer = object.uniform_buffer.buffer,
			.offset = 0,
			.range = sizeof(ObjectUniforms),
		};

		const VkWriteDescriptorSet descriptor_writes[] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = object.descriptor_set,
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo = &uniform_buffer_descriptor,
			},
		};

		vkUpdateDescriptorSets(context.device, sizeof(descriptor_writes) / sizeof(descriptor_writes[0]),
		                       descriptor_writes, 0, nullptr);
	}

	return true;
}

bool createPipeline() {
	auto& context = graphics::internal::context;

	// Конвейер должен знать, какие наборы дескрипторов будут к нему привязаны
	const VkPipelineLayoutCreateInfo pipeline_layout = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &vk_descriptor_set_layout,
	};

	if (vkCreatePipelineLayout(context.device, &pipeline_layout, nullptr,
	                           &vk_pipeline_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create pipeline layout\n";
		return false;
	}

	VkShaderModule vertex_shader = graphics::loadShaderModule("shaders/basic.vert.spv");
	VkShaderModule fragment_shader = graphics::loadShaderModule("shaders/basic.frag.spv");

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

	// Вершины лежат в одном буфере (binding = 0) подряд, по sizeof(Vertex) байт каждая
	const VkVertexInputBindingDescription vertex_bindings[] = {
		{
			.binding = 0,
			.stride = sizeof(Vertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		},
	};

	// Атрибуты вершины: location = 0 — позиция, location = 1 — цвет, оба по три float
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
			.offset = offsetof(Vertex, color),
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
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};

	// Сами размеры viewport и scissor задаются при записи команд (динамически)
	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	// Грани заданы против часовой стрелки, поэтому лицевая сторона — COUNTER_CLOCKWISE.
	// Задние (отвёрнутые от камеры) грани отбрасываются.
	const VkPipelineRasterizationStateCreateInfo rasterization_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};

	const VkPipelineMultisampleStateCreateInfo multisample_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	// Тест глубины: пиксель рисуется, только если он ближе уже нарисованного
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

	const VkResult result = vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &pipeline,
	                                                  nullptr, &vk_pipeline);

	// Шейдерные модули нужны только для создания конвейера
	vkDestroyShaderModule(context.device, vertex_shader, nullptr);
	vkDestroyShaderModule(context.device, fragment_shader, nullptr);

	if (result != VK_SUCCESS) {
		std::cerr << "Failed to create graphics pipeline\n";
		return false;
	}

	return true;
}

} // namespace

bool initialize() {
	if (!createBuffers()) {
		return false;
	}

	if (!createDescriptorSets()) {
		return false;
	}

	if (!createPipeline()) {
		return false;
	}

	return true;
}

void shutdown() {
	auto& context = graphics::internal::context;
	vkQueueWaitIdle(context.graphics_queue);

	vkDestroyPipeline(context.device, vk_pipeline, nullptr);
	vkDestroyPipelineLayout(context.device, vk_pipeline_layout, nullptr);

	// Наборы дескрипторов освобождаются вместе с пулом
	vkDestroyDescriptorPool(context.device, vk_descriptor_pool, nullptr);
	vkDestroyDescriptorSetLayout(context.device, vk_descriptor_set_layout, nullptr);

	for (const Object& object : objects) {
		graphics::destroyBuffer(object.uniform_buffer);
	}
	graphics::destroyBuffer(index_buffer);
	graphics::destroyBuffer(vertex_buffer);
}

void update(double time) {
	// Время, прошедшее с прошлого кадра
	const float delta_time = float(time - last_time);
	last_time = time;

	ImGui::Begin("Icosahedron");

	ImGui::SeparatorText("Projection");
	ImGui::RadioButton("Perspective", &projection_type, 0);
	ImGui::SameLine();
	ImGui::RadioButton("Orthographic", &projection_type, 1);
	if (projection_type == 0) {
		ImGui::SliderFloat("FOV", &fov_degrees, 30.0f, 120.0f);
	} else {
		ImGui::SliderFloat("Size", &ortho_size, 2.0f, 20.0f);
	}

	ImGui::SeparatorText("Animation");
	if (ImGui::Button(animation_playing ? "Pause" : "Play")) {
		animation_playing = !animation_playing;
	}
	ImGui::SliderFloat("Speed", &animation_speed, 0.0f, 5.0f);
	ImGui::SliderFloat("Radius", &trajectory_radius, 0.0f, 5.0f);
	ImGui::SliderFloat("Wave height", &trajectory_height, 0.0f, 2.0f);

	// Доп. задание 6: сначала выбираем объект, потом меняем его параметры
	ImGui::SeparatorText("Object");
	ImGui::SliderInt("Selected", &selected_object, 0, object_count - 1);

	Object& selected = objects[selected_object];
	ImGui::SliderFloat3("Position", selected.position, -3.0f, 3.0f);
	ImGui::SliderFloat3("Rotation", selected.rotation, -180.0f, 180.0f);
	ImGui::SliderFloat3("Scale", selected.scale, 0.1f, 3.0f);
	ImGui::ColorEdit3("Color", selected.color);

	ImGui::End();

	// Доп. задание 3: время анимации идёт, только когда она не на паузе
	if (animation_playing) {
		animation_time += delta_time * animation_speed;
	}

	// Вид: камера смотрит вдоль +Z, поэтому отодвигаем всю сцену от камеры
	const math::Matrix4 view = math::translation(0.0f, 0.0f, camera_distance);

	// Доп. задание 1: выбор матрицы проекции
	const VkExtent2D extent = graphics::internal::context.swapchain_extent;
	const float aspect = float(extent.width) / float(extent.height);

	math::Matrix4 projection;
	if (projection_type == 0) {
		projection = math::perspective(math::radians(fov_degrees), aspect, z_near, z_far);
	} else {
		projection = math::orthographic(ortho_size * aspect, ortho_size, z_near, z_far);
	}

	// Вид и проекция общие для всей сцены, а матрица модели и цвет у каждого объекта свои
	for (int i = 0; i < object_count; ++i) {
		Object& object = objects[i];

		// Траектория: окружность радиуса trajectory_radius в плоскости XZ,
		// вдоль которой фигура ещё и колеблется по Y (3 волны за один оборот).
		// Во время движения фигура вращается вокруг своих осей Y и X.
		// Объекты сдвинуты по фазе на 2 * pi / object_count, поэтому идут
		// по окружности на одинаковом расстоянии друг от друга.
		const float t = animation_time + 2.0f * math::pi * float(i) / float(object_count);
		const float offset_x = trajectory_radius * std::cos(t);
		const float offset_y = trajectory_height * std::sin(3.0f * t);
		const float offset_z = trajectory_radius * std::sin(t);

		// Модель: сначала растяжение, потом поворот, потом перемещение
		// (матрицы применяются к вершине справа налево)
		const math::Matrix4 model =
			math::translation(object.position[0] + offset_x, object.position[1] + offset_y,
			                  object.position[2] + offset_z) *
			math::rotationZ(math::radians(object.rotation[2])) *
			math::rotationY(math::radians(object.rotation[1]) + t) *
			math::rotationX(math::radians(object.rotation[0]) + t * 0.5f) *
			math::scale(object.scale[0], object.scale[1], object.scale[2]);

		object.uniforms.matrix = projection * view * model;

		// Доп. задание 4: цвет из интерфейса уходит в шейдер через uniform-буфер
		object.uniforms.color[0] = object.color[0];
		object.uniforms.color[1] = object.color[1];
		object.uniforms.color[2] = object.color[2];
		object.uniforms.color[3] = 1.0f;
	}
}

void render(const graphics::internal::FrameData& fd) {
	auto& context = graphics::internal::context;

	// Копируем данные именно здесь: prepare() уже дождался, пока GPU закончит
	// предыдущий кадр, поэтому uniform-буферы сейчас никто не читает
	for (const Object& object : objects) {
		memcpy(object.uniform_buffer.memory, &object.uniforms, sizeof(object.uniforms));
	}

	vkResetCommandBuffer(fd.command_buffer, 0);

	const VkCommandBufferBeginInfo command_buffer_begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	vkBeginCommandBuffer(fd.command_buffer, &command_buffer_begin);

	// Чем очистить изображения: цвет фона и глубина 1 (самое дальнее значение)
	const VkClearValue clear_values[] = {
		{ .color = { .float32 = { 0.1f, 0.1f, 0.1f, 1.0f } } },
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

	const VkDeviceSize vertex_buffer_offset = 0;
	vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertex_buffer.buffer, &vertex_buffer_offset);
	vkCmdBindIndexBuffer(fd.command_buffer, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

	// Доп. задание 6: все объекты рисуются из одних и тех же вершин и индексов, но перед
	// каждой отрисовкой привязывается набор дескрипторов объекта, поэтому вершинный шейдер
	// получает матрицу и цвет именно этого объекта
	for (const Object& object : objects) {
		vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout,
		                        0, 1, &object.descriptor_set, 0, nullptr);

		vkCmdDrawIndexed(fd.command_buffer, index_count, 1, 0, 0, 0);
	}

	vkCmdEndRenderPass(fd.command_buffer);

	vkEndCommandBuffer(fd.command_buffer);
}

} // namespace application
