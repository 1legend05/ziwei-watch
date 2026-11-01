#pragma once
// ui.h —— 圆屏 UI（LVGL 8.4）

#include <lvgl.h>

// 初始化 UI（创建主页）
void ui_init();

// 页面切换（单屏主界面 + 覆盖式设置页）
void ui_showHome();
void ui_showSettings();

// 全局字体（由 scripts 生成的 LVGL 中文字体）
extern lv_font_t ziwei_font_28;
extern lv_font_t ziwei_font_22;

// 供 main loop 轮询：处理后台请求结果刷新
void ui_poll();

// 屏幕亮度（main.cpp 实现于 gfx 作用域；供空闲自动降亮度/触摸恢复使用）
void ui_setBrightness(uint8_t v);

// 交互活动通知（main.cpp 触摸/按键触发）：记录活动时间并恢复亮度
void ui_notifyActivity();
