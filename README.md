# Компьютерная графика: лабораторные работы

Н. Д. Князьков, группа М8О-303БВ-24, преподаватель В. Д. Бахарев.
Vulkan + GLFW + ImGui на основе стартового кода [vulkan-starter-app](https://github.com/vladeemerr/vulkan-starter-app).

| № | Тема | Статус | Отчёт |
|---|---|---|---|
| 1 | Основы 3D-графики (вариант 11 — правильный икосаэдр) | ✅ выполнена и сдана, доп. задания 1–6 | [`reports/303_Князьков_Лаб1.pdf`](reports/303_Князьков_Лаб1.pdf) |
| 2 | Камера и освещение (модель Блинна-Фонга) | ✅ выполнена и сдана, доп. задания 1–4 | [`reports/303_Князьков_Лаб2.pdf`](reports/303_Князьков_Лаб2.pdf) |
| 3 | Текстурирование | ✅ выполнена, доп. задания 1–2 (на оценку 4) | [`reports/303_Князьков_Лаб3.pdf`](reports/303_Князьков_Лаб3.pdf) |

| 4 | Наложение теней (Shadow mapping) | ✅ выполнена, база и тень прожектора (на оценку 5) | [PDF](reports/303_Князьков_Лаб4.pdf), [DOCX](reports/303_Князьков_Лаб4.docx) |

Ветка `my-working-version` содержит итоговый код ЛР4. Каждая работа разбита на коммиты по шагам.
Последнее состояние ЛР1 — коммит `b2750b7`, ЛР2 — коммит `6f7ba6e`, ЛР3 — `c8bbe7c`.

## Сборка и запуск (GNU/Linux)

Из корня репозитория:

```bash
cmake --preset debug
cmake --build build-debug --parallel
./build-debug/vulkan-starter-app
```

Запускать нужно из корня репозитория, иначе программа не найдёт `shaders/*.spv`.
В `CMakeLists.txt` версия vk-bootstrap понижена до `v1.4.313`, чтобы проект собирался
с Vulkan SDK 1.4.313 (последний SDK в пакетах для Ubuntu).

Управление камерой (ЛР2–ЛР4): W/S/A/D — движение, Q/E — вниз/вверх, стрелки — поворот.

---

## Лабораторная работа №4: Наложение теней

Выполнено базовое задание и дополнительное на оценку 5 — тень от одного выбранного прожектора.

- Каждый кадр карты глубины солнца и прожектора (2048×2048, D32_SFLOAT) рисуются через dynamic rendering.
- Конвейер теней содержит только `shadow.vert`, без фрагментного шейдера, VkRenderPass и VkFramebuffer.
- Солнце: ортографическая проекция и LookAt; прожектор: перспективная проекция и LookAt.
- В основном шейдере `sampler2DShadow` читает карты со сравнением LESS; затенение применяется к вкладу соответствующего источника.
- Shadows: `Sun shadow area` — сторона охвата солнца, `Bias constant` / `Bias slope` — depth bias;
  `Spot light shadow` — включение тени, `Shadow spot light` — выбор прожектора.
- Точечные источники работают без теней. Текстуры, материалы и камера сохранены из предыдущих работ.

Запускайте из обычного терминала Ubuntu: окружение snap-версии VS Code может вызывать конфликт GTK/GSettings.
Требуется Vulkan 1.3 и поддержка `dynamicRendering`. Отчёт: [PDF](reports/303_Князьков_Лаб4.pdf), [DOCX](reports/303_Князьков_Лаб4.docx).

---

## Лабораторная работа №3: Текстурирование

Объект — правильный икосаэдр из ЛР1 (вариант 11), выполнены базовое задание и дополнительные задания 1–2.

- Базовое задание: изображения загружаются из файлов библиотекой [stb_image](https://github.com/nothings/stb)
  и копируются в `VkImage` в памяти видеокарты через staging-буфер. У вершин есть позиция, нормаль и
  текстурные координаты (генерируются процедурно), текстура используется как альбедо в модели Блинна-Фонга
  (combined image sampler). Свойства всех моделей лежат в одном динамическом uniform-буфере, сдвиг задаётся
  в `vkCmdBindDescriptorSets`. Вершинные и индексные буферы находятся в VRAM (staging-буфер + `vkCmdCopyBuffer`).
- Доп. 1: пять текстур, для каждой модели текстура выбирается в выпадающем списке; у каждого материала свой
  набор дескрипторов.
- Доп. 2: нетривиальное сэмплирование для одного объекта (две выборки текстуры с разными координатами и
  модуляция цвета, зависят от времени), включается флагом в push-константах (галочка «Animated texture»).

Текстуры лежат в `textures/`, они сгенерированы скриптом `textures/generate_textures.py` (numpy + Pillow).
Код: `source/application.cpp`, `source/graphics.cpp` (буферы в VRAM и загрузка текстур), шейдеры в `shaders/`.
Отчёт: `reports/303_Князьков_Лаб3.pdf`.

---

## Лабораторная работа №2: Камера и освещение

Объект — правильный икосаэдр из ЛР1 (вариант 11), выполнены все дополнительные задания (1–4).

- Базовое задание: у вершин есть нормали, они считаются процедурно (векторное произведение рёбер грани).
  Камера (позиция, yaw/pitch, угол обзора) управляется из ImGui и с клавиатуры: WASD, Q/E — движение,
  стрелки — поворот. Один направленный источник, рассеянный свет и материал объекта (альбедо, цвет блика,
  коэффициент блеска); освещение по модели Блинна-Фонга во фрагментном шейдере. На сцене пол и три икосаэдра.
- Доп. 1: два набора дескрипторов — `set = 0` для сцены (uniform-буфер `SceneUniforms` и storage-буферы
  источников), `set = 1` для модели (матрица модели и материал).
- Доп. 2: массив точечных источников в storage-буфере, затухание `1 / D^2`, добавление и удаление в интерфейсе.
- Доп. 3: массив прожекторов в storage-буфере, затухание `1 / D^2`, мягкие края (внутренний и внешний углы).
- Доп. 4: визуализация источников (кнопка «Show light sources»): кубы цвета источника и конусы прожекторов
  из белых линий, рисуются отдельными конвейерами без освещения, матрица и цвет передаются push-константами.

Код: `source/application.cpp`, `source/math.hpp` (матрицы и векторы), `shaders/basic.vert`, `shaders/basic.frag`, `shaders/light.vert`, `shaders/light.frag`.
Отчёт: `reports/303_Князьков_Лаб2.pdf`.

## Лабораторная работа №1: Основы 3D-графики

**Вариант 11 — правильный икосаэдр**, выполнены все дополнительные задания (1–6).

- Икосаэдр (12 вершин, 20 граней) рисуется из вершинного и индексного буферов,
  матрица `projection * view * model` передаётся в шейдер через uniform-буфер и набор дескрипторов.
- Доп. 1: переключение перспективной и ортографической проекции.
- Доп. 2: изменение позиции, поворота и растяжения фигуры.
- Доп. 3: движение по траектории (окружность + волна по Y) с вращением,
  пауза/воспроизведение, скорость, радиус траектории и высота волны.
- Доп. 4: выбор цвета фигуры (`ImGui::ColorEdit3`).
- Доп. 5: цвет каждой вершины считается из её позиции и умножается на выбранный цвет.
- Доп. 6: на сцене три икосаэдра. У каждого свои параметры, свой uniform-буфер и свой
  набор дескрипторов (`VkDescriptorSet`), который привязывается перед его отрисовкой.

Код лабы: `source/application.cpp`, `source/graphics.cpp` (создание буферов и загрузка шейдеров),
`source/math.hpp` (матрицы), `shaders/basic.vert`, `shaders/basic.frag`. Отчёт лежит в папке `reports/`.

---

# 🌋 Vulkan Starter App

## Getting started

You need C++ compiler, Vulkan SDK and CMake installed before you can build this project.

This project uses C++20 standard and thus requires either of those compilers:
- GCC 10.X
- Clang 10
- Microsoft Visual Studio 2019

This is officially tested on *Windows* and *GNU/Linux platforms*, no *macOS* support yet.
If you have a working macOS solution of this code, consider submitting a PR so others
can build this example code without a hassle!

<ins>**1. Downloading the repository**</ins>

Start by cloning the repository with `git clone --depth 1 https://github.com/vladeemerr/vulkan-starter-app`

This repository does not contain any submodules, it utilizes CMake's `FetchContent` feature instead.

<ins>**2. Configuring the project**</ins>

Run either one of the CMake lines to download dependencies and configure the project:

```bash
cmake --preset debug       # for GNU/Linux (GCC/Clang)
cmake --preset msvc-debug  # for Windows (Visual Studio 2019)
cmake --preset mingw-debug # for Windows (MinGW)
```

If you wish to build in `release` mode, change `debug` to `release`.

If changes are made (added/removed files), or if you want to regenerate project files, rerun the command above.

<ins>**3. Building**</ins>

To build the project, use the line below. You are most likely using `debug` preset, so
the directory that will eventually contain your build files is named `build-debug`.

Likewise for `release` that directory will be named `build-release`

Run one those commands, depending on which preset you chose:

```bash
cmake --build build-debug --parallel # for debug
cmake --build build-release --parallel # for release
```

### Running

`build-*` directory will contain the executable in one of the subdirectories after successful build.

For `msvc-{debug|release}` builds output subdirectory is set to `Debug` or `Release` respectively.
For other configurations output subdirectory is set to `vulkan-starter-app`.

**Make sure your working directory is set to the project root!**
Project root is where this README file resides. Otherwise, the
code responsible for loading shaders or other resources from files will fail,
because relative paths are used.

### Compiling shaders

`CMakeLists.txt` has a build recipe for compiling shader files
along with an application. Look for a comment in this file to see
how to compile your shaders.
