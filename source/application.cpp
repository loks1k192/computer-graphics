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
};

// Данные для шейдеров. Раскладка должна совпадать с uniform-блоком
// GlobalUniforms в shaders/basic.vert по правилам std140: mat4 занимает 64 байта
struct GlobalUniforms {
	math::Matrix4 matrix; // projection * view * model
};

// Правильный икосаэдр: 12 вершин и 20 граней (правильных треугольников).
// Его вершины — это углы трёх взаимно перпендикулярных "золотых" прямоугольников
// со сторонами 2 и 2 * phi, где phi — золотое сечение.
const float phi = (1.0f + std::sqrt(5.0f)) / 2.0f;

const Vertex vertices[] = {
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

graphics::Buffer vertex_buffer;
graphics::Buffer index_buffer;
graphics::Buffer uniform_buffer;

VkDescriptorSetLayout vk_descriptor_set_layout;
VkDescriptorPool vk_descriptor_pool;
VkDescriptorSet vk_descriptor_set;

VkPipelineLayout vk_pipeline_layout;
VkPipeline vk_pipeline;

// Значения uniform-буфера на CPU: считаются в update(), копируются на GPU в render()
GlobalUniforms uniforms;

bool createBuffers() {
	if (!graphics::createBuffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer)) {
		return false;
	}
	memcpy(vertex_buffer.memory, vertices, sizeof(vertices));

	if (!graphics::createBuffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer)) {
		return false;
	}
	memcpy(index_buffer.memory, indices, sizeof(indices));

	// Uniform-буфер заполняется каждый кадр
	return graphics::createBuffer(sizeof(GlobalUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	                              uniform_buffer);
}

bool createDescriptorSet() {
	auto& context = graphics::internal::context;

	// В наборе один дескриптор: uniform-буфер в binding = 0, его читает вершинный шейдер
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

	// Из пула выделяется один набор, в котором один uniform-буфер
	const VkDescriptorPoolSize descriptor_pool_sizes[] = {
		{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
		},
	};

	const VkDescriptorPoolCreateInfo descriptor_pool = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = sizeof(descriptor_pool_sizes) / sizeof(descriptor_pool_sizes[0]),
		.pPoolSizes = descriptor_pool_sizes,
	};

	if (vkCreateDescriptorPool(context.device, &descriptor_pool, nullptr,
	                           &vk_descriptor_pool) != VK_SUCCESS) {
		std::cerr << "Failed to create descriptor pool\n";
		return false;
	}

	const VkDescriptorSetAllocateInfo descriptor_set = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = vk_descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &vk_descriptor_set_layout,
	};

	if (vkAllocateDescriptorSets(context.device, &descriptor_set,
	                             &vk_descriptor_set) != VK_SUCCESS) {
		std::cerr << "Failed to allocate descriptor set\n";
		return false;
	}

	// Связываем uniform-буфер с binding = 0 выделенного набора
	const VkDescriptorBufferInfo uniform_buffer_descriptor = {
		.buffer = uniform_buffer.buffer,
		.offset = 0,
		.range = sizeof(GlobalUniforms),
	};

	const VkWriteDescriptorSet descriptor_writes[] = {
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = vk_descriptor_set,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &uniform_buffer_descriptor,
		},
	};

	vkUpdateDescriptorSets(context.device, sizeof(descriptor_writes) / sizeof(descriptor_writes[0]),
	                       descriptor_writes, 0, nullptr);

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

	// Атрибут location = 0 в шейдере — это позиция, три float
	const VkVertexInputAttributeDescription vertex_attributes[] = {
		{
			.location = 0,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Vertex, position),
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

	if (!createDescriptorSet()) {
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

	graphics::destroyBuffer(uniform_buffer);
	graphics::destroyBuffer(index_buffer);
	graphics::destroyBuffer(vertex_buffer);
}

void update(double time) {
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

	ImGui::End();

	// Модель: фигура вращается со временем
	const math::Matrix4 model = math::rotationY(float(time)) * math::rotationX(float(time) * 0.5f);

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

	uniforms.matrix = projection * view * model;
}

void render(const graphics::internal::FrameData& fd) {
	auto& context = graphics::internal::context;

	// Копируем данные именно здесь: prepare() уже дождался, пока GPU закончит
	// предыдущий кадр, поэтому uniform-буфер сейчас никто не читает
	memcpy(uniform_buffer.memory, &uniforms, sizeof(uniforms));

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

	vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout,
	                        0, 1, &vk_descriptor_set, 0, nullptr);

	vkCmdDrawIndexed(fd.command_buffer, index_count, 1, 0, 0, 0);

	vkCmdEndRenderPass(fd.command_buffer);

	vkEndCommandBuffer(fd.command_buffer);
}

} // namespace application
