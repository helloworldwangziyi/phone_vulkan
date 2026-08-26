#!/usr/bin/env python3
"""字体子集生成工具：从完整 TTF 裁出 App 需要的字符，减小嵌入体积。

字符集 = 既有子集（输出文件已存在时）的全部 cmap 码点 ∪ samples/app
源码字符串字面量用到的字符。只增不减：新增 UI 文案导致缺字（渲染空白）
时重跑一次本脚本即可。

用法：
    python3 tools/subset_fonts.py <完整字体.ttf> <输出子集.ttf>
例：
    python3 tools/subset_fonts.py \
        /path/to/NotoSansSC-Regular.ttf \
        core/assets/fonts/notosanssc_regular_subset.ttf

依赖 fonttools（pip install fonttools）。裁剪配置：丢弃 hinting 程序，
保留源字体自带的全部表。注意 stb_truetype 渲染只依赖 cmap/glyf/hmtx
等基础表；GSUB/GPOS 等排版表若源字体没有则子集同样没有（本引擎不读）。
"""
import re
import sys
import glob

from fontTools import subset
from fontTools.ttLib import TTFont


def collect_app_codepoints():
    """samples/app 源码字符串字面量里的全部可打印字符。"""
    codepoints = set()
    for path in glob.glob("samples/app/*.cpp") + glob.glob("samples/app/*.h"):
        src = open(path, encoding="utf-8").read()
        for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
            for ch in literal:
                if ord(ch) >= 0x20 and ch not in '\\"':
                    codepoints.add(ord(ch))
    return codepoints


def collect_existing_codepoints(path):
    """既有子集文件的 cmap 码点（文件不存在时为空集）。"""
    try:
        font = TTFont(path, lazy=True)
    except Exception:
        return set()
    codepoints = set(font.getBestCmap().keys())
    font.close()
    return codepoints


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src_path, dst_path = sys.argv[1], sys.argv[2]

    codepoints = (collect_app_codepoints()
                  | collect_existing_codepoints(dst_path))
    # 保底：可打印 ASCII 始终在子集内。
    codepoints |= set(range(0x20, 0x7F))

    src = TTFont(src_path, lazy=True)
    src_cmap = src.getBestCmap()
    missing = sorted(cp for cp in codepoints if cp not in src_cmap)
    src.close()
    if missing:
        chars = "".join(chr(cp) for cp in missing)
        print(f"warning: 源字体缺 {len(missing)} 个码点（无法裁入）: {chars}",
              file=sys.stderr)

    options = subset.Options()
    options.no_hinting = True  # 与原仓库子集一致：不含 fpgm/prep/cvt
    font = subset.load_font(src_path, options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=sorted(codepoints))
    subsetter.subset(font)
    font.save(dst_path)

    out = TTFont(dst_path, lazy=True)
    print(f"{dst_path}: {out['maxp'].numGlyphs} glyphs, "
          f"{len(out.getBestCmap())} codepoints")
    out.close()


if __name__ == "__main__":
    main()
