#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一段文字合成语音并下发到设备播放。

管线（全程本地，不出网）：
    文本 → 本地 TTS(127.0.0.1:8799, OpenAI 兼容) → mp3
         → ffmpeg 转 16kHz 单声道 s16le
         → IMA-ADPCM 4bit 压缩（16k × 0.5B = 8 KB/s，与 8kHz u-law 同带宽，
           但采样率翻倍，人声明显清楚；实测 BLE 吞吐 9.21 KB/s）
         → BLE 分片下发 → 设备端解码 → ES8311 播放

用法：
    python say.py "要播报的文字"
    python say.py --dry "文字"        # 只生成 wav/ulaw 文件，不连设备

注意：BLE 单连接，跑之前请先停掉 hermes_passport_bridge.py。
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import subprocess
import sys
import tempfile
import urllib.request
import wave

import audioop

TTS_URL = "http://127.0.0.1:8799/v1/audio/speech"
TTS_MODEL = "edge-xiaoxiao"
TTS_VOICE = "Serena"
TTS_SPEED = 1.25              # 播报语速倍数（1.0 = 原速）。科长 10/03 定：1.25，原来 1.0 太慢

RATE = 16000                   # 采样率；ADPCM 4bit -> 8000 字节/s，卡在 BLE 带宽内
BYTE_RATE = RATE // 2          # IMA-ADPCM 的线速率：每 2 个采样 1 字节
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
FFMPEG = os.path.expandvars(
    r"%LOCALAPPDATA%\hermes\tools\ffmpeg-9.0.1-win32-x64\bin\ffmpeg.exe")


def jline(payload: dict) -> bytes:
    return (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def synthesize(text: str, mp3_path: str) -> int:
    payload = json.dumps({"model": TTS_MODEL, "input": text,
                          "voice": TTS_VOICE, "speed": TTS_SPEED}).encode("utf-8")
    req = urllib.request.Request(TTS_URL, data=payload,
                                headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = resp.read()
    with open(mp3_path, "wb") as fh:
        fh.write(data)
    return len(data)


def to_pcm(mp3_path: str, wav_path: str) -> bytes:
    subprocess.run([FFMPEG, "-y", "-i", mp3_path, "-ar", str(RATE), "-ac", "1",
                    "-acodec", "pcm_s16le", wav_path],
                   capture_output=True, check=True)
    with wave.open(wav_path, "rb") as w:
        return w.readframes(w.getnframes())


def build_adpcm(text: str, workdir: str) -> tuple[bytes, float]:
    mp3 = os.path.join(workdir, "say.mp3")
    wav = os.path.join(workdir, "say.wav")
    n = synthesize(text, mp3)
    pcm = to_pcm(mp3, wav)
    # ⚠ 设备端解码器照 Python audioop 的格式写（nibble 高 4 位在前），
    #   已用 2000 采样正弦对拍验证逐采样误差为 0。
    adpcm, _ = audioop.lin2adpcm(pcm, 2, None)
    with open(os.path.join(workdir, "say.adpcm"), "wb") as fh:
        fh.write(adpcm)
    seconds = len(pcm) / 2.0 / float(RATE)
    print(f"合成 {n} 字节 mp3 → {len(pcm)} 字节 PCM → {len(adpcm)} 字节 IMA-ADPCM"
          f"（{seconds:.1f} 秒音频）")
    return adpcm, seconds


async def send_chunks(client, data: bytes, limit: int) -> None:
    """按 ATT MTU 分片写：单片超限会被 Windows 蓝牙栈拒为 E_INVALIDARG
    （WinError -2147024809），连接与服务发现都正常但第一次写就报参数错误。"""
    for i in range(0, len(data), limit):
        await client.write_gatt_char(NUS_RX_UUID, data[i:i + limit], response=False)


async def send_to_device(adpcm: bytes, seconds: float) -> int:
    from bleak import BleakClient, BleakScanner

    device = await BleakScanner.find_device_by_filter(
        lambda d, adv: (d.name or "").startswith("Hermes-"), timeout=15.0)
    if device is None:
        print("!! 没找到设备（是否被桥占着？先停桥）")
        return 1
    print(f"连接 {device.name} [{device.address}]")
    async with BleakClient(device, timeout=30.0) as client:
        # 主动请求大 MTU。Windows 默认只给 23（20 字节/片），22 秒音频要发 9073 片，
        # 单核 C3 的 nimble_host 处理不过来 → IDLE 任务饿死 → 看门狗把设备打重启
        # （症状：播到一半就断，日志 task_wdt: CPU 0: nimble_host + 寄存器转储）。
        # GattSession.MaxPduSize 是可写的，设上去系统就会发 ATT MTU 交换请求。
        try:
            sess = getattr(getattr(client, "_backend", None), "_session", None)
            if sess is not None:
                sess.max_pdu_size = 517
                await asyncio.sleep(0.3)
            print(f"MTU 协商结果: {getattr(client, 'mtu_size', '?')} 字节")
        except Exception as mtu_err:
            print(f"MTU 请求失败（不影响功能）: {mtu_err}")
        char = client.services.get_characteristic(NUS_RX_UUID)
        limit = max(20, int(getattr(char, "max_write_without_response_size", 20) or 20))
        print(f"MTU 分片 {limit} 字节/片，共需 {len(adpcm)//limit + 1} 片")

        # 0) 先把音量设到最大 —— 设备端记住目标值，打开 codec 时生效。
        await send_chunks(client, jline({"cmd": "volume", "pct": 100}), limit)

        # 1) 声明音频参数，让设备切到音频接收模式
        await send_chunks(client, jline({"cmd": "audio", "rate": RATE,
                                         "codec": "adpcm", "bytes": len(adpcm)}), limit)
        await asyncio.sleep(0.25)

        # 2) 裸字节流：先垫满设备侧缓冲，之后才按实时速率节流
        #    ⚠ 全程"精确贴播放速率"发会紧贴消耗线 —— 主机(Windows BLE 调度)
        #    稍有抖动设备就断供，听起来就是 glitch。先灌 PREBUFFER 字节垫底
        #    (设备环形缓冲 8KB / DMA 180ms)，剩余部分按 1:1 速率发。
        PREBUFFER = 3072
        t0 = asyncio.get_event_loop().time()
        sent = 0
        for i in range(0, len(adpcm), limit):
            chunk = adpcm[i:i + limit]
            await client.write_gatt_char(NUS_RX_UUID, chunk, response=False)
            sent += len(chunk)
            if sent > PREBUFFER:
                target = (sent - PREBUFFER) / float(BYTE_RATE)
                behind = target - (asyncio.get_event_loop().time() - t0)
                if behind > 0:
                    await asyncio.sleep(behind)
        print(f"已发送 {sent} 字节（预垫 {PREBUFFER} 字节）")

        # 3) 收尾
        await send_chunks(client, jline({"cmd": "audio_end"}), limit)
        await asyncio.sleep(max(0.3, seconds * 0.15))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="文字 → 设备语音播报")
    ap.add_argument("text", help="要播报的文字")
    ap.add_argument("--dry", action="store_true", help="只合成，不下发")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        adpcm, seconds = build_adpcm(args.text, tmp)
        if args.dry:
            print("dry-run 完成（未连设备）")
            return 0
        return asyncio.run(send_to_device(adpcm, seconds))


if __name__ == "__main__":
    sys.exit(main())
