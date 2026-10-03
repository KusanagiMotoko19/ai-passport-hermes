# AI 工牌 · Hermes 伙伴

把 **Hermes（桌面端 AI Agent）** 装进一块挂在胸前的 **ESP32-C3 电子工牌**：
屏幕显示 Agent 的回复摘要、按键直接对 Agent 下达指令、麦克风录音转写回传、语音播报要点。

面向的使用场景很具体：**人不在电脑前**（挂着工牌走、拿着手柄玩）也能知道
「Agent 在干什么、干完了没有、有没有事需要我拍板」。

---

## 硬件

| 项 | 值 |
|---|---|
| 设备 | FoloToy **AI Passport**（ESP32-C3 @160MHz，无 PSRAM，4MB Flash） |
| 屏幕 | ST7789 240×320，4-bit 调色板 16 色 |
| 音频 | ES8311 codec（模拟麦 + 喇叭），IMA-ADPCM 16kHz |
| 链路 | BLE（Nordic UART Service），COM9 串口用于刷机/截图/抓日志 |

## 来源与许可

本项目**基于**以下两个开源工作，改动部分以 **Apache-2.0** 一并发布：

- **[FoloToy AI Passport](https://github.com/FoloToy)** 固件框架（ESP-IDF / 设备抽象 / BLE 协议 / 字符集）
- **Claude Desktop Buddy** by **Felix Rieseberg** —— 桌面端 ↔ 硬件伙伴的原始构想与桥的设计
- **[Hermes Agent](https://github.com/NousResearch)** by **Nous Research** —— 本项目的 Agent 侧

设备「关于」页也保留了这两处致谢。

## 这一版新增的（相对上游）

**固件**

- **首页整屏黑白头像**：Hermes 的 Nous Girl，180×180 **四级灰度**（4× 超采样 + 调色板索引），
  素材取自官方 SVG（viewBox 5487×5559），不是放大的小位图
- **配色改成 Hermes 的冷峻黑白灰**：菜单选中条 `#C3C7CC`，状态栏 BLE 标记米白，
  全工程无彩色（红/绿/黄/蓝仅用于语义提示）
- **设置菜单 9 项**：亮度 / 背光 / 音量 / 播报 / 提示音 / 蓝牙 / 钟向 / 重置 / 返回 ——
  六项设置**存 NVS**，重启保留，开机自动应用亮度与音量
- **正文排版修复三连**（详见 `docs/`）：
  1. 中文换行**按字符断行**（原实现走「截断 + 打 `...`」分支，中文每行都被砍一刀）
  2. 量宽度按**完整字符**（原实现量到半个多字节字符，每行少放一个字、那个字被挤成单独一行）
  3. **避头尾**：行首不出现标点（做法是「本行末字留给下一行」，不是「超宽硬塞」——
     后者会被渲染层裁掉、连着前面的字一起消失）
- **正文可滚动 + 翻对话历史**：`▲`/`▼` 短按先滚动，滚到头再翻上一条 / 回首页；
  设备缓存最近 10 条（环形）
- **删除「帮助」「演示」入口**；「关于」页写清协作与致谢，并烙入编译日期（`__DATE__ __TIME__`）
- **三种提示音可区分**：思考 `880Hz` 一声 / 开录 `1760Hz` 一声 / 停录 `1760Hz` 两声

**桥**（`bridge/hermes_passport_bridge.py`）

- 屏幕与语音**分工**：屏幕推 `<!--SPK-->` 要点摘要（≈170 汉字），语音念同一段
  —— 不推全文是有原因的，见「已知限制」
- **设备重连后补推**最后一条正文（设备一复位屏幕就空，桥不重启就不会补）
- **幻听守卫只裁尾巴**：whisper 在静音段会吐「中文字幕志愿者 李沛」这类套话，
  原实现「文本里出现套话就整段丢弃」会把真实长句一起枪毙，现在只从尾部裁
- **日志轮转**（2MB）+ `raw_rx_log` 默认关闭（录音时它每 5 秒一行，会把日志刷爆）
- **看门狗** `passport_bridge_guard.py`：计划任务每 5 分钟检查一次，桥不在就拉起
- 设备菜单的「播报 / 提示音」开关与桥侧真值**双向同步**（开关存 `config.json`）

## 怎么跑

**固件**（需要 ESP-IDF v5.5）

```bash
# 编译（Windows，venv 已配好的直调方式）
tools\idf-build-direct.bat

# 刷机：只写 0x0 / 0x8000 / 0x10000 三个区，**保留 nvs**（BLE 配对不用重做）
tools\idf-flash-safe.bat
```

**桥**（Windows，Python 3.11）

```bash
# 依赖：bleak, psutil, faster-whisper（转写模型 large-v3-turbo-ct2）
python bridge\hermes_passport_bridge.py

# 计划任务（登录自启 + 每 5 分钟看门狗）
schtasks /create /tn HermesPassportBridge      /tr "pythonw.exe <路径>\hermes_passport_bridge.py" /sc onlogon
schtasks /create /tn HermesPassportBridgeGuard /tr "pythonw.exe <路径>\passport_bridge_guard.py" /sc minute /mo 5
```

**配置**：把 `bridge/config.example.json` 复制成 `bridge/config.json` 再改
（`owner_name` 是设备屏上显示的归属名 —— **别把真名写进代码**，那个文件不进仓库）。

## 已知限制

- **屏幕只显示摘要，不显示全文** —— 不是懒，是 BLE **无确认写入**：
  1000 字节要分约 50 个包，丢任何一个，设备收到的就是残缺 JSON、**整条丢弃不更新**。
  摘要约 240 字节（约 12 包）才稳。要看全文请回电脑。
- **正文缓冲只有 512 字节**（约 170 汉字）—— ESP32-C3 的**静态**内存本来就紧
  （LVGL 画布 38KB + GB2312 字符集 + BLE 协议栈）。实测把它调到 3072 并加 10×1024
  的历史缓冲后，**设备直接不广播 BLE 了**（编译不报错，运行时静默失败）。
- **设备重启后对话历史会清空**（历史目前只存在内存里）。
- **设备无电量显示。**

## 文件结构

```
firmware/main/     ESP-IDF 应用（屏幕 / BLE / 语音 / 状态机 / 排版 / 设置）
bridge/            桌面桥：BLE 收发、whisper 转写、按键注入、TTS 播报、看门狗
tools/             构建与刷机脚本、字库生成、GB2312 字符集
docs/              改动归档与交付检查
```

## 排查手册

改这个项目的坑基本都记在 `docs/` 的归档里，另外本机 Hermes 技能
`folotoy-ai-passport` 的 `references/hermes-bridge-pitfalls.md` 有一份更细的
「踩坑 → 现象 → 根因 → 正解」清单（含本 README「已知限制」里那些数字的由来）。
