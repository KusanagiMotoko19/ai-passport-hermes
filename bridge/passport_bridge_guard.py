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
# 只认**启动器**（venv 的 python/pythonw）；桥的真解释器是它的子进程，cmdline 里没有脚本路径
LAUNCHER_HINT = "hermes-agent" + "\\venv\\Scripts\\python"


def bridge_launchers():
    """所有**桥启动器**进程 [(create_time, pid)]，按启动时间排序。

    ⚠ 一条桥会有两个匹配进程：venv 的 pythonw **启动器** + 它 spawn 的**真解释器**
    （后者 cmdline 里没有脚本路径）。所以要认「启动器」，否则会把一条桥数成两条。
    """
    found = []
    for proc in psutil.process_iter(["pid", "cmdline", "create_time"]):
        try:
            cmdline = " ".join(proc.info.get("cmdline") or [])
            created = proc.info.get("create_time") or 0.0
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
        if TARGET in cmdline and "guard" not in cmdline and LAUNCHER_HINT in cmdline:
            found.append((created, proc.info["pid"]))
    found.sort()
    return found


def main() -> int:
    found = bridge_launchers()
    if len(found) > 1:
        # ⚠ **两个桥实例会互相抢同一条 BLE 连接**：症状是设备反复「已断开」、
        #   按 OK 收不到音频（10/03 夜里实测：启动撞车跑出 4 个进程抢一条链路）。
        #   保留最早启动的那个，其余终止。
        killed = 0
        for _, pid in found[1:]:
            try:
                psutil.Process(pid).terminate()
                killed += 1
            except psutil.Error:
                pass
        print(f"发现 {len(found)} 个桥实例，已终止多余 {killed} 个（只留最早的那个）")
        return 0
    if found:
        return 0
    # CREATE_NO_WINDOW：绝不弹控制台（科长铁律：前台被占他的手柄按键全失效）
    subprocess.Popen([PYTHONW, BRIDGE], creationflags=0x08000000, close_fds=True)
    print("桥未在运行，已拉起")
    return 0


if __name__ == "__main__":
    sys.exit(main())
