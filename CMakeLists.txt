cmake_minimum_required(VERSION 3.20)
include(FetchContent)

project(vulkan-starter-app LANGUAGES C CXX)

add_executable(${PROJECT_NAME} source/main.cpp source/graphics_internal.cpp source/application.cpp source/graphics.cpp)

if(LINUX)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
elseif(APPLE)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
elseif(WIN32)
	if(MSVC)
		target_compile_options(${PROJECT_NAME} PRIVATE /W4 /wd4201)
		target_link_options(${PROJECT_NAME} PRIVATE /subsystem:windows /entry:mainCRTStartup)
	else()
		target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
		target_link_options(${PROJECT_NAME} PRIVATE -mwindows)
	endif()
	target_compile_definitions(${PROJECT_NAME} PRIVATE -D_USE_MATH_DEFINES)
else()
	message(FATAL_ERROR "${CMAKE_SYSTEM_NAME} is not supported")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD_REQUIRED TRUE CXX_STANDARD 20)

FetchContent_Declare(
	glfw
	GIT_REPOSITORY https://github.com/glfw/glfw.git
	GIT_TAG 3.5.1
)

FetchContent_Declare(
	vk-bootstrap
	GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
	# Версия совпадает с установленным Vulkan SDK 1.4.313 (v1.4.321 не собирается со старыми заголовками)
	GIT_TAG v1.4.313
)

FetchContent_Declare(
	VulkanMemoryAllocator
	GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
	GIT_TAG v3.4.0
)

FetchContent_Declare(
	imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG v1.92.9
)

find_package(Vulkan REQUIRED)

set(GLFW_LIBRARY_TYPE STATIC)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_DOCS OFF)

FetchContent_MakeAvailable(
	glfw
	vk-bootstrap
	VulkanMemoryAllocator
	imgui
)

target_link_libraries(${PROJECT_NAME} PRIVATE
	glfw
	Vulkan::Vulkan
	vk-bootstrap::vk-bootstrap
	GPUOpen::VulkanMemoryAllocator
)

target_include_directories(${PROJECT_NAME} PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_sources(${PROJECT_NAME} PRIVATE
	${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
	${imgui_SOURCE_DIR}/imgui.cpp
	${imgui_SOURCE_DIR}/imgui_draw.cpp
	${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp
	${imgui_SOURCE_DIR}/imgui_demo.cpp
)

# Compile shaders
find_program(GLSLC_FOUND glslc)
if(GLSLC_FOUND)
	set(_SHADER_BINARIES)

	macro(compile_shader SHADER_FILE)
		set(SHADER_SOURCE ${CMAKE_SOURCE_DIR}/shaders/${SHADER_FILE})
		set(SHADER_BINARY ${SHADER_FILE}.spv)
		set(SHADER_BINARY_PATH ${CMAKE_SOURCE_DIR}/shaders/${SHADER_BINARY})

		add_custom_command(
			OUTPUT ${SHADER_BINARY_PATH}
			COMMAND glslc ${SHADER_SOURCE} -o ${SHADER_BINARY_PATH}
			DEPENDS ${SHADER_SOURCE}
			COMMENT "Compiling ${SHADER_FILE} shader"
		)

		list(APPEND _SHADER_BINARIES ${SHADER_BINARY_PATH})
	endmacro()

	# To compile shader file, use compile_shader function with a file name
	# of a shader inside shaders directory. See example below:

	# compile_shader(example.vert)
	# compile_shader(example.frag)

	compile_shader(basic.vert)
	compile_shader(basic.frag)

	add_custom_target(shaders DEPENDS ${_SHADER_BINARIES})
	add_dependencies(${PROJECT_NAME} shaders)
endif()