/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FAP_SCREENSHOT_V1 —— 串口截屏协议（只读观察）
 *
 * 主机经 USB-Serial/JTAG 发送 ASCII 行 "FAP_SCREENSHOT_V1\n"，
 * 设备回 "FAP_SCREENSHOT_V1 <宽> <高> RGB565LE <字节数>\n" 头行，
 * 紧跟恰好 <字节数> 字节的紧密小端 RGB565 像素。
 *
 * 契约：纯观察——不重启、不刷机、不改设置；任何失败只记日志不应答，
 * 让主机得到干净的超时错误，而不是一条被破坏的流。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动截屏监听任务（幂等性由调用方保证，只调用一次）。 */
esp_err_t fap_screenshot_start(void);

#ifdef __cplusplus
}
#endif
