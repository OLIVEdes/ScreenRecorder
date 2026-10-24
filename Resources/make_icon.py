# ScreenRecorder
# Copyright (C) 2026 OLIVEdes
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# アプリアイコン（Resources/Icon_1024.png, Resources/Icon_64.png）を生成する。
#   python Resources/make_icon.py
# 生成後は Projucer で .jucer を保存し直すと Builds/VisualStudio2022/icon.ico が更新される。
#
# 64px 版は 16/32/48px の .ico 用。小さくても潰れないよう線を太く、スタンドを省略している。

from pathlib import Path
from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parent

# アプリの配色（Main.cpp の LookAndFeel）に合わせた薄い水色のパステル調
BG_TOP, BG_BOTTOM = (227, 242, 251), (182, 220, 243)
EDGE = (143, 188, 219)   # 明るいタスクバーでも輪郭が分かるように縁取る
SCREEN = (44, 74, 94)


def vertical_gradient(size, top, bottom):
    w, h = size
    column = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        column.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return column.resize((w, h))


def render(size, s, detailed):
    """s: 1024 基準の座標をこの解像度に変換する係数"""
    def p(*v):
        return tuple(round(x * s) for x in v)

    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))

    # 背景（角丸＋縦グラデーション）
    inset, radius = (64, 200) if detailed else (16, 190)
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(p(inset, inset, 1024 - inset, 1024 - inset), radius=round(radius * s), fill=255)
    img.paste(vertical_gradient(img.size, BG_TOP, BG_BOTTOM), (0, 0), mask)

    d = ImageDraw.Draw(img)
    d.rounded_rectangle(p(inset, inset, 1024 - inset, 1024 - inset), radius=round(radius * s),
                        outline=EDGE, width=round((8 if detailed else 24) * s))

    # モニター
    if detailed:
        d.rounded_rectangle(p(208, 268, 816, 668), radius=round(56 * s), outline=SCREEN, width=round(52 * s))
        d.rectangle(p(472, 668, 552, 748), fill=SCREEN)
        d.rounded_rectangle(p(356, 744, 668, 796), radius=round(24 * s), fill=SCREEN)
    else:
        d.rounded_rectangle(p(160, 240, 864, 720), radius=round(80 * s), outline=SCREEN, width=round(88 * s))
        d.rounded_rectangle(p(352, 776, 672, 856), radius=round(40 * s), fill=SCREEN)
    return img


def save(name, final_size, detailed, supersample=4):
    big = final_size * supersample
    image = render(big, big / 1024, detailed)
    image.resize((final_size, final_size), Image.LANCZOS).save(OUT / name)
    print("wrote", OUT / name)


if __name__ == "__main__":
    save("Icon_1024.png", 1024, detailed=True)
    save("Icon_64.png", 64, detailed=False, supersample=16)
