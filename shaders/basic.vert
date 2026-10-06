#version 450 core

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 out_color;

layout(std140, set = 0, binding = 0) uniform GlobalUniforms {
	mat4 matrix; // projection * view * model
	vec4 color;  // цвет фигуры из интерфейса
} global_uniforms;

void main() {
	gl_Position = global_uniforms.matrix * vec4(in_position, 1);

	// Цвет вершины умножается на цвет, выбранный в интерфейсе
	out_color = in_color * global_uniforms.color.rgb;
}
