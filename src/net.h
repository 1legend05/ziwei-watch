#pragma once
// net.h —— WiFi 连接与服务器 API 调用

#include <Arduino.h>

// 连接 WiFi；返回 true 表示成功
bool net_connectWifi(const String& ssid, const String& pass, unsigned long timeoutMs = 15000);

// 是否已连接
bool net_isConnected();

// 扫描附近 WiFi（同步阻塞约 2-4 秒）；out 数组接收 SSID（最多 max 个），返回找到的数量
int net_scanNetworks(String* out, int max);

// POST JSON 到 {server}{path}，body 为 JSON 字符串；返回响应字符串（成功）或空串（失败）
String net_postJson(const String& path, const String& jsonBody, unsigned long timeoutMs = 60000);

// POST WAV 音频（multipart/form-data, 字段 file）到 {server}{path}；返回响应字符串
String net_postAudio(const String& path, const uint8_t* data, size_t len,
                     const String& filename, unsigned long timeoutMs = 60000);

// POST 裸字节（屏幕快照等）到 {server}{path}；返回响应字符串
String net_postRaw(const String& path, const uint8_t* data, size_t len, unsigned long timeoutMs = 60000);

// GET 下载二进制资源（TTS WAV 等）到 PSRAM；成功返回 true 并填充 buf/len（调用方 heap_caps_free）
bool net_getBinary(const String& path, uint8_t** outBuf, size_t* outLen, unsigned long timeoutMs = 30000);

// ── 业务 API 封装 ──
// AI 解读：返回 {"ok":true,"summary":"...","text":"..."} 或 {"ok":false,"error":"..."}
String api_interpret(int year, int month, int day, int hour, const char* gender);
// 多轮对话：messages 为 JSON 数组字符串（[{"role":"user","content":"..."}]）；summary 可为空
String api_chat(const String& messagesJson, const String& summary);
// 语音识别：上传 WAV，返回 {"ok":true,"text":"..."}
String api_asr(const uint8_t* wav, size_t len);
// 语音排盘：ASR 文本（+ 上轮已收集 birth JSON，多轮补全）→ 服务器解析/追问或排盘 + AI 解读
String api_voiceInterpret(const String& text, const String& partial = "");
