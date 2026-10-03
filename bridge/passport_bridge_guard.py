"""工牌桥看门狗 —— 桥进程不在了就拉起来。

为什么需要：计划任务 `HermesPassportBridge` 只有「登录时」触发器，**桥崩了不会自动重启**，
设备就一直显示「启动 Hermes 配对」。10/03 夜里实测过一次（刷机后设备状态异常、
桥反复重连失败那阵）。

由计划任务 `HermesPassportBridgeGuard` 每 5 分钟跑一次；幂等：桥在跑就什么都不做。
纯 Python，不弹窗（解释器用 venv 的 pythonw）。
"""
import subprocess
import sys

import psutil

BRIDGE = r"D:\ai-passport\bridge\hermes_passport_bridge.py"
PYTHONW = r"C:\Users\Administrator\AppData\Local\hermes\hermes-agent\venv\Scripts\pythonw.exe"
# 拆开写，免得扫描时把自己这份脚本的路径也算进去
TARGET = "hermes_" + "passport_bridge.py"


def bridge_running() -> bool:
    for proc in psutil.process_iter(["cmdline"]):
        try:
            cmdline = " ".join(proc.info.get("cmdline") or [])
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
        if TARGET in cmdline and "guard" not in cmdline:
            return True
    return False


def main() -> int:
    if bridge_running():
        return 0
    # CREATE_NO_WINDOW：绝不弹控制台（科长铁律：前台被占他的手柄按键全失效）
    subprocess.Popen([PYTHONW, BRIDGE], creationflags=0x08000000, close_fds=True)
    print("桥未在运行，已拉起")
    return 0


if __name__ == "__main__":
    sys.exit(main())
