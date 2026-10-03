#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""接收设备麦克风上行（16kHz IMA-ADPCM）并存成 WAV，顺带实测上行带宽。

用法：
    python listen_mic.py [秒数] [输出wav]

⚠ BLE 单连接：跑之前先停掉 hermes_passport_bridge.py。
说明：录音期间设备只发裸 ADPCM 字节（没有换行分隔），所以收到的通知直接按字节流拼接。
"""
from __future__ import annotations

import asyncio
import json
import os
import sys
import time
import wave

import audioop

NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
RATE = 16000


def jline(payload: dict) -> bytes:
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")


async def main(seconds: float, out_path: str) -> int:
    from bleak import BleakClient, BleakScanner

    device = await BleakScanner.find_device_by_filter(
        lambda d, adv: (d.name or "").startswith("Hermes-"), timeout=15.0)
    if device is None:
        print("!! 没找到设备（桥在跑？先停桥）")
        return 1
    print(f"连接 {device.name} [{device.address}]")

    chunks: list[bytes] = []
    received = 0

    async with BleakClient(device, pair=True, timeout=30.0) as client:
        char = client.services.get_characteristic(NUS_RX_UUID)
        limit = max(20, int(getattr(char, "max_write_without_response_size", 20) or 20))

        def on_notify(_sender, data: bytearray) -> None:
            nonlocal received
            chunks.append(bytes(data))
            received += len(data)

        await client.start_notify(NUS_TX_UUID, on_notify)
        await asyncio.sleep(0.3)
        await client.write_gatt_char(NUS_RX_UUID, jline({"cmd": "mic_start"}),
                                     response=False)
        print(f"录音 {seconds:g} 秒…")
        await asyncio.sleep(seconds)
        await client.write_gatt_char(NUS_RX_UUID, jline({"cmd": "mic_stop"}),
                                     response=False)
        await asyncio.sleep(0.6)

    raw = b"".join(chunks)
    pcm, _ = audioop.adpcm2lin(raw, 2, None)
    with wave.open(out_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)

    seconds_audio = len(pcm) / 2.0 / RATE
    print(f"收到 {len(raw)} 字节 ADPCM → {len(pcm)} 字节 PCM（{seconds_audio:.1f} 秒音频）")
    print(f"实测上行 {len(raw) / seconds:.0f} 字节/s（目标 8000，语音转写足够即算通过）")
    print(f"已保存 {out_path}")
    return 0


if __name__ == "__main__":
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.expanduser("~"), "Downloads", "ai-passport-mic-test.wav")
    sys.exit(asyncio.run(main(secs, out)))
