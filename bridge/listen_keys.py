# -*- coding: utf-8 -*-
"""常驻监听：只打印设备上报的按键事件，不做任何键盘注入（零副作用）。
用法: python listen_keys.py [监听秒数]"""
import asyncio, sys, time
from bleak import BleakScanner, BleakClient

NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

def on_notify(_, data):
    msg = data.decode("utf-8", "replace").strip()
    print("[%s] << %s" % (time.strftime("%H:%M:%S"), msg), flush=True)

async def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 180
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: d.name and "Hermes" in d.name, timeout=20.0)
    print("[%s] 设备: %s" % (time.strftime("%H:%M:%S"), dev.name if dev else "没找到"), flush=True)
    if dev is None:
        return
    async with BleakClient(dev, timeout=40.0) as c:
        print("[%s] 已连接: %s" % (time.strftime("%H:%M:%S"), c.is_connected), flush=True)
        await c.start_notify(NUS_TX, on_notify)
        print("[%s] [OK] 已订阅上报通道，监听 %d 秒 —— 请按设备的 上/下/确定" % (time.strftime("%H:%M:%S"), secs), flush=True)
        hb = b'{"total":3,"running":1,"waiting":0,"msg":"listen","entries":[],"tokens":0}\n'
        t0 = time.time()
        last_hb = t0
        while time.time() - t0 < secs:
            if time.time() - last_hb > 20:
                try:
                    await c.write_gatt_char(NUS_RX, hb, response=False)
                except Exception as e:
                    print("[%s] 心跳失败: %s" % (time.strftime("%H:%M:%S"), e), flush=True)
                last_hb = time.time()
            await asyncio.sleep(2)
        await c.stop_notify(NUS_TX)
    print("[%s] 监听结束" % time.strftime("%H:%M:%S"), flush=True)

asyncio.run(main())
