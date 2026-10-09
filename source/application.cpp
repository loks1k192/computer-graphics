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


struct ObjectUniforms {
	math::Matrix4 matrix; // projection * view * model
	float color[4];
};


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


const uint32_t indices[] = {
	0, 11, 5,   0, 5, 1,    0, 1, 7,    0, 7, 10,   0, 10, 11,
	1, 5, 9,    5, 11, 4,   11, 10, 2,  10, 7, 6,   7, 1, 8,
	3, 9, 4,    3, 4, 2,    3, 2, 6,    3, 6, 8,    3, 8, 9,
	4, 9, 5,    2, 4, 11,   6, 2, 10,   8, 6, 7,    9, 8, 1,
};

constexpr uint32_t index_count = sizeof(indices) / sizeof(indices[0]);

constexpr float camera_distance = 6.0f;
constexpr float z_near = 0.1f;
constexpr float z_far = 100.0f;




int projection_type = 0;
float fov_degrees = 60.0f;
float ortho_size = 7.0f;

bool animation_playing = true;
float animation_speed = 1.0f;
float trajectory_radius = 2.5f;
float trajectory_height = 0.5f;
float animation_time = 0.0f;
double last_time = 0.0;


struct Object {
	float position[3];
	float rotation[3];
	float scale[3];

	float color[3];

	ObjectUniforms uniforms;
	graphics::Buffer uniform_buffer;
	VkDescriptorSet descriptor_set;
};

constexpr int object_count = 3;

Object objects[object_count] = {
	{ .scale = { 0.6f, 0.6f, 0.6f }, .color = { 1.0f, 1.0f, 1.0f } },
	{ .scale = { 0.6f, 0.6f, 0.6f }, .color = { 1.0f, 0.4f, 0.4f } },
	{ .scale = { 0.6f, 0.6f, 0.6f }, .color = { 0.4f, 0.6f, 1.0f } },
};

int selected_object = 0;

graphics::Buffer vertex_buffer;
graphics::Buffer index_buffer;

VkDescriptorSetLayout vk_descriptor_set_layout;
VkDescriptorPool vk_descriptor_pool;

VkPipelineLayout vk_pipeline_layout;
VkPipeline vk_pipeline;

bool createBuffers() {
	const float radius = std::sqrt(1.0f + phi * phi);

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

	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

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

	vkDestroyShaderModule(context.device, vertex_shader, nullptr);
	vkDestroyShaderModule(context.device, fragment_shader, nullptr);

	if (result != VK_SUCCESS) {
		std::cerr << "Failed to create graphics pipeline\n";
		return false;
	}

	return true;
}

}

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

	vkDestroyDescriptorPool(context.device, vk_descriptor_pool, nullptr);
	vkDestroyDescriptorSetLayout(context.device, vk_descriptor_set_layout, nullptr);

	for (const Object& object : objects) {
		graphics::destroyBuffer(object.uniform_buffer);
	}
	graphics::destroyBuffer(index_buffer);
	graphics::destroyBuffer(vertex_buffer);
}

void update(double time) {
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

	ImGui::SeparatorText("Object");
	ImGui::SliderInt("Selected", &selected_object, 0, object_count - 1);

	Object& selected = objects[selected_object];
	ImGui::SliderFloat3("Position", selected.position, -3.0f, 3.0f);
	ImGui::SliderFloat3("Rotation", selected.rotation, -180.0f, 180.0f);
	ImGui::SliderFloat3("Scale", selected.scale, 0.1f, 3.0f);
	ImGui::ColorEdit3("Color", selected.color);

	ImGui::End();

	if (animation_playing) {
		animation_time += delta_time * animation_speed;
	}

	const math::Matrix4 view = math::translation(0.0f, 0.0f, camera_distance);

	const VkExtent2D extent = graphics::internal::context.swapchain_extent;
	const float aspect = float(extent.width) / float(extent.height);

	math::Matrix4 projection;
	if (projection_type == 0) {
		projection = math::perspective(math::radians(fov_degrees), aspect, z_near, z_far);
	} else {
		projection = math::orthographic(ortho_size * aspect, ortho_size, z_near, z_far);
	}

	for (int i = 0; i < object_count; ++i) {
		Object& object = objects[i];

		const float t = animation_time + 2.0f * math::pi * float(i) / float(object_count);
		const float offset_x = trajectory_radius * std::cos(t);
		const float offset_y = trajectory_height * std::sin(3.0f * t);
		const float offset_z = trajectory_radius * std::sin(t);

		const math::Matrix4 model =
			math::translation(object.position[0] + offset_x, object.position[1] + offset_y,
			                  object.position[2] + offset_z) *
			math::rotationZ(math::radians(object.rotation[2])) *
			math::rotationY(math::radians(object.rotation[1]) + t) *
			math::rotationX(math::radians(object.rotation[0]) + t * 0.5f) *
			math::scale(object.scale[0], object.scale[1], object.scale[2]);

		object.uniforms.matrix = projection * view * model;

		object.uniforms.color[0] = object.color[0];
		object.uniforms.color[1] = object.color[1];
		object.uniforms.color[2] = object.color[2];
		object.uniforms.color[3] = 1.0f;
	}
}

void render(const graphics::internal::FrameData& fd) {
	auto& context = graphics::internal::context;

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

	for (const Object& object : objects) {
		vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout,
		                        0, 1, &object.descriptor_set, 0, nullptr);

		vkCmdDrawIndexed(fd.command_buffer, index_count, 1, 0, 0, 0);
	}

	vkCmdEndRenderPass(fd.command_buffer);

	vkEndCommandBuffer(fd.command_buffer);
}

}
