# -*- coding: utf-8 -*-
"""FAP_SCREENSHOT_V1 主机端（分块版）：请求整屏截图并拼装成 PNG。

设备端不申请帧缓冲，改为接管 LVGL flush 把渲染块逐块转发，所以协议是：

  -> "FAP_SCREENSHOT_V1\n"
  <- "FAP_SCREENSHOT_V1 <w> <h> RGB565LE <总字节>\n"      头行
  <- "B <x> <y> <w> <h> <字节>\n" + 紧密 RGB565LE 像素     每块（可多块）
  <- "FAP_END\n"                                          结束

用法: python fap_screenshot.py [COM口] [输出PNG]
"""
import os
import re
import sys
import time

import serial
from PIL import Image

CMD = b"FAP_SCREENSHOT_V1\n"
HEADER_RE = re.compile(rb"FAP_SCREENSHOT_V1\s+(\d+)\s+(\d+)\s+RGB565LE\s+(\d+)")
BLOCK_RE = re.compile(rb"B\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(\d+)")
BOOT_SETTLE_S = 2.5


def read_line(ser, deadline):
    """读到换行为止，返回该行（不含换行）；超时返回 None。"""
    line = bytearray()
    while time.time() < deadline:
        ch = ser.read(1)
        if not ch:
            continue
        if ch == b"\n":
            return bytes(line)
        line += ch
    return None


def read_exact(ser, n, deadline):
    got = bytearray()
    while len(got) < n and time.time() < deadline:
        chunk = ser.read(min(4096, n - len(got)))
        if chunk:
            got += chunk
    return bytes(got)


def rgb565le_pixels(data):
    """RGB565LE -> RGB888 元组列表（PIL putdata 用）。"""
    out = []
    for i in range(0, len(data) - 1, 2):
        v = data[i] | (data[i + 1] << 8)
        out.append((((v >> 11) & 0x1F) * 255 // 31,
                    ((v >> 5) & 0x3F) * 255 // 63,
                    (v & 0x1F) * 255 // 31))
    return out


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.expanduser("~"), "Downloads",
        "ai-passport-screenshot.png")

    # ⚠ DTR 控 ESP32-C3 的 GPIO9(启动模式)。pyserial 默认会拉低它 —— 一开串口
    #   就把芯片踢进下载模式，固件不运行、截屏毫无响应。必须先显式置 True。
    # ⚠ 必须"先设 dtr/rts 再 open"。直接给端口名构造会立刻打开串口，
    #   那一刻 DTR 还是默认低电平 -> GPIO9 被拉低 -> 芯片进下载模式，
    #   固件不运行、截屏无响应。空构造 + 设属性 + open() 才来得及。
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.3
    ser.dtr = True
    ser.rts = False
    ser.open()
    print("已打开 %s，等芯片启动 ..." % port, flush=True)
    time.sleep(BOOT_SETTLE_S)
    ser.reset_input_buffer()

    ser.write(CMD)
    ser.flush()
    print("已发送 FAP_SCREENSHOT_V1", flush=True)

    deadline = time.time() + 10
    header = None
    while time.time() < deadline:
        line = read_line(ser, deadline)
        if line is None:
            break
        m = HEADER_RE.search(line)
        if m:
            header = m
            break
    if header is None:
        print("!! 没收到头行", flush=True)
        ser.close()
        return 1

    w, h, total = (int(x) for x in header.groups())
    print("头行: %dx%d RGB565LE 声明 %d 字节" % (w, h, total), flush=True)

    img = Image.new("RGB", (w, h), (0, 0, 0))
    nblocks = 0
    covered = 0
    deadline = time.time() + 40
    while time.time() < deadline:
        line = read_line(ser, deadline)
        if line is None:
            break
        if line.startswith(b"FAP_END"):
            break
        m = BLOCK_RE.match(line.strip())
        if m is None:
            continue                      # 握手/日志行，跳过
        bx, by, bw, bh, nbytes = (int(x) for x in m.groups())
        data = read_exact(ser, nbytes, time.time() + 10)
        if len(data) < nbytes:
            print("!! 块数据不完整 (%d/%d)" % (len(data), nbytes), flush=True)
            break
        blk = Image.new("RGB", (bw, bh))
        blk.putdata(rgb565le_pixels(data))
        img.paste(blk, (bx, by))
        nblocks += 1
        covered += nbytes
    ser.close()

    print("收到 %d 块，共 %d / %d 字节" % (nblocks, covered, total), flush=True)
    img.save(out)
    print("已保存: %s" % out, flush=True)
    return 0 if nblocks else 1


if __name__ == "__main__":
    sys.exit(main())
