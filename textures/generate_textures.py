# Процедурная генерация текстур для ЛР3 (numpy + Pillow): python3 textures/generate_textures.py
# Все текстуры бесшовные: при повторе (REPEAT) соседние копии стыкуются без швов.
import os

import numpy as np
from PIL import Image

SIZE = 512
OUT = os.path.dirname(os.path.abspath(__file__))
rng = np.random.default_rng(11)


def value_noise(cells, seed):
    """Плавный бесшовный шум: случайные значения в узлах сетки cells x cells,
    между узлами — сглаженная интерполяция."""
    g = np.random.default_rng(seed).random((cells, cells))
    t = np.arange(SIZE) / SIZE * cells
    i0 = np.floor(t).astype(int)
    f = t - i0
    f = f * f * (3 - 2 * f)
    i1 = (i0 + 1) % cells
    i0 = i0 % cells
    fy, fx = np.meshgrid(f, f, indexing='ij')
    a = g[np.ix_(i0, i0)]
    b = g[np.ix_(i0, i1)]
    c = g[np.ix_(i1, i0)]
    d = g[np.ix_(i1, i1)]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fractal(seed, octaves=5, base=4):
    """Сумма нескольких октав шума — более «природный» рисунок."""
    result = np.zeros((SIZE, SIZE))
    amplitude = 1.0
    for k in range(octaves):
        result += amplitude * value_noise(base * 2 ** k, seed + k)
        amplitude *= 0.5
    return result / result.max()


def save(name, rgb):
    Image.fromarray(np.clip(rgb * 255, 0, 255).astype(np.uint8), 'RGB').save(os.path.join(OUT, name))
    print('saved', name)


y, x = np.mgrid[0:SIZE, 0:SIZE] / SIZE

# Кирпичи: 8 рядов по 4 кирпича, каждый второй ряд сдвинут на полкирпича
rows, cols = 8, 4
row = np.floor(y * rows)
bx = x * cols + 0.5 * (row % 2)
by = y * rows
mortar = (np.abs(bx - np.round(bx)) < 0.04) | (np.abs(by - np.round(by)) < 0.07)
brick_id = (np.floor(bx) % cols) + row * cols
tone = np.random.default_rng(1).uniform(0.75, 1.1, rows * cols)[brick_id.astype(int)]
noise = fractal(2)
brick = np.stack([0.62, 0.27, 0.18]) * (tone * (0.8 + 0.3 * noise))[..., None]
mortar_color = np.stack([0.75, 0.73, 0.68]) * (0.85 + 0.15 * noise)[..., None]
save('bricks.png', np.where(mortar[..., None], mortar_color, brick))

# Дерево: годичные кольца, искажённые шумом, вдоль доски
rings = np.sin((x + 0.15 * fractal(10, 4, 2)) * 2 * np.pi * 12) * 0.5 + 0.5
grain = 0.85 + 0.15 * fractal(20, 3, 32)
wood = np.stack([0.55, 0.35, 0.18]) * ((0.7 + 0.3 * rings) * grain)[..., None]
save('wood.png', wood)

# Мрамор: светлый фон с тёмными прожилками sin(x + шум)
veins = np.abs(np.sin((x + y + 1.5 * fractal(30)) * np.pi * 4)) ** 0.25
marble = np.stack([0.92, 0.91, 0.88]) * (0.55 + 0.45 * veins)[..., None]
save('marble.png', marble)

# Плитка для пола: шахматный порядок из светлых и тёмных плиток с затиркой
tiles = 4
tx, ty = x * tiles, y * tiles
grout = (np.abs(tx - np.round(tx)) < 0.025) | (np.abs(ty - np.round(ty)) < 0.025)
checker = (np.floor(tx) + np.floor(ty)) % 2
noise = 0.9 + 0.1 * fractal(40)
tile = np.where(checker[..., None], np.stack([0.80, 0.78, 0.72]), np.stack([0.30, 0.32, 0.36])) * noise[..., None]
save('tiles.png', np.where(grout[..., None], np.stack([0.15, 0.15, 0.15]), tile))

# Ржавый металл: серый металл с пятнами ржавчины по порогу шума
rust_mask = np.clip((fractal(50) - 0.6) * 5, 0, 1)
metal = np.stack([0.55, 0.57, 0.6]) * (0.85 + 0.15 * fractal(60, 3, 64))[..., None]
rust = np.stack([0.5, 0.22, 0.08]) * (0.7 + 0.5 * fractal(70))[..., None]
save('rusty_metal.png', metal * (1 - rust_mask[..., None]) + rust * rust_mask[..., None])
