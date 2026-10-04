#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AI Passport <-> 桌面 AI 桥（Hermes / DSH）

设备侧：FoloToy AI Passport，跑 Hermes-Buddy 固件（BLE Nordic UART + 换行 JSON）。
主机侧：本进程常驻，做两件事——

  上行（设备 -> Hermes/DSH）
      {"cmd":"key","k":"up|down|ok","ev":"click|long"}  ->  按映射注入快捷键
      {"cmd":"permission","id":"...","decision":"once|deny"} -> 审批通过/拒绝

  下行（Hermes/DSH -> 设备）
      每 HEARTBEAT_SECONDS 推一次 heartbeat（<=> 设备 30s 心跳超时）：
      {"total":..,"running":..,"waiting":..,"msg":"..","entries":[..],"tokens":..}

注入层（keybd_event / 前台判定 / 抢前台）照搬 D:\\dualsense-bridge\\ds_bridge.py
的成熟实现，两套输入栈互不干扰：手柄桥不动，本桥只加一路 BLE 输入。

启动： python hermes_passport_bridge.py [--scan] [--verbose]
"""

from __future__ import annotations

import argparse
import asyncio
import audioop
import ctypes
import ctypes.wintypes
import io as _io
import hashlib
import json
import os
import queue
import re
import sqlite3
import sys
import tempfile
import threading
import wave
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "config.json")
LOG_PATH = os.path.join(HERE, "bridge.log")
LOG_MAX_BYTES = 2 * 1024 * 1024      # 日志超过 2MB 就轮转成 bridge.log.1（常驻进程，不轮转会一直涨）
# 已播报过的最后一条回复 id（持久化）：桥重启后首轮会「补推」屏上那条，
# 若不想清楚就会连带再念一遍（科长 10/03 报的「上一段已经播报过又播一次」）。
SPOKEN_STATE_PATH = os.path.join(HERE, "spoken_state.json")

# 语音播报复用 say.py 的合成管线（文本→本地TTS→ffmpeg→IMA-ADPCM），避免两处各写一份。
sys.path.insert(0, HERE)
from say import build_adpcm, RATE as AUDIO_RATE  # noqa: E402

AUDIO_BYTE_RATE = AUDIO_RATE // 2   # IMA-ADPCM 线速率：每 2 个采样 1 字节
AUDIO_PREBUFFER = 3072              # 先垫满设备侧缓冲再按 1:1 节流

# ───────────────────────── 默认配置 ─────────────────────────
DEFAULT_CONFIG = {
    # 只连广播名以该前缀开头的设备（固件被改成 Hermes-XXXXXX）
    "device_name_prefix": "Hermes",
    # 目标客户端：前台 exe 命中任一前缀算“目标在前台”，顺序即优先级
    "target_exe_prefixes": ["deepseek harness", "hermes"],
    # 目标窗口标题关键字（抢前台用；找不到就放弃抢）
    "target_window_keys": ["deepseek harness", "hermes"],
    "heartbeat_seconds": 10,
    # 新回复自动语音播报（关掉只推文字）
    "speak_replies": True,
    # 播报文本上限。正文已被 extract_card 压成「要点块」（通常 60~100 字），
    # 这里给足余量让它**整段念完** —— 原先 60 字会把要点块也腰斩，
    # 实测症状＝只念到第 60 个字，后半句凭空消失。
    # ⚠ 别设太大：BLE 单连接，播报期间按键/心跳都排在后面（中文 TTS 约 4~5 字/秒）。
    "speak_max_chars": 200,
    # 工牌提示音：Hermes 干活期间每 N 秒滴一声（不用看屏幕也知道在跑还是跑完了）
    "beep_enabled": True,
    # 10/03 晚：设备菜单「播报」开关的镜像 —— 真值在桥这边（播报是桥在执行），
    # 设备只是把开关拨过来。见 apply_device_setting()。
    "speak_enabled": True,
    "beep_interval_seconds": 3.0,
    "beep_amp": 1200,
    # 说话提示音的频率（开录一声 / 停录两声）——**故意取「思考滴」（880Hz）的两倍**：
    # 科长 10/03 反馈两者音色一样、容易分不清是在思考还是收音中。现在：
    #   思考中 = 880Hz 一声（低沉短促，每 3 秒）
    #   开录   = 1760Hz 一声（尖锐）
    #   停录   = 1760Hz 两声（尖锐「滴滴」）
    "voice_beep_freq_hz": 1760.0,
    "voice_beep_ms": 70,
    # ↑ 提示音波形幅度（满量程 32767）。1200 ≈ 1/27，很轻的「滴滴」；
    #   嫌吵调小（600），听不见调大（2400）。
    "reconnect_seconds": 5,
    # 开录后**一直没有收到任何音频**（用户没说 / 设备没上行）时的自动关麦兜底（秒）。
    # 到点就关麦 + 两声「滴滴」，免得设备麦克风一直亮着、桥一直等（科长 10/03 定）。
    "voice_idle_timeout_seconds": 60.0,
    # 原始 RX 诊断日志（10/03 为查「设备没发 vs 桥没收」加的）：
    # 每 5 秒最多一行 `RAW RX <字节数> voice=… <前48字节>`。
    # ⚠ 默认关闭：录音时它每 5 秒一行，长时间挂着会把 bridge.log 刷爆
    #   （10/03 实测涨到 4.5MB）。要排查「设备没发 vs 桥没收」时临时改 True。
    "raw_rx_log": False,
    "require_target_foreground": False,  # True=只有目标在前台才注入
    "raise_target_before_key": True,     # 注入前把目标窗口置顶
    # ⚠ 占位符：真名不要写进代码（本文件要进 Git 仓库）。
    #   想显示自己的名字，在 bridge\config.json 里覆盖（那个文件被 .gitignore 忽略）。
    "owner_name": "Mond",
    "device_name": "Hermes-Passport",
    "log_verbose": False,
    # 动作表（DSL）：
    #   tap:VK      单击
    #   chord:A+B   组合键
    #   wait:秒
    "actions": {
        "dsh": {
                                    "down.long":   ["tap:RETURN"],                        # 确定审批（科长 10/03 定）
                                    "up.long":     ["chord:CONTROL+N"],                   # 新建会话（科长 10/03 定）
        },
        "hermes": {
                        "down.long":   ["tap:F8"],                            # 确定审批（科长 10/03 定）
                                                "up.long":     ["chord:CONTROL+N"],                   # 新建会话（科长 10/03 定）
        },
    },
}

VK = {
    "RETURN": 0x0D, "ESCAPE": 0x1B, "CONTROL": 0x11, "MENU": 0x12,
    # ⚠ 注入用的键全在这张表里查，缺键 = KeyError，而且异常是在 daemon 线程里抛的，
    # pythonw 没有 stderr → 整个注入静默消失，连一行日志都不留。
    # 实测（10/03）：表里少了 "V"，语音输入转写完（日志正常、文本正确）之后
    # 「已注入 Hermes」永远不出现，文字也进不了窗口 —— 就是这个 KeyError。
    "V": 0x56, "C": 0x43, "X": 0x58, "A": 0x41, "S": 0x53, "F": 0x46, "N": 0x4E,
    "F2": 0x71, "F3": 0x72, "F4": 0x73, "F5": 0x74, "F6": 0x75,
    "F7": 0x76, "F8": 0x77, "F9": 0x78, "F10": 0x79,
}
KEYEVENTF_KEYUP = 0x0002
SW_RESTORE = 9

user32 = ctypes.windll.user32
# ⚠ win64 上句柄是 64 位：ctypes 不声明 argtypes/restype 时一律按 c_int 传/收，
# 句柄被截断到 32 位 —— 实测症状就是「枚举窗口全失败、目标窗口永远找不到」
# 以及「foreground_exe() 永远返回空」（OpenProcess 返回的句柄被截断）。
# 凡收句柄的 API 必须显式声明，这里统一在模块加载时声明一次。
_W = ctypes.wintypes
user32.GetForegroundWindow.restype = _W.HWND
user32.GetWindowTextLengthW.argtypes = [_W.HWND]
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowTextW.argtypes = [_W.HWND, _W.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.restype = ctypes.c_int
user32.IsWindowVisible.argtypes = [_W.HWND]
user32.IsWindowVisible.restype = _W.BOOL
user32.ShowWindow.argtypes = [_W.HWND, ctypes.c_int]
user32.ShowWindow.restype = _W.BOOL
user32.SetForegroundWindow.argtypes = [_W.HWND]
user32.SetForegroundWindow.restype = _W.BOOL
user32.GetWindowThreadProcessId.argtypes = [_W.HWND, ctypes.POINTER(_W.DWORD)]
user32.GetWindowThreadProcessId.restype = _W.DWORD
user32.keybd_event.argtypes = [_W.BYTE, _W.BYTE, _W.DWORD, ctypes.c_void_p]
user32.keybd_event.restype = None
_k32 = ctypes.windll.kernel32
_k32.OpenProcess.argtypes = [_W.DWORD, _W.BOOL, _W.DWORD]
_k32.OpenProcess.restype = ctypes.c_void_p
_k32.QueryFullProcessImageNameW.argtypes = [
    ctypes.c_void_p, _W.DWORD, _W.LPWSTR, ctypes.POINTER(_W.DWORD)]
_k32.QueryFullProcessImageNameW.restype = _W.BOOL
_k32.CloseHandle.argtypes = [ctypes.c_void_p]
_k32.CloseHandle.restype = _W.BOOL
user32.BringWindowToTop.argtypes = [_W.HWND]
user32.BringWindowToTop.restype = _W.BOOL
user32.IsIconic.argtypes = [_W.HWND]
user32.IsIconic.restype = _W.BOOL
_log_lock = threading.Lock()


# ───────────────────── 语音输入（工牌麦克风 -> 转写 -> Hermes） ─────────────────────
VOICE_WAV_TMP = os.path.join(tempfile.gettempdir(), "passport_voice.wav")
VOICE_DRAIN_SECONDS = 0.9        # mic_stop 后等设备把尾巴发完
VOICE_MIN_BYTES = 1200           # 少于此认为误触（约 0.3 秒）
VOICE_RMS_MIN = 400              # 音量门限：低于此判为没说话（底噪实测 rms<300，说话 >1000）
# whisper 在「没人说话 / 只录到底噪」时会稳定吐同一批套话（实测 10/03 反复出现，
# 例如「请不吝点赞 订阅 转发 打赏支持明镜与点点栏目」），直接丢掉，别污染会话。
VOICE_HALLUCINATIONS = ("明镜", "点点栏目", "请不吝点赞", "打赏支持", "订阅转发",
                        "字幕志愿者", "字幕由", "感谢观看", "谢谢观看")


def strip_hallucinated_tail(text: str) -> str:
    """裁掉 whisper 在**静音尾巴**上吐的套话（「中文字幕志愿者 李沛」之类）。

    ⚠ 为什么不是「出现套话就整段丢弃」：套话几乎总是**接在真实内容之后**（录完话、
    麦克风还没关的那几秒静音段）。原来那条判据把整句话扔了 —— 10/03 夜里实测：
    科长讲了 88 秒、转写完全正确，只因结尾带了「中文字幕志愿者 李沛」就整段消失。
    现在只从**尾部**裁，且裁完还有内容就照常注入；裁空了才算「只有套话」。
    """
    cleaned = text.strip()
    for _ in range(4):                      # 尾部可能接连吐几段套话
        cut = None
        for marker in VOICE_HALLUCINATIONS:
            index = cleaned.rfind(marker)
            if index < 0:
                continue
            # 两种情况才算幻听：
            #   ① 出现在**末尾 12 字以内** —— 套话总是缀在最尾巴上；
            #   ② 整句很短（≤20 字）且从开头就是套话 —— 那就是「只有套话」。
            # 不用 40 字那种宽窗口：正常说话里提到「谢谢观看」也会被误裁。
            near_tail = index >= len(cleaned) - 12
            whole_short = index == 0 and len(cleaned) <= 20
            if near_tail or whole_short:
                cut = index if cut is None else min(cut, index)
        if cut is None:
            break
        cleaned = cleaned[:cut].strip(" ，。、,.")
    return cleaned

_whisper_model = None
_whisper_lock = threading.Lock()


def get_whisper():
    """懒加载 faster-whisper（单例）。本机模型：~/.cache/whisper/large-v3-turbo-ct2。"""
    global _whisper_model
    with _whisper_lock:
        if _whisper_model is None:
            from faster_whisper import WhisperModel
            model_dir = os.path.expanduser("~/.cache/whisper/large-v3-turbo-ct2")
            _whisper_model = WhisperModel(model_dir, device="cpu", compute_type="int8")
            log("语音输入：whisper 模型已加载")
        return _whisper_model


def set_clipboard_text(text: str) -> None:
    """用 Win32 API 写剪贴板（CTypes，不依赖第三方库）。"""
    import ctypes
    from ctypes import wintypes
    CF_UNICODETEXT = 13
    GMEM_MOVEABLE = 0x0002
    u32 = ctypes.windll.user32
    k32 = ctypes.windll.kernel32
    u32.OpenClipboard.argtypes = [wintypes.HWND]
    u32.OpenClipboard.restype = wintypes.BOOL
    u32.EmptyClipboard.argtypes = []
    u32.EmptyClipboard.restype = wintypes.BOOL
    # ⚠ win64 上句柄是 64 位：凡是不显式声明 argtypes 的调用会按默认 c_int 传参，
    # 实测就在这里炸过 —— "写剪贴板失败 ... OverflowError: int too long to convert"。
    # 一律用 c_void_p 声明句柄参数/返回值。
    u32.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
    u32.SetClipboardData.restype = ctypes.c_void_p
    u32.CloseClipboard.argtypes = []
    u32.CloseClipboard.restype = wintypes.BOOL
    k32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
    k32.GlobalAlloc.restype = ctypes.c_void_p
    k32.GlobalLock.argtypes = [ctypes.c_void_p]
    k32.GlobalLock.restype = ctypes.c_void_p
    k32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    k32.GlobalUnlock.restype = wintypes.BOOL

    data = text.encode("utf-16-le") + b"\x00\x00"
    if not u32.OpenClipboard(None):
        raise RuntimeError("OpenClipboard 失败")
    try:
        u32.EmptyClipboard()
        h = k32.GlobalAlloc(GMEM_MOVEABLE, len(data))
        if not h:
            raise RuntimeError("GlobalAlloc 失败")
        p = k32.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        k32.GlobalUnlock(h)
        if not u32.SetClipboardData(CF_UNICODETEXT, h):
            raise RuntimeError("SetClipboardData 失败")
    finally:
        u32.CloseClipboard()


def save_config(cfg: dict) -> bool:
    """把运行时改过的配置写回 config.json（设备菜单同步开关时用）。"""
    try:
        with open(CONFIG_PATH, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh, ensure_ascii=False, indent=2)
        return True
    except Exception as exc:  # noqa: BLE001
        log(f"配置写盘失败: {exc}")
        return False


def load_config() -> dict:
    cfg = json.loads(json.dumps(DEFAULT_CONFIG))  # deep copy
    if os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, encoding="utf-8") as fh:
                user_cfg = json.load(fh)
            for k, v in user_cfg.items():
                if k == "actions" and isinstance(v, dict):
                    cfg["actions"].update(v)
                else:
                    cfg[k] = v
        except Exception as exc:  # noqa: BLE001
            log(f"config.json 读取失败，用默认配置: {exc}")
    return cfg


def log(text: str, *args) -> None:
    msg = (text % args) if args else text
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}"
    with _log_lock:
        print(line, flush=True)
        try:
            # 超过上限就先轮转一份：10/03 出过 bridge.log 涨到 4.5MB 的事（RX 乱码刷屏），
            # 桥是常驻进程，没有轮转就会一直涨。
            if os.path.exists(LOG_PATH) and os.path.getsize(LOG_PATH) > LOG_MAX_BYTES:
                os.replace(LOG_PATH, LOG_PATH + ".1")
            with open(LOG_PATH, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")
        except OSError:
            pass


# ───────────────────────── 注入层（照搬 ds_bridge） ─────────────────────────
def key_down(vk: int) -> None:
    user32.keybd_event(vk, 0, 0, 0)


def key_up(vk: int) -> None:
    user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)


def tap(vk: int, hold: float = 0.03) -> None:
    key_down(vk)
    time.sleep(hold)
    key_up(vk)


def foreground_exe() -> str:
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return ""
    pid = ctypes.wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)
    if not handle:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(512)
        size = ctypes.wintypes.DWORD(512)
        if ctypes.windll.kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return buf.value.rsplit("\\", 1)[-1].lower()
        return ""
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def foreground_title() -> str:
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return ""
    length = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buf, length + 1)
    return buf.value


def target_profile(cfg: dict) -> str:
    """当前前台是哪个目标。返回 dsh / hermes / ''（都不在）。dsh 优先。"""
    fg = foreground_exe()
    for prefix in cfg["target_exe_prefixes"]:
        if fg.startswith(prefix):
            return "dsh" if "harness" in prefix else "hermes"
    return ""


def find_target_window(cfg: dict):
    """按标题关键字找目标窗口句柄（找不到返回 0）。"""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)
    def _enum(hwnd, _lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length <= 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        title = buf.value.lower()
        for key in cfg["target_window_keys"]:
            if key in title:
                found.append(hwnd)
                return False
        return True

    user32.EnumWindows(_enum, 0)
    return found[0] if found else 0


def raise_target(cfg: dict) -> bool:
    """抢前台。照搬 ds_bridge 的成熟做法：ALT 键技巧解除前台锁 + BringWindowToTop + 重试。

    ⚠ 实测（10/03）：只做 ShowWindow(SW_RESTORE) + SetForegroundWindow，
    在前台是别的程序（尤其全屏游戏）时抢不到 —— 日志表现为
    「目标窗口没找到/抢前台失败」，转写出来的文字就注进了当前前台那个程序里。
    """
    hwnd = find_target_window(cfg)
    if not hwnd:
        return False
    if user32.GetForegroundWindow() == hwnd:
        return True
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, SW_RESTORE)
    for _ in range(3):
        user32.keybd_event(VK["MENU"], 0, 0, 0)      # ALT down：解除前台锁定
        time.sleep(0.01)
        user32.SetForegroundWindow(hwnd)
        user32.BringWindowToTop(hwnd)
        user32.keybd_event(VK["MENU"], 0, KEYEVENTF_KEYUP, 0)
        if user32.GetForegroundWindow() == hwnd:
            return True
        time.sleep(0.08)
    return False


def run_action(cfg: dict, script: list) -> None:
    """执行动作 DSL。注入前按需抢前台，保证按键落在目标上。"""
    if cfg.get("raise_target_before_key"):
        if not raise_target(cfg):
            log("目标窗口没找到/抢前台失败，仍尝试注入到当前前台")
    for step in script:
        if step.startswith("wait:"):
            time.sleep(float(step.split(":", 1)[1]))
        elif step.startswith("chord:"):
            keys = [VK[k.strip().upper()] for k in step.split(":", 1)[1].split("+")]
            for vk in keys:
                key_down(vk)
            time.sleep(0.03)
            for vk in reversed(keys):
                key_up(vk)
        else:
            name = step.split(":", 1)[1].upper()
            tap(VK[name])


# LOCAL PATCH (speak-brief): 每次设备按键写一个"硬件活动"时间戳。
# Hermes 的播报层（hermes_cli/web_routers/audio.py 的 speak-brief 补丁）读它，判断
# "用户现在是不是在用硬件跟我说话" —— 是则只播报要点，不念全文。
def mark_hardware_activity(src: str = "passport") -> None:
    """原子写 %LOCALAPPDATA%/hermes/state/hardware_activity.json（读侧可能正在读）。"""
    try:
        state_dir = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"),
                                 "hermes", "state")
        os.makedirs(state_dir, exist_ok=True)
        path = os.path.join(state_dir, "hardware_activity.json")
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump({"ts": time.time(), "src": src}, fh)
        os.replace(tmp, path)
    except Exception as exc:  # 标记失败绝不能影响按键转发
        log("hardware_activity 写入失败: %s", exc)


def dispatch_key(cfg: dict, key: str, ev: str) -> None:
    """设备按键 -> 当前目标客户端的动作。"""
    mark_hardware_activity("passport")
    profile = target_profile(cfg)
    if not profile:
        if cfg.get("require_target_foreground"):
            log(f"按键 {key}/{ev} 忽略：目标不在前台")
            return
        profile = "dsh"  # 目标不在前台时按 dsh 表处理（并会尝试抢前台）
    action = cfg["actions"].get(profile, {}).get(f"{key}.{ev}")
    if not action:
        log(f"按键 {key}/{ev} 在 {profile} 表里没有映射")
        return
    log(f"按键 {key}/{ev} -> {profile} {action}")
    run_action(cfg, action)


# ───────────────────────── 协议 ─────────────────────────
class LineDecoder:
    """BLE 通知流 -> 换行分隔文本行（照搬官方参考实现）。"""

    def __init__(self, max_line_bytes: int = 4096) -> None:
        self._maximum = max_line_bytes
        self._buffer = bytearray()
        self._discarding = False

    def feed(self, data: bytes) -> list[str]:
        lines = []
        for byte in data:
            if byte == 0x0A:
                if self._discarding:
                    self._discarding = False
                elif self._buffer:
                    if self._buffer[-1:] == b"\r":
                        del self._buffer[-1:]
                    lines.append(self._buffer.decode("utf-8", "replace"))
                self._buffer.clear()
            elif self._discarding:
                continue
            elif len(self._buffer) >= self._maximum:
                self._buffer.clear()
                self._discarding = True
            else:
                self._buffer.append(byte)
        return lines


# 非录音期收到的「残留音频」过滤（10/03 晚加）：设备在桥收尾（mic_stop）之后
# 还会零星上行音频帧，此时 voice_active=False，裸字节被 LineDecoder 按 0x0A 切成
# 一行行乱码，handle_device_line 又把每行打进 bridge.log —— 实测刷了 13578 行、
# 把日志撑到 2.6MB，诊断时全是噪音。
# 判据（宁可漏判，也不能误吞按键 JSON）：
#   1. 包内出现任何行协议结构字符（{ } " 换行）→ 一律不丢，交 LineDecoder 处理。
#      这一条同时挡住「中文 JSON 被拆包、后半段不含 {」的情况——中文 UTF-8
#      字节在 0x80 以上，纯按可打印率判会和音频混淆（实测两者都≈0.36）。
#   2. 一个结构字符都没有、且可打印 ASCII 占比 < 0.6 才算残留音频。
# 漏判的音频包会进 LineDecoder，但 handle_device_line 已不再为非 JSON 行打日志，
# 所以日志不会再被刷；LineDecoder 自带 4096 字节上限兜底。
def looks_like_residual_audio(data: bytes) -> bool:
    if not data:
        return False
    if any(c in data for c in (b'{', b'}', b'"', b'\n')):
        return False
    printable = sum(1 for b in data if 0x20 <= b <= 0x7E or b in (0x09, 0x0A, 0x0D))
    return printable / len(data) < 0.6


def jline(payload: dict) -> bytes:
    return (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


# ───────────────────────── 状态源（Hermes 本机） ─────────────────────────
HERMES_HOME = os.environ.get("LOCALAPPDATA", r"C:\Users\Administrator\AppData\Local") + r"\hermes"


def read_active_session() -> dict:
    """Hermes 正在跑的会话（runtime/active_sessions.json）。

    ⚠ 同一个桌面端进程里可能同时挂着多个活动会话（开过新会话、旧会话尚未回收），
    而且 **entries 的顺序不保证最新在前** —— 实测（10/03）：entries[0] 是 14:01 的旧会话，
    正在用的 15:44 会话排在后面。原先取 entries[0]，桥就一直盯着一个已经没人说话的会话，
    工牌从此收不到任何正文（症状＝「早先能推到工牌，后来突然不推了」）。
    这里按 updated_at（其次 started_at）取**最新**的那个。
    """
    try:
        with open(os.path.join(HERMES_HOME, "runtime", "active_sessions.json"), encoding="utf-8") as fh:
            data = json.load(fh)
        entries = data.get("entries") or []
        if not entries:
            return {}
        return max(entries, key=lambda e: e.get("updated_at") or e.get("started_at") or 0)
    except Exception:  # noqa: BLE001
        return {}


def read_hermes_status() -> dict:
    try:
        with urllib.request.urlopen("http://127.0.0.1:8553/api/status", timeout=3) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception:  # noqa: BLE001
        return {}


def build_heartbeat(cfg: dict, extra_msg: str = "") -> bytes:
    active = read_active_session()
    status = read_hermes_status()
    running = 1 if active else 0
    entries: list[str] = []
    session_id = active.get("session_id", "")
    if session_id:
        entries.append(f"会话 {session_id}")
    surface = active.get("surface")
    if surface:
        entries.append(f"通道 {surface}")
    if status.get("active_agents"):
        entries.append(f"agent 在跑 {status['active_agents']}")
    msg = extra_msg or ("Hermes 在线" if status else "等待 Hermes")
    return jline({
        "total": 1,
        "running": running,
        "waiting": 0,
        "msg": msg[:60],
        "entries": entries[:4],
        "tokens": 0,
        "tokens_today": 0,
    })


# ───────────────────────── 下行正文（Hermes 回复 -> 设备屏） ─────────────────────────
STATE_DB = os.path.join(HERMES_HOME, "state.db")
BODY_MAX_BYTES = 512          # 屏幕与播报**共用**的上限：只推 SPK 摘要（约 170 汉字）
TEXT_POLL_SECONDS = 2.0


def _db():
    return sqlite3.connect(f"file:{STATE_DB}?mode=ro", uri=True, timeout=3)


def read_latest_reply(session_id: str, after_id: int):
    """取该会话中 id 最大的 assistant 正文（跳过工具调用那类空正文）。"""
    if not session_id:
        return None
    try:
        con = _db()
        try:
            # ⚠ 只取「最终答复」（finish_reason='stop'）：
            #   工作中我也会发带正文的 assistant 消息（和工具调用一起落库，
            #   finish_reason='tool_calls'），以前只挡了空正文，于是「找到了…」
            #   这类中途说明被一段段推上工牌刷屏。科长要的是「干完给一份摘要」（10/03）。
            row = con.execute(
                "SELECT id, content FROM messages "
                "WHERE session_id = ? AND role = 'assistant' AND id > ? "
                "AND COALESCE(finish_reason, '') = 'stop' "
                "AND content IS NOT NULL AND TRIM(content) <> '' "
                "ORDER BY id DESC LIMIT 1",
                (session_id, after_id),
            ).fetchone()
        finally:
            con.close()
        return row
    except Exception:  # noqa: BLE001
        return None


def read_last_final_reply(session_id: str):
    """该会话里最后一条「最终答复」（不限 id）—— 桥启动时补推用。

    ⚠ 桥重启后 text_cursor 会对齐到最新（避免把历史整段灌给设备），代价是
    历史消息不再推 —— 于是屏上会一直停着上次的内容（实测：停了几十分钟）。
    启动时用这个函数补推一次，保证屏上有当前该有的东西。
    """
    if not session_id:
        return None
    try:
        con = _db()
        try:
            row = con.execute(
                "SELECT id, content FROM messages "
                "WHERE session_id = ? AND role = 'assistant' "
                "AND COALESCE(finish_reason, '') = 'stop' "
                "AND content IS NOT NULL AND TRIM(content) <> '' "
                "ORDER BY id DESC LIMIT 1",
                (session_id,),
            ).fetchone()
        finally:
            con.close()
        return row
    except Exception:  # noqa: BLE001
        return None


HISTORY_SNAPSHOT_COUNT = 10       # 连上后重放多少条摘要给设备（填翻页历史）


def read_recent_final_replies(session_id: str, count: int = HISTORY_SNAPSHOT_COUNT) -> list:
    """该会话最近 count 条「最终答复」的正文，**按旧→新排列**。

    给设备补历史用：设备的历史只在内存里，一重启就清空 —— 桥每次连上后重放一遍，
    「翻之前的对话」就回来了。返回旧→新是因为设备端是「新的压在最前面」。
    """
    if not session_id or count <= 0:
        return []
    try:
        con = _db()
        try:
            rows = con.execute(
                "SELECT content FROM messages "
                "WHERE session_id = ? AND role = 'assistant' "
                "AND COALESCE(finish_reason, '') = 'stop' "
                "AND content IS NOT NULL AND TRIM(content) <> '' "
                "ORDER BY id DESC LIMIT ?",
                (session_id, count),
            ).fetchall()
        finally:
            con.close()
    except Exception:                                   # noqa: BLE001
        return []
    return [str(row[0]) for row in reversed(rows)]


def latest_message_id(session_id: str) -> int:
    if not session_id:
        return 0
    try:
        con = _db()
        try:
            row = con.execute("SELECT COALESCE(MAX(id), 0) FROM messages WHERE session_id = ?",
                              (session_id,)).fetchone()
        finally:
            con.close()
        return int(row[0] or 0)
    except Exception:  # noqa: BLE001
        return 0


def clean_for_device(text: str) -> str:
    """把 markdown 压成适合 16px 小屏的纯文本。"""
    out = text
    # 剥掉 HTML 注释：Hermes 每条回复开头的 <!--SPK ...--> 要点块是给播报层读的，
    # 推到设备屏上只会白占一屏（设备正文上限 512 字节）。
    out = re.sub(r"<!--.*?-->", "", out, flags=re.S)
    # ⚠ 兜底：结尾不是 -->（例如误写成中文 】）的注释，上面那条剥不掉 ——
    #   这里按"以 <!--SPK 开头那一行"整行删掉，不让标记漏到设备屏上（10/04 bug）。
    out = re.sub(r"<!--\s*SPK\b[^\n]*", "", out, flags=re.S)
    out = re.sub(r"```.*?```", "", out, flags=re.S)
    out = re.sub(r"`([^`]*)`", r"\1", out)
    out = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", out)
    out = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", out)
    out = re.sub(r"^\s{0,3}#{1,6}\s*", "", out, flags=re.M)
    out = re.sub(r"^\s*[-*+]\s+", "· ", out, flags=re.M)
    out = re.sub(r"^\s*>\s?", "", out, flags=re.M)
    out = re.sub(r"^\s*\|.*\|\s*$", "", out, flags=re.M)
    out = out.replace("**", "").replace("__", "")
    out = re.sub(r"\n{3,}", "\n\n", out)
    out = re.sub(r"[ \t]+", " ", out)
    return out.strip()


# ⚠ 结尾必须容错：规范是 `-->`，但写成中文 `】`（或干脆漏写）时，原来的
#   `<!--\s*SPK\s*(.*?)-->` 匹配不到 → 整段 `<!--SPK …】` 被当成"没有要点块"时的
#   第一段原文，原样推上设备屏（10/04 连拍实锤：屏上直接显示 `<!--SPK …】`，
#   又难看又白占一屏）。所以这里只抓"以 <!--SPK 开头的那一行"，首尾标记一律
#   交给 _strip_spk 剥。
SPK_RE = re.compile(r"<!--\s*SPK\b[^\n]*", re.S)


def _strip_spk(text: str) -> str:
    """剥掉 SPK 块的首尾标记。

    开头：`<!--SPK` / `<!-- SPK` / 带中英文冒号都认。
    结尾：`-->`、误写的 `】`、或什么都没有 —— 都能剥干净。
    """
    out = re.sub(r"^\s*<!--\s*SPK\b[：:]?\s*", "", text or "")
    out = re.sub(r"\s*(?:-->|】)\s*$", "", out)
    return out.strip()

# 录音期间，设备会把控制 JSON 直接追加在音频包尾巴上（同一个 notify）。
# 这里用来把控制行从裸字节流里挖出来（详见 _on_notify 的说明）。
_CTRL_LINE_RE = re.compile(rb'\{"[a-z_]{1,16}":.{0,512}?\}\r?\n')


def extract_card(text: str, limit: int) -> str:
    """给设备屏 + 语音播报用的短文本。

    优先取回复开头的 <!--SPK ...--> 摘要（Hermes 每条回复都带）—— 一屏装得下、
    播报也能整段念完，于是设备端不需要翻页，上下键正好空出来做别的用途。

    ⚠ 没有 SPK 块时**只取第一段**，而且上限压到 240 字节：早先的兜底是
    「正文压缩 + 512 字节截断」，一旦我漏写要点块，几千字的正文就会被压成
    一屏过程推上去 —— 科长要的是「总结」，不是「过程」（10/03 实测截图确认）。
    """
    m = SPK_RE.search(text or "")
    if m:
        src = _strip_spk(m.group(0))
    else:
        src = re.split(r"\n\s*\n", (text or "").strip(), maxsplit=1)[0]
        # ⚠ 兜底路径也要剥一次。10/04 那个"标记原样上屏"的 bug 就出在这条路上：
        #   当时 SPK_RE 要求规范结尾 -->，我写成 】 就匹配不到，直接走兜底取第一段，
        #   而 clean_for_device 的 `<!--.*?-->` 同样剥不掉没有 --> 的注释 → 原样上屏。
        src = _strip_spk(src)
        limit = min(limit, 240)          # 约 80 汉字，宁可少也不要灌屏
    return truncate_utf8(clean_for_device(src), limit)


def truncate_utf8(text: str, limit: int) -> str:
    """按 UTF-8 字节截断，不切断多字节字符。"""
    raw = text.encode("utf-8")
    if len(raw) <= limit:
        return text
    cut = raw[:limit]
    while cut and (cut[-1] & 0xC0) == 0x80:
        cut = cut[:-1]
    return cut.decode("utf-8", "ignore")


def build_beep(freqs: tuple = (880.0,), ms: int = 90, gap_ms: int = 70,
               amp: int = 1200) -> bytes:
    """生成短提示音（默认「滴」一声，90ms）的 IMA-ADPCM —— 纯本地合成，不走 TTS。

    用途：Hermes 干活期间给工牌一个听得见的「我还在跑」信号 —— 不用看屏幕就能
    区分「正在工作（有滴滴）/ 已经完事（滴滴停 + 随后播报正文）/ 卡住了」。

    ⚠ amp 是波形幅度（满量程 32767）。**默认 1200 ≈ 1/27，只是轻轻提醒**——
    科长明确要求「很轻很轻，能知道在干活就行」，原来 11000 太吵。
    设备端音量是 100%，所以响度基本由这个幅度决定。
    """
    import math
    import struct

    pcm = bytearray()
    ramp = max(1, int(AUDIO_RATE * 0.008))       # 8ms 淡入淡出，免得咔哒爆音
    for idx, freq in enumerate(freqs):
        n = int(AUDIO_RATE * ms / 1000)
        for i in range(n):
            env = min(1.0, i / ramp, (n - i) / ramp)
            pcm += struct.pack(
                "<h", int(amp * env * math.sin(2 * math.pi * freq * i / AUDIO_RATE)))
        if idx < len(freqs) - 1:
            pcm += b"\x00\x00" * int(AUDIO_RATE * gap_ms / 1000)
    adpcm, _ = audioop.lin2adpcm(bytes(pcm), 2, None)
    return adpcm


# ───────────────────────── BLE 传输 ─────────────────────────
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


def scan_devices(prefix: str) -> list:
    from bleak import BleakScanner

    async def _scan():
        found = await BleakScanner.discover(timeout=8.0, return_adv=True)
        out = []
        for dev, adv in found.values():
            name = dev.name or ""
            uuids = [str(u).lower() for u in (adv.service_uuids or [])]
            if name.startswith(prefix) or NUS_SERVICE_UUID in uuids:
                out.append((name, dev.address, adv.rssi, NUS_SERVICE_UUID in uuids))
        return out

    return asyncio.run(_scan())


class Bridge:
    def __init__(self, cfg: dict) -> None:
        self.cfg = cfg
        self.events: queue.Queue = queue.Queue()
        self.client = None
        self.loop: asyncio.AbstractEventLoop | None = None
        self.decoder = LineDecoder()
        # 语音输入状态：上行期间 voice_active=True，裸字节直接进 voice_buf
        self.voice_active = False
        self.voice_draining = False
        self.voice_buf = bytearray()
        self.voice_started_at = 0.0
        self.voice_last_voice_at = 0.0   # 最近一次「听到有人在说话」（RMS 过线）的时刻
        self._probe_at = 0.0             # 音量探测节流时间戳
        self._dec_pos = 0                # 已喂进增量 ADPCM 解码器的位置（必须 4 字节对齐）
        self._dec_state = None           # 增量解码状态（ADPCM 有状态，不能中途切片解）
        self.last_rx = 0.0
        self._raw_log_at = 0.0     # 原始 RX 诊断日志的节流时间戳
        # 非录音期残留音频的丢弃统计（节流打点，避免又刷屏）
        self._residual_pkts = 0
        self._residual_bytes = 0
        self._residual_log_at = 0.0
        self._mic_stop_nudge_at = 0.0   # 主动补发 mic_stop 的节流（防设备 mic 关不掉）
        self.pending_prompt_id: str | None = None
        self.stop_flag = threading.Event()
        # 播报期间置位：提示音不许插进语音播报中间（BLE 单连接，插进去就是噪音）
        self.speaking = threading.Event()
        # 语音输入让路标志：用户一按 OK 就置位，正在下发的播报立刻收尾让出 BLE。
        # ⚠ 事件队列是串行消费的（_session 的 while 循环），播报 await 十几秒会把
        #   mic_start 堵在队尾 —— 表现就是「我正播报时按 OK 毫无反应、录到 0 字节」
        #   （10/03 查实：mic 命令排到播报后面才发，用户早按完了）。
        self.mic_interrupt = threading.Event()
        # 下行正文游标：起手对齐到当前会话已有消息，避免把历史整段推给设备
        self.text_cursor = latest_message_id(read_active_session().get("session_id", ""))
        # 10/03 晚：最后推给设备屏的那条摘要 —— 设备重连后补推用（见 BLE 连接处）。
        self.last_pushed_body = ""
        self._load_spoken_state()   # 播报游标 + 已播内容指纹（跨重启/跨压缩重写）
        self._last_volume = None    # 上次下发给设备的音量（None = 连接后重设一次）
        # 待播文本队列（text_loop 生产，speech_loop 消费）
        self.speech_queue: queue.Queue = queue.Queue()
        self.speech_tmp = tempfile.mkdtemp(prefix="fap_say_")

    # ---------- asyncio 侧 ----------
    async def _session(self) -> None:
        from bleak import BleakClient, BleakScanner

        self.loop = asyncio.get_running_loop()
        prefix = self.cfg["device_name_prefix"]
        while not self.stop_flag.is_set():
            try:
                log(f"扫描 BLE 设备（前缀 {prefix}-）…")
                device = await BleakScanner.find_device_by_filter(
                    lambda d, adv: (d.name or "").startswith(prefix), timeout=15.0)
                if device is None:
                    log("没扫到设备，重试")
                    await asyncio.sleep(self.cfg["reconnect_seconds"])
                    continue
                log(f"连接 {device.name} [{device.address}]（首次需要输入设备上显示的六位配对码）")
                client = BleakClient(device, disconnected_callback=self._on_disconnect,
                                     pair=True, timeout=60.0)
                await client.connect()
                self.client = client
                await client.start_notify(NUS_TX_UUID, self._on_notify)
                log("已连接并订阅通知（加密链路）")
                # 音量归设备管（10/03 晚），桥不再下发 {"cmd":"volume"}。
                self._last_volume = None
                # 把桥这边的开关状态推下去，免得菜单显示的和实际（桥的行为）相反
                await self._send(jline({"cmd": "settings",
                                        "speak": bool(self.cfg.get("speak_enabled", True)),
                                        "beep": bool(self.cfg.get("beep_enabled", True))}))
                await self._send(jline({"time": [int(time.time()), -time.timezone]}))
                await self._send(jline({"cmd": "owner", "name": self.cfg["owner_name"]}))
                await self._send(jline({"cmd": "name", "name": self.cfg["device_name"]}))
                await self._send(build_heartbeat(self.cfg, "Hermes 已连接"))
                # 10/03 晚：设备重连（或刚复位重启）后屏幕是空的 —— 把最后一条正文补推一次。
                #   桥不重启就不会走 text_loop 首轮的 force 补推，所以这里单独补，
                #   免得再出现「最新这条没推送到设备」。
                if self.last_pushed_body:
                    log(f"设备重连：补推最后一条正文（{len(self.last_pushed_body.encode('utf-8'))} 字节）")
                    await self._send(jline({"cmd": "text", "body": self.last_pushed_body}))
                # 再重放最近几条摘要，把翻页历史填回来。
                #   顺序讲究：必须在补推当前正文**之后** —— 设备的 push 是「新的在前」，
                #   重放是旧→新，最后落地的那条才会成为 history[0]（最新）。
                await self._replay_history()
                # ⚠ 判据用 _link_alive（不能只看 client.is_connected，见其注释）：
                #   否则 WinRT 误报 False 时内层退出，events 队列没人消费 → mic_start 发不出去。
                while self._link_alive(client) and not self.stop_flag.is_set():
                    # 兜底①：开录后一直「没人说话」（没有任何音频，或只有底噪）→ 自动关麦 +
                    # 两声「滴滴」（科长 10/03 定）。⚠ 判据是 **RMS 过线的说话时刻**，不是
                    # 「有没有字节」——设备一开麦就持续推流底噪，只看字节永远等不到超时。
                    if (self.voice_active and not self.voice_draining
                            and time.time() - self.voice_last_voice_at
                            > float(self.cfg.get("voice_idle_timeout_seconds", 60.0))):
                        _secs = float(self.cfg.get("voice_idle_timeout_seconds", 60.0))
                        log(f"语音输入：{_secs:.0f} 秒没听到说话，自动关闭收音")
                        self.toggle_voice_input()
                    elif self.voice_active and not self.voice_draining:
                        self._probe_voice_level()
                    # 兜底②：上行期间安静 2.5 秒就认为说完了，自动收尾转写
                    # （万一按键上报丢了，不能让转圈一直亮着、桥一直等）。
                    # ⚠ 必须**已经收到过音频**才算「说完了」：刚按下 OK 时设备上行还没起来，
                    #   只看 last_rx 会把「刚开录、还在等设备」误判成说完（10/03）。
                    if (self.voice_active and self.voice_buf and self.last_rx
                            and time.time() - self.last_rx > 2.5):
                        log("语音输入：上行静默 2.5 秒，自动收尾转写")
                        self.toggle_voice_input()
                    try:
                        item = self.events.get_nowait()
                    except queue.Empty:
                        await asyncio.sleep(0.2)
                        continue
                    if item[0] in ("heartbeat", "text"):
                        await self._send(item[1])
                    elif item[0] == "audio":
                        # 播报期间挡住提示音（BLE 单连接，插进来就是噪音）
                        self.speaking.set()
                        try:
                            # item[2] = 播报对应的消息 id，item[3] = 内容指纹（提示音都没有）；
                            # 只有**真正发完**才在 _send_audio 末尾记为「已播」（让路/断连不算）。
                            await self._send_audio(item[1],
                                                   item[2] if len(item) > 2 else None,
                                                   item[3] if len(item) > 3 else None)
                        finally:
                            self.speaking.clear()
                    elif item[0] == "beep":
                        # 收音开始的提示音必须发得出去，所以临时撤掉让路标志：
                        # 让路标志是给「播报」用的（用户一按 OK 就掐掉播报），
                        # 而这声「滴」正是要放给用户听的，不能连它一起掐掉。
                        self.mic_interrupt.clear()
                        try:
                            await self._send_audio(build_beep(
                                freqs=(self._voice_beep_freq(),),
                                ms=int(self.cfg.get("voice_beep_ms", 70)), gap_ms=60,
                                amp=int(self.cfg.get("beep_amp", 1200))))
                        except Exception as exc:  # noqa: BLE001
                            log(f"提示音（开录）下发失败: {exc}")
                        finally:
                            if self.voice_active:
                                self.mic_interrupt.set()
                    elif item[0] == "beep2":
                        # 停止收音的两声「滴滴」＝「听到了，正在转写/结束」（科长 10/03 定）。
                        # 与「思考滴」（880Hz）不同频：这里用更尖的 voice_beep_freq_hz，
                        # 开录一声、停录两声，闭着眼也能分辨是在思考还是在收音。
                        _vf = self._voice_beep_freq()
                        try:
                            await self._send_audio(build_beep(
                                freqs=(_vf, _vf),
                                ms=int(self.cfg.get("voice_beep_ms", 70)), gap_ms=60,
                                amp=int(self.cfg.get("beep_amp", 1200))))
                        except Exception as exc:  # noqa: BLE001
                            log(f"提示音（停止）下发失败: {exc}")
                    elif item[0] == "mic":
                        cmd_js = "mic_start" if item[1] == "start" else "mic_stop"
                        await self._send(jline({"cmd": cmd_js}))
                    elif item[0] == "quit":
                        break
            except Exception as exc:  # noqa: BLE001
                log(f"BLE 会话异常: {exc}")
            finally:
                # ⚠ 无条件断开：以前按 is_connected 判断再断，而 WinRT 会误报 False ——
                #   旧 client 就不断开、notify 回调继续收（按键还能进），设备又因为
                #   **还连着**而不再广播，外层于是永远「没扫到设备」。断开失败也不致命。
                if self.client is not None:
                    try:
                        await self.client.disconnect()
                    except Exception:                       # noqa: BLE001
                        pass
                    self.client = None
            if not self.stop_flag.is_set():
                await asyncio.sleep(self.cfg["reconnect_seconds"])

    async def _replay_history(self) -> None:
        """连上后把最近几条摘要重放给设备 —— 补回它的翻页历史（设备重启会清空）。

        只发 `history_add`：设备端只入库、不切页、不动当前正文，所以不会闪屏。
        10 条约 2.4KB，按 0.25 秒间隔发完，避免灌满 BLE 写队列。
        """
        try:
            session_id = read_active_session().get("session_id", "")
            replies = read_recent_final_replies(session_id, HISTORY_SNAPSHOT_COUNT)
        except Exception as exc:                        # noqa: BLE001
            log(f"历史补推：读库失败 {exc}")
            return
        sent = 0
        for raw in replies:
            body = extract_card(raw, BODY_MAX_BYTES)
            if not body:
                continue
            try:
                await self._send(jline({"cmd": "history_add", "body": body}))
            except Exception as exc:                    # noqa: BLE001
                log(f"历史补推：发送中断 {exc}")
                break
            sent += 1
            await asyncio.sleep(0.25)
        if sent:
            log(f"历史补推：已重放 {sent} 条摘要（翻页历史已恢复）")

    def _link_alive(self, client=None) -> bool:
        """链路是否可用。

        ⚠ 不能只看 `BleakClient.is_connected`：WinRT 后端会在链路仍活着时误报 False
        （10/03 晚 22:12 之后实测：桥认为「没连上」在外层空转扫描，而设备的按键通知
        仍在到达）。一旦据此判定「未连接」，events 队列就没人消费（mic_start 永远发不
        出去）、设备又因为**还连着**而不再广播（外层永远「没扫到设备」）→ 用户表现为
        「按 OK 收不到音」。所以活性判据 = bleak 说连着 **或** 最近 20 秒收到过 notify。
        """
        target = client if client is not None else self.client
        if target is None:
            return False
        try:
            if target.is_connected:
                return True
        except Exception:                                   # noqa: BLE001
            pass
        return bool(self.last_rx) and (time.time() - self.last_rx) < 20.0

    async def _send(self, payload: bytes) -> None:
        client = self.client
        if client is None or not self._link_alive(client):
            raise RuntimeError("设备未连接")
        char = client.services.get_characteristic(NUS_RX_UUID)
        limit = max(20, int(getattr(char, "max_write_without_response_size", 20) or 20))
        for i in range(0, len(payload), limit):
            await client.write_gatt_char(NUS_RX_UUID, payload[i:i + limit], response=False)

    async def _send_audio(self, adpcm: bytes, speak_id: int | None = None,
                          fingerprint: str | None = None) -> None:
        """下发一段 IMA-ADPCM。节奏与 say.py 一致：声明 → 垫底 → 1:1 节流 → 收尾。
        ⚠ 全程贴速率发会紧贴消耗线，主机一抖动设备就断供（听感 glitch）。"""
        client = self.client
        if client is None or not client.is_connected:
            return
        # ⚠ 10/03 晚起**桥不再设音量**：设备菜单新增「音量」项（NVS 持久化，直接调
        #   bsp_audio_set_volume）。桥再插一手会把用户刚调好的音量顶回 100%。
        #   _last_volume 仅作历史标记保留，恒为 None → 永不发送。
        await self._send(jline({"cmd": "audio", "rate": AUDIO_RATE,
                                "codec": "adpcm", "bytes": len(adpcm)}))
        await asyncio.sleep(0.25)

        char = client.services.get_characteristic(NUS_RX_UUID)
        limit = max(20, int(getattr(char, "max_write_without_response_size", 20) or 20))
        t0 = asyncio.get_event_loop().time()
        sent = 0
        for i in range(0, len(adpcm), limit):
            if self.mic_interrupt.is_set():
                # 用户要说话，播报立刻让路。先补一个收尾声明，免得设备端音频泵
                # 一直等数据白白耗着；剩下的字节直接丢（这是"打断"，不是"放完"）。
                await self._send(jline({"cmd": "audio_end"}))
                log(f"播报让路：已发 {sent}/{len(adpcm)} 字节后收尾")
                return
            chunk = adpcm[i:i + limit]
            await client.write_gatt_char(NUS_RX_UUID, chunk, response=False)
            sent += len(chunk)
            if sent > AUDIO_PREBUFFER:
                target = (sent - AUDIO_PREBUFFER) / float(AUDIO_BYTE_RATE)
                behind = target - (asyncio.get_event_loop().time() - t0)
                if behind > 0:
                    await asyncio.sleep(behind)
        await self._send(jline({"cmd": "audio_end"}))
        log(f"播报下发完成：{sent} 字节")
        # ⚠ 只有**整段发完**才记「已播」：让路（提前 return）与断连都不算。
        #   10/03 实测的坑：桥断连期间播报排进了队列却发不出去，而游标当时已经推走，
        #   重连后就不再补念 —— 用户表现为「这轮回答没推到工牌」。
        if speak_id is not None:
            self.last_spoken_id = max(self.last_spoken_id, int(speak_id))
        if fingerprint:
            # 内容指纹（持久化）：Hermes 压缩上下文会把同一批消息换新 id 重新落盘，
            # 靠指纹才认得出「这段已经念过」，否则同一段会被念第二遍。
            self.recent_hashes = (self.recent_hashes + [fingerprint])[-60:]
        if speak_id is not None or fingerprint:
            self._save_spoken_state()

    def _on_notify(self, _sender, data: bytearray) -> None:
        self.last_rx = time.time()
        # 诊断（10/03）：任何 notify 都留痕，用来区分「设备没发」和「桥收了没识别」。
        # ⚠ 这是排查用的临时日志：节流 5 秒、只 dump 前 48 字节。
        #   查完把 DEFAULT_CONFIG 里的 raw_rx_log 置 False 就彻底安静（重启桥生效）。
        _now = time.time()
        if self.cfg.get("raw_rx_log", True) and _now - self._raw_log_at > 5.0:
            self._raw_log_at = _now
            log("RAW RX %dB voice=%s drain=%s %r" % (
                len(data), self.voice_active, self.voice_draining, bytes(data[:48])))
        _b = bytes(data)
        # 上行期间（含收尾排空窗口）设备发的裸 ADPCM 没有换行分隔，不能丢给
        # LineDecoder（否则字节会在行缓冲里越积越多）。但设备的按键/状态 JSON
        # 走的是同一条通知 —— 一律当音频吞掉的话「再按一下 OK 停止」就永远传不进来。
        # ⚠ 实测（10/03）：桥收到第一次 ok.click 进入 voice 态后，后面连续 4 次按键
        # 全被当成音频字节吞了，用户表现为「能开始收音、再按 OK 停不下来」。
        if self.voice_active or self.voice_draining:
            # 录音期间：设备的控制 JSON 会直接追加在音频包尾巴上（同一个 notify）。
            # ⚠ 只按「包首字节是不是 {」判断会漏 —— 音频首字节恰好是 '{' 时，整包
            # （音频 + JSON）被 LineDecoder 当成一行，json 解析失败，按键就丢了
            # （实测症状：录音中按 OK 没反应 / 停不下来）。这里把控制行从字节流里
            # 挖出来交给解析器，剩下的才当音频。误判＝音频里恰好出现 {"xxx":…}\n，
            # 概率可忽略。
            for _m in _CTRL_LINE_RE.finditer(_b):
                for line in self.decoder.feed(_m.group(0)):
                    self.handle_device_line(line)
            _audio_part = _CTRL_LINE_RE.sub(b"", _b)
            if _audio_part:
                self.voice_buf += _audio_part
            return
        # 非录音期的残留音频（设备收尾后还在上行）直接丢，不进 LineDecoder——
        # 否则会被切成一行行乱码刷爆 bridge.log（见 looks_like_residual_audio 注释）。
        if looks_like_residual_audio(_b):
            self._drop_residual(_b)
            return
        for line in self.decoder.feed(_b):
            self.handle_device_line(line)

    def _drop_residual(self, data: bytes) -> None:
        self._residual_pkts += 1
        self._residual_bytes += len(data)
        now = time.time()
        if now - self._residual_log_at > 10.0:
            self._residual_log_at = now
            log("丢弃非录音期残留音频 %d 包 / %d 字节（设备收尾后仍在上行，已过滤）"
                % (self._residual_pkts, self._residual_bytes))
        # ⚠ 非录音期**持续**推流 = 设备的 mic 没被真正关掉（mic_stop 丢包或没生效）。
        #   实测 10/03 晚：桥发过 mic_stop 之后设备仍以 ~8KB/s 推流，用户表现为
        #   「按 OK 停不下来收音」。攒够约 1.2 秒的音频就主动补一条 mic_stop
        #   （节流 5 秒，避免和治疗无关的偶发残留打架）。
        if (self._residual_pkts >= 40 and now - self._mic_stop_nudge_at > 5.0
                and not self.voice_active and not self.voice_draining):
            self._mic_stop_nudge_at = now
            self._residual_pkts = 0
            self._residual_bytes = 0
            self.events.put(("mic", "stop"))
            log("非录音期仍有音频上行 → 主动补发 mic_stop（设备 mic 没关掉）")

    def _on_disconnect(self, _client) -> None:
        log("设备已断开")

    # ---------- 设备消息 ----------
    def handle_device_line(self, line: str) -> None:
        line = line.strip()
        if not line:
            return
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            # 不是 JSON = 残行（多半是残留音频被行解码切出的碎片）。
            # ⚠ 这里**不能**打日志：10/03 实测这类残行刷了 13578 行把 bridge.log
            #   撑到 4.5MB。只记一行节流摘要（见 _drop_residual）。
            return
        if not isinstance(payload, dict):
            # ⚠ 裸数字/字符串也是合法 JSON（实测收到过一行 `7`）：以前会直接在
            #   payload.get() 上抛 AttributeError，把整个 BLE 会话打断（掉线重连）。
            log(f"RX 非对象 JSON，忽略: {line[:80]!r}")
            return
        log(f"RX {line[:400]}")   # 只记真 JSON；超长的截断，别让异常行撑爆日志
        cmd = payload.get("cmd")
        if cmd == "key":
            key = payload.get("k", "")
            ev = payload.get("ev", "")
            if self.pending_prompt_id and key == "ok" and ev == "click":
                # 审批页按确定：固件会另发 permission/once，这里不重复注入
                log("审批进行中，按键交给 permission 通道")
                return
            if key == "ok" and ev == "click":
                # OK 短按 = 语音输入开关，**但只在主界面（ui=home）**。
                # ⚠ 菜单/设置/重置/审批/配对页里，OK 短按是设备自己在导航，桥不能再插一手 ——
                #   曾经无脑触发，表现就是「在菜单里按 OK 进设置」的同时桥开始录音（按键冲突）。
                ui = (payload.get("ui") or "home").lower()
                # ⚠ 转写页（transcript）也算「可说话」的界面：设备**每收到一次回复正文
                #   就会自动切过去**（buddy_state.c 的 BUDDY_EVENT_TEXT），而那一页除了
                #   看正文没别的用途，用户按 OK 十有八九就是说下一句。
                #   早期只放行 home —— 结果「我一回复、设备跳到转写页、他再按 OK 就毫无反应」。
                if ui not in ("home", "transcript"):
                    log(f"按键 ok/click 在 {ui} 上下文里 -> 交给设备处理（不触发语音输入）")
                    return
                self.toggle_voice_input()
                return
            dispatch_key(self.cfg, key, ev)
        elif cmd == "setting":
            # 设备菜单把开关拨过来（10/03 晚）：播报/提示音是**桥**在执行的动作，
            # 所以设备只是遥控器，真值在桥这边（见 apply_device_setting）。
            self.apply_device_setting(str(payload.get("key") or ""),
                                      bool(payload.get("value")))
        elif cmd == "permission":
            decision = payload.get("decision")
            self.pending_prompt_id = None
            if decision == "once":
                log(f"设备批准 {payload.get('id')} -> 审批通过")
                run_action(self.cfg, self.cfg["actions"].get(target_profile(self.cfg) or "dsh", {})
                           .get("ok.click", ["tap:RETURN"]))
            else:
                log(f"设备拒绝 {payload.get('id')} -> 审批拒绝")
                run_action(self.cfg, ["tap:ESCAPE"])
        elif payload.get("ack"):
            log(f"设备状态回执: {line}")

    # ---------- 语音输入 ----------
    def _voice_test_loop(self, seconds: float) -> None:
        """诊断用：连上后自动开录 N 秒（不必按 OK），走与按键完全相同的上行/转写/注入路径。"""
        while not self.stop_flag.is_set():
            if self.client is not None and self.client.is_connected:
                break
            time.sleep(0.5)
        time.sleep(2.0)
        log(f"【voice-test】自动开始上行 {seconds:g} 秒，请对着工牌说话")
        self.toggle_voice_input()
        time.sleep(seconds)
        self.toggle_voice_input()

    def _probe_voice_level(self) -> None:
        """看一眼最新上行音频的音量，判「有没有人在说话」。

        只在**有新数据**时探测：否则设备停推流后我们会反复测同一段旧音频、
        一直刷新 voice_last_voice_at，静音超时就永远不触发。节流 0.2 秒。

        ⚠ ADPCM 是**有状态的差分编码**，必须接着上次的状态增量解码 —— 直接对
        `voice_buf` 中间切一段丢给 `adpcm2lin` 会整段失真（实测：把 9000 幅度的
        音频从中间切 1600 字节单独解，rms 从 6354 掉到 88，判成「没人说话」）。
        """
        now = time.time()
        if now - self._probe_at < 0.2:
            return
        self._probe_at = now
        n = len(self.voice_buf)
        n -= n % 4                        # ADPCM 每帧 4 字节，尾部残字节留着下轮
        if n <= self._dec_pos:            # 没有新数据
            return
        chunk = bytes(self.voice_buf[self._dec_pos:n])
        self._dec_pos = n
        try:
            import audioop
            pcm, self._dec_state = audioop.adpcm2lin(chunk, 2, self._dec_state)
            rms = audioop.rms(pcm, 2)
        except Exception as exc:          # noqa: BLE001
            log(f"音量探测失败: {exc}")
            return
        if self.cfg.get("voice_probe_log"):
            # 临时诊断（排查「60 秒没说话不触发」时打开）：看实际 rms 与阈值差多少
            log(f"音量探测：rms={rms:.0f} / 阈值 {self._voice_active_rms()}（本段 {len(chunk)}B）")
        if rms >= self._voice_active_rms():
            self.voice_last_voice_at = now

    def _voice_beep_freq(self) -> float:
        """说话提示音的频率（开录一声 / 停录两声用）。

        与「思考滴」的 880Hz 故意拉开一个八度：默认 1760Hz，听起来更尖、更像蜂鸣。
        想更「风鸣」就调到 2000~2400；嫌刺耳降到 1320。
        """
        try:
            return float(self.cfg.get("voice_beep_freq_hz", 1760.0))
        except (TypeError, ValueError):
            return 1760.0

    def _voice_active_rms(self) -> float:
        """判「有人在说话」的音量门限。

        默认与静音守卫同一个口径（底噪实测 <300、说话 >1000）。**环境吵时嫌它太灵**
        （实测 10/03：房间有声音时探测值常在 1000~9000，麦克风一直判「有人在说」、
        静音超时永不触发）→ 在 config.json 里把 `voice_active_rms_min` 提到 800~1500。
        """
        try:
            return float(self.cfg.get("voice_active_rms_min", VOICE_RMS_MIN))
        except (TypeError, ValueError):
            return float(VOICE_RMS_MIN)

    def toggle_voice_input(self) -> None:
        if not self.voice_active:
            self.voice_buf = bytearray()
            self.voice_active = True
            self.voice_draining = False
            self.voice_started_at = time.time()
            self.voice_last_voice_at = time.time()   # 静音超时从「按下 OK」起算
            self._dec_pos = 0
            self._dec_state = None
            self._probe_at = 0.0
            self.mic_interrupt.set()          # 让正在放的播报立刻收尾
            # 开录「滴」一声；结束「滴滴」两声 —— 靠声音就能分辨这一下是开还是关。
            # （科长 10/03 改定：原先结束不响，实际用起来分不清收音到底停没停）。
            self.events.put(("beep", None))
            self.events.put(("mic", "start"))
            log("语音输入：开始上行（再按 OK 结束并转写）")
        else:
            self.voice_active = False
            self.voice_draining = True       # 继续收尾巴
            self.mic_interrupt.clear()       # 说完了，播报恢复正常
            self.events.put(("mic", "stop"))
            self.events.put(("beep2", None))   # 两声「滴滴」＝收音已停、正在转写
            secs = time.time() - self.voice_started_at
            log(f"语音输入：停止（录了 {secs:.1f} 秒，已收 {len(self.voice_buf)} 字节），转写中…")
            threading.Thread(target=self._voice_finish, daemon=True).start()

    def _voice_finish(self) -> None:
        """等尾巴到齐 -> ADPCM 转 PCM -> whisper -> 打进 Hermes。"""
        time.sleep(VOICE_DRAIN_SECONDS)
        self.voice_draining = False
        raw = bytes(self.voice_buf)
        self.voice_buf = bytearray()
        if len(raw) < VOICE_MIN_BYTES:
            log(f"语音输入：音频太短（{len(raw)} 字节），忽略")
            return
        try:
            import audioop
            pcm, _ = audioop.adpcm2lin(raw, 2, None)
        except Exception as exc:                                    # noqa: BLE001
            log(f"语音输入：解码失败 {exc}")
            return
        # 静音守卫：按了 OK 但没说话时，whisper 会凭空吐「请不吝点赞…」这类套话。
        rms = audioop.rms(pcm, 2)
        if rms < VOICE_RMS_MIN:
            log(f"语音输入：音量过低（rms={rms:.0f} < {VOICE_RMS_MIN}），判为没说话，忽略")
            return
        try:
            with wave.open(VOICE_WAV_TMP, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(16000)
                w.writeframes(pcm)
        except Exception as exc:                                    # noqa: BLE001
            log(f"语音输入：写 wav 失败 {exc}")
            return
        try:
            model = get_whisper()
            segments, _info = model.transcribe(
                VOICE_WAV_TMP, language="zh", beam_size=1,
                vad_filter=False, condition_on_previous_text=False,
                no_speech_threshold=0.6)
            text = "".join(seg.text for seg in segments).strip()
        except Exception as exc:                                    # noqa: BLE001
            log(f"语音输入：转写失败 {exc}")
            return
        if not text:
            log("语音输入：没听出内容")
            return
        # 静音守卫：**只裁尾巴上的幻听套话**，不整段丢弃（见 strip_hallucinated_tail 说明）。
        text = strip_hallucinated_tail(text)
        if not text:
            log("语音输入：整段都是幻听套话，丢弃")
            return
        log(f"语音输入：转写 -> {text}")
        if not self.cfg.get("voice_inject_enabled", True):
            # 测试用开关：只转写不注入（免得验证时把环境里的说话打进会话）
            log("语音输入：注入已关闭（voice_inject_enabled=false），只留转写日志")
            return
        self._inject_text(text)

    def _inject_text(self, text: str) -> None:
        """把文本打进 Hermes 当前会话并回车发送。"""
        try:
            set_clipboard_text(text)
        except Exception as exc:                                    # noqa: BLE001
            log(f"语音输入：写剪贴板失败 {exc}")
            return
        if raise_target(self.cfg):
            time.sleep(0.25)
        # Ctrl+V 粘贴，再回车发送；末尾留一点间隔免得回车早于粘贴完成
        try:
            run_action(self.cfg, ["chord:CONTROL+V", "wait:0.35", "tap:RETURN"])
        except Exception as exc:                                    # noqa: BLE001
            log(f"语音输入：注入失败 {exc}")
            return
        log("语音输入：已注入 Hermes")

    # ---------- 工牌提示音 ----------
    def is_hermes_busy(self, idle_after: float = 180.0) -> bool:
        """Hermes 是否还在干活（决定要不要「滴」）。

        判据 = 本会话**最新一条消息的角色**，而不是「最近有没有写过库」：
          user               -> 刚收到指令，马上开工            = 忙
          tool               -> 工具刚返回，链路还在走          = 忙
          assistant 正文为空  -> 工具调用轮次，还在想            = 忙
          assistant 正文非空  -> **最终答复已出 = 干完了**       = 空闲（立即停止滴）

        ⚠ 早期版本只看时间戳（最近 15 秒内有动静就算忙），结果是「回复都写完了
        还在滴十几秒」—— 科长要求「干完就不要再滴了，只在思考过程中滴」。

        idle_after 只作兜底：超过这么久没有任何消息（我崩了 / 会话被丢弃）也判空闲，
        免得「滴」停不下来。
        """
        sid = read_active_session().get("session_id", "")
        if not sid:
            return False
        try:
            con = _db()
            try:
                row = con.execute(
                    "SELECT role, content, timestamp FROM messages "
                    "WHERE session_id = ? ORDER BY id DESC LIMIT 1", (sid,)
                ).fetchone()
            finally:
                con.close()
        except Exception:                                        # noqa: BLE001
            return False
        if not row:
            return False
        role = (row[0] or "").strip()
        content = (row[1] or "").strip()
        stamp = float(row[2] or 0)
        if stamp and (time.time() - stamp) > idle_after:
            return False
        return not (role == "assistant" and content)

    def activity_loop(self) -> None:
        """Hermes 干活时给工牌「滴滴」—— 不看屏幕也知道是在跑、跑完了、还是卡住了。"""
        if not self.cfg.get("beep_enabled", True):
            log("提示音：已关闭（beep_enabled=false）")
            return
        interval = float(self.cfg.get("beep_interval_seconds", 6.0))
        beep = build_beep(amp=int(self.cfg.get("beep_amp", 1200)))
        was_busy = False
        last_at = 0.0
        while not self.stop_flag.is_set():
            busy = self.is_hermes_busy()
            now = time.time()
            if busy and (not was_busy or now - last_at >= interval):
                if not self.speaking.is_set():     # 正在播报就不插了
                    self.events.put(("audio", beep))
                    last_at = now
                    if not was_busy:
                        log("提示音：Hermes 开始工作（滴）")
            elif was_busy and not busy:
                log("提示音：Hermes 空闲下来了")
            was_busy = busy
            self.stop_flag.wait(1.5)

    # ---------- 心跳 ----------
    def heartbeat_loop(self) -> None:
        while not self.stop_flag.is_set():
            try:
                self.events.put(("heartbeat", build_heartbeat(self.cfg)))
            except Exception as exc:  # noqa: BLE001
                log(f"心跳构造失败: {exc}")
            self.stop_flag.wait(self.cfg["heartbeat_seconds"])

    # ---------- 播报去重：跨重启 + 跨「压缩重写」 ----------
    def _load_spoken_state(self) -> None:
        """读播报游标 + 最近播过的内容指纹。

        ⚠ 为什么除了 id 还要内容指纹：Hermes **压缩上下文时会把同一批消息重新落盘**
        （新 id、时间戳不变、内容一字不差，旧 id 标 active=0）。
        只按 id 判重必然把同一段再念一遍 —— 这正是科长 10/03 报的
        「上一段内容又播报了一次」。指纹去重与 id 无关，重写也认得出。
        """
        self.last_spoken_id = 0
        self.recent_hashes: list[str] = []
        self.recent_text_hashes: list[str] = []
        try:
            with open(SPOKEN_STATE_PATH, encoding="utf-8") as fh:
                data = json.load(fh)
            self.last_spoken_id = int(data.get("last_spoken_id") or 0)
            self.recent_hashes = [str(h) for h in (data.get("recent_hashes") or [])][-60:]
        except Exception:  # noqa: BLE001
            pass

    def _save_spoken_state(self) -> None:
        try:
            with open(SPOKEN_STATE_PATH, "w", encoding="utf-8") as fh:
                json.dump({"last_spoken_id": int(self.last_spoken_id),
                           "recent_hashes": self.recent_hashes[-60:],
                           "at": time.strftime("%Y-%m-%d %H:%M:%S")}, fh)
        except OSError as exc:
            log(f"播报状态写入失败: {exc}")

    @staticmethod
    def _fingerprint(text: str) -> str:
        """正文指纹：压掉全部空白后取前 600 字符的 sha1（换行/缩进差异不影响判重）。"""
        norm = "".join(str(text).split())
        return hashlib.sha1(norm[:600].encode("utf-8")).hexdigest()[:16]

    def apply_device_setting(self, key: str, value: bool) -> None:
        """设备菜单同步过来的开关（10/03 晚）。

        「播报」「提示音」都由桥执行（桥在播 TTS / 桥在发滴声），所以菜单只是个遥控器：
        设备把新值送上来，桥改自己的配置并落盘，下次启动仍生效。
        """
        field = {"speak": "speak_enabled", "beep": "beep_enabled"}.get(key)
        if field is None:
            log(f"设备设置：未知键 {key!r}，忽略")
            return
        if self.cfg.get(field) == value:
            return
        self.cfg[field] = value
        save_config(self.cfg)
        log(f"设备设置同步：{field} = {value}（{'开' if value else '关'}）")

    def text_loop(self) -> None:
        """轮询 Hermes 会话库，把新的回复正文推给设备。"""
        first = True
        while not self.stop_flag.is_set():
            session_id = read_active_session().get("session_id", "")
            # 启动首轮：无视游标，补推一次屏上「本来就该有」的最后答复。
            # ⚠ 桥重启后 text_cursor 对齐到最新，历史不会再推，不补这一下
            #   屏上就停着上一次的内容（10/03 实测：停了几十分钟没变）。
            # ⚠ force 必须单独带 —— 补推那条的 id 本来就 < cursor，
            #   用 row[0] > cursor 判断的话补推永远不生效（踩过）。
            if first:
                row = read_last_final_reply(session_id)
                force = True
                first = False
            else:
                row = read_latest_reply(session_id, self.text_cursor)
                force = False
            if row and (force or row[0] > self.text_cursor):
                self.text_cursor = max(self.text_cursor, row[0])
                # ⚠ 屏幕推**摘要**（extract_card = SPK 要点块），不是全文 —— 科长 10/03 定：
                #   「我不需要看那么多，我只需要看你 summarize 的就行了」。技术上也是必须：
                #   1000 字节的正文在 BLE 上要分 ~50 片（无确认写入），丢一片设备就收到残缺
                #   JSON、整条不更新（实测「最新这条没推送到设备」就是这么来的）。
                body = extract_card(row[1] or "", BODY_MAX_BYTES)
                if body:
                    # ⚠ 正文也按指纹去重：Hermes 压缩上下文会把同一批消息换新 id 重新落盘，
                    #   于是「同一条内容」会以新 id 再走一遍 —— 屏幕没必要再刷一次（会闪同样的字），
                    #   语音更不能重念（科长 10/03 报的就是这个）。
                    fp = self._fingerprint(body)
                    dup = fp in self.recent_text_hashes
                    if dup:
                        log(f"★ 同一段内容已推过（压缩重写/重发），跳过 #{row[0]}")
                    else:
                        self.recent_text_hashes = (self.recent_text_hashes + [fp])[-40:]
                        log(f"下发正文 #{row[0]}（{len(body.encode('utf-8'))} 字节）")
                        self.last_pushed_body = body
                        self.events.put(("text", jline({"cmd": "text", "body": body})))
                        # ⚠ BLE 是「无确认写入」(response=False)，偶发丢包会让屏幕停在旧内容上
                        #   —— 10/03 21:59 实测：正文排进队列、连接正常，但设备屏幕没更新，
                        #   用户报「文字内容没有投送到设备」。
                        #   设备收到 text 会无条件切到转录页并刷新，所以补发是幂等的：
                        #   首次若已到，这次只是重绘同一内容；首次若丢了，这次就补上了。
                        #   （设备对 text 不回 ack，桥无法确认，只能重发。）
                        self.stop_flag.wait(2.5)
                        self.events.put(("text", jline({"cmd": "text", "body": body})))
                    if (self.cfg.get("speak_replies", True)
                            and row[0] > self.last_spoken_id):
                        # 跨重启：桥重启后首轮会 force 补推屏上那条（为的是屏幕不空着），
                        #   以前无脑排语音 → 同一条被念第二遍。
                        # 跨压缩重写：只比 id 不够，必须比内容指纹（见 _load_spoken_state 注释）。
                        # 这里**只判断不落盘**：真正发完才在 _send_audio 里记「已播」，
                        #   断连/让路没念成时游标不推走 → 下一轮自动补念。
                        if dup or fp in self.recent_hashes:
                            log(f"★ 同一段内容已念过，跳过播报 #{row[0]}")
                            self.last_spoken_id = max(self.last_spoken_id, row[0])
                            self._save_spoken_state()
                        else:
                            self.speech_queue.put((row[0], body, fp))
            self.stop_flag.wait(TEXT_POLL_SECONDS)

    def speech_loop(self) -> None:
        """把待播文本合成成 IMA-ADPCM。TTS+ffmpeg 要几秒，绝不能放在 BLE 主循环里，
        否则心跳和按键全卡住。"""
        while not self.stop_flag.is_set():
            try:
                msg_id, text, fp = self.speech_queue.get(timeout=0.5)
            except queue.Empty:
                continue
            if not self.cfg.get("speak_enabled", True):
                # 设备菜单把「播报」关了（10/03 晚）：正文照旧推屏幕，只是不出声。
                # ⚠ 不动游标 —— 重新打开后这一段还能补念。
                log(f"播报已关（设备设置），跳过 #{msg_id}")
                continue
            limit = int(self.cfg.get("speak_max_chars", 100))
            spoken = text[:limit]
            if len(text) > limit:
                spoken += "……"
            try:
                adpcm, seconds = build_adpcm(spoken, self.speech_tmp)
                log(f"播报 #{msg_id} 就绪：{seconds:.1f} 秒 / {len(adpcm)} 字节")
                # 带上 msg_id 与内容指纹：只有这段音频**完整发到设备**之后才记「已播」
                self.events.put(("audio", adpcm, msg_id, fp))
            except Exception as exc:  # noqa: BLE001
                log(f"播报 #{msg_id} 合成失败: {exc}")

    def run(self) -> None:
        # ⚠ 顺序要求（2026-10-03 实测，勿改回后台线程）：
        # WinRT BLE 的活动与本机 ctranslate2 加载 faster-whisper 模型并发时，
        # Windows 直接抛 access violation (0xC0000005)，整个桥进程静默崩溃。
        # 症状：bridge.log 停在「扫描 BLE 设备…」或「连接 …」那行，进程消失，
        # 而设备/IPC 完全正常（单跑一次 bleak 连接 0.5 秒就能连上）——
        # 极易误判成「桥连不上设备」。先在主线程同步加载模型，再启动 BLE，即稳定。
        try:
            get_whisper()
        except Exception as exc:  # noqa: BLE001
            log(f"语音输入预热失败（转写将不可用，桥继续跑）: {exc}")
        if self.cfg.get("voice_test_seconds"):
            threading.Thread(target=self._voice_test_loop,
                             args=(float(self.cfg["voice_test_seconds"]),),
                             daemon=True, name="voice-test").start()
        threading.Thread(target=self.activity_loop, daemon=True, name="beep").start()
        threading.Thread(target=self.heartbeat_loop, daemon=True, name="heartbeat").start()
        threading.Thread(target=self.text_loop, daemon=True, name="text").start()
        if self.cfg.get("speak_replies", True):
            threading.Thread(target=self.speech_loop, daemon=True, name="speech").start()
        try:
            asyncio.run(self._session())
        except KeyboardInterrupt:
            pass
        finally:
            self.stop_flag.set()


SINGLETON_LOCK = os.path.join(HERE, "bridge.lock")


def _acquire_singleton() -> bool:
    """单实例守卫：已经有桥在跑就让本进程退出。

    ⚠ 为什么必须有：**两个桥实例会抢同一条 BLE 连接** —— 症状是设备反复
    「已断开」、按 OK 收不到音频（10/03 夜里实测：手动启动与看门狗撞车，
    跑出 4 个进程抢一条链路，界面全乱）。看门狗那边也做了多实例清理，
    这里是最后一道。
    """
    try:
        if os.path.exists(SINGLETON_LOCK):
            with open(SINGLETON_LOCK, encoding="utf-8") as fh:
                other = int((fh.read() or "0").strip() or 0)
            if other and other != os.getpid() and psutil.pid_exists(other):
                return False
    except Exception:                                   # noqa: BLE001
        pass
    try:
        with open(SINGLETON_LOCK, "w", encoding="utf-8") as fh:
            fh.write(str(os.getpid()))
    except OSError:
        pass
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="AI Passport <-> Hermes/DSH 桥")
    parser.add_argument("--scan", action="store_true", help="只扫描 BLE 设备并退出")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--voice-test", type=float, default=0.0,
                        help="诊断：连接后自动开录 N 秒（免按键），随后转写并注入")
    args = parser.parse_args()

    # 单实例守卫（`--scan` 这类只读诊断放行）
    if not args.scan and not _acquire_singleton():
        log("已有一个桥在运行（bridge.lock）—— 两个实例会抢同一条 BLE 连接，本进程退出")
        return 0

    cfg = load_config()
    if args.verbose:
        cfg["log_verbose"] = True
    if args.voice_test:
        cfg["voice_test_seconds"] = args.voice_test

    if args.scan:
        prefix = cfg["device_name_prefix"]
        log(f"扫描中（{prefix}-* 或 NUS 服务）…")
        rows = scan_devices(prefix)
        for name, addr, rssi, has_nus in rows:
            log(f"  {name!r} [{addr}] rssi={rssi} NUS={'是' if has_nus else '否'}")
        if not rows:
            log("没发现目标设备（检查设备是否开机、BLE 是否开启）")
        return 0

    log(f"AI Passport 桥启动；日志 {LOG_PATH}")
    log(f"配置：目标 {cfg['target_exe_prefixes']}，心跳 {cfg['heartbeat_seconds']}s")
    Bridge(cfg).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
