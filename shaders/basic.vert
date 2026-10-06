#version 450 core

layout(location = 0) in vec3 in_position;

layout(std140, set = 0, binding = 0) uniform GlobalUniforms {
	mat4 matrix; // projection * view * model
} global_uniforms;

void main() {
	gl_Position = global_uniforms.matrix * vec4(in_position, 1);
}
