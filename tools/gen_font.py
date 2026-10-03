#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重新生成 LVGL 中文字体：ASCII + GB2312 全集(符号+一级+二级, 7445 字)。

用法： python gen_font.py
产出： firmware/assets/fonts/ui_font_cjk_16.c
说明： ⚠ 只用一级字库(3755)时，二级字如"渲"会整字丢失(不是画成豆腐块，
       是被跳过) —— 实测"中文渲染正常"显示成"中文染正常"。
       全字库 7445 字形 × 32 字节 ≈ 238 KB，落在 flash(rodata)，不占 DRAM。
       app 分区 1500 KB，当前 bin 约 1265 KB，改后约 1360 KB，仍有余量。
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = r"D:\ai-passport\firmware"
CONV = r"D:\ai-passport\node_modules\lv_font_conv\lv_font_conv.js"
TTF = os.path.join(FW, "assets", "fonts", "wqy", "wqy-microhei-Regular.ttf")
OUT = os.path.join(FW, "assets", "fonts", "ui_font_cjk_16.c")
CHARSET = os.path.join(HERE, "charset_gb2312_full.txt")

chars = open(CHARSET, encoding="utf-8").read().strip()
print(f"字符集：{len(chars)} 个字符")

if os.path.exists(OUT):
    bak = OUT + ".prev"
    shutil.copy2(OUT, bak)
    print(f"已备份旧字体 -> {bak}")

cmd = [
    "node", CONV,
    "--font", TTF,
    "--range", "0x20-0x7E",
    "--symbols", chars,
    "--size", "16",
    "--bpp", "1",
    "--format", "lvgl",
    "--no-compress",
    "--lv-font-name", "ui_font_cjk_16",
    "--lv-include", "lvgl.h",
    "-o", OUT,
]
print("执行 lv_font_conv …")
proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
if proc.returncode != 0:
    print("失败：", proc.returncode)
    print(proc.stdout[-3000:])
    print(proc.stderr[-3000:])
    sys.exit(1)
print("OK")
if proc.stdout.strip():
    print(proc.stdout[-800:])
print("字体文件大小:", os.path.getsize(OUT), "字节")
