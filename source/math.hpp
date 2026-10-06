#pragma once

#include <cmath>
#include <numbers>

namespace math {

// Матрица 4x4. Элементы хранятся по столбцам (column-major), так же как
// хранится mat4 в GLSL, поэтому матрицу можно сразу копировать в uniform-буфер.
struct Matrix4 {
	float elements[16];

	// Доступ к элементу как в математике: строка row, столбец column
	float& at(int row, int column) { return elements[column * 4 + row]; }
	float at(int row, int column) const { return elements[column * 4 + row]; }
};

inline Matrix4 operator*(const Matrix4& a, const Matrix4& b) {
	Matrix4 result = {};

	for (int row = 0; row < 4; ++row) {
		for (int column = 0; column < 4; ++column) {
			for (int k = 0; k < 4; ++k) {
				result.at(row, column) += a.at(row, k) * b.at(k, column);
			}
		}
	}

	return result;
}

inline float radians(float degrees) {
	return degrees * std::numbers::pi_v<float> / 180.0f;
}

inline Matrix4 identity() {
	Matrix4 result = {};
	result.at(0, 0) = 1.0f;
	result.at(1, 1) = 1.0f;
	result.at(2, 2) = 1.0f;
	result.at(3, 3) = 1.0f;
	return result;
}

// Перемещение: благодаря однородным координатам (w = 1) сдвиг
// записывается в последний столбец матрицы
inline Matrix4 translation(float x, float y, float z) {
	Matrix4 result = identity();
	result.at(0, 3) = x;
	result.at(1, 3) = y;
	result.at(2, 3) = z;
	return result;
}

inline Matrix4 scale(float x, float y, float z) {
	Matrix4 result = identity();
	result.at(0, 0) = x;
	result.at(1, 1) = y;
	result.at(2, 2) = z;
	return result;
}

// Повороты вокруг осей X, Y и Z, угол в радианах
inline Matrix4 rotationX(float angle) {
	const float c = std::cos(angle);
	const float s = std::sin(angle);

	Matrix4 result = identity();
	result.at(1, 1) = c;
	result.at(1, 2) = -s;
	result.at(2, 1) = s;
	result.at(2, 2) = c;
	return result;
}

inline Matrix4 rotationY(float angle) {
	const float c = std::cos(angle);
	const float s = std::sin(angle);

	Matrix4 result = identity();
	result.at(0, 0) = c;
	result.at(0, 2) = s;
	result.at(2, 0) = -s;
	result.at(2, 2) = c;
	return result;
}

inline Matrix4 rotationZ(float angle) {
	const float c = std::cos(angle);
	const float s = std::sin(angle);

	Matrix4 result = identity();
	result.at(0, 0) = c;
	result.at(0, 1) = -s;
	result.at(1, 0) = s;
	result.at(1, 1) = c;
	return result;
}

// Перспективная проекция из лекции 1. Как и в Vulkan, ось X направлена вправо,
// ось Y вниз, камера смотрит вдоль +Z, а глубина после деления на w попадает в [0, 1].
// fov — вертикальный угол обзора в радианах, aspect = ширина / высота окна.
inline Matrix4 perspective(float fov, float aspect, float z_near, float z_far) {
	const float t = std::tan(fov / 2.0f);

	Matrix4 result = {};
	result.at(0, 0) = 1.0f / (aspect * t);
	result.at(1, 1) = 1.0f / t;
	result.at(2, 2) = z_far / (z_far - z_near);
	result.at(2, 3) = -z_far * z_near / (z_far - z_near);
	result.at(3, 2) = 1.0f; // w = z: после деления на w дальние объекты становятся меньше
	return result;
}

// Ортографическая проекция из лекции 1: параллелепипед width x height x [z_near, z_far]
// перемещается и растягивается в канонический объём x, y в [-1, 1], z в [0, 1].
// Здесь w остаётся равным 1, поэтому размер объекта не зависит от расстояния до камеры.
inline Matrix4 orthographic(float width, float height, float z_near, float z_far) {
	Matrix4 result = identity();
	result.at(0, 0) = 2.0f / width;
	result.at(1, 1) = 2.0f / height;
	result.at(2, 2) = 1.0f / (z_far - z_near);
	result.at(2, 3) = -z_near / (z_far - z_near);
	return result;
}

} // namespace math
