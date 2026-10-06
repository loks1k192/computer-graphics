#version 450 core

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 out_color;

// Данные текущего объекта: берутся из набора дескрипторов, привязанного перед отрисовкой
layout(std140, set = 0, binding = 0) uniform ObjectUniforms {
	mat4 matrix; // projection * view * model
	vec4 color;  // цвет объекта из интерфейса
} object_uniforms;

void main() {
	gl_Position = object_uniforms.matrix * vec4(in_position, 1);

	// Цвет вершины умножается на цвет, выбранный в интерфейсе
	out_color = in_color * object_uniforms.color.rgb;
}
