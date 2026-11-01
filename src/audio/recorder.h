#pragma once
// recorder.h —— 麦克风录音（ES7210 + I2S），输出 16kHz/16bit/mono WAV

#include <Arduino.h>

// 初始化麦克风；成功返回 true
bool rec_begin();

// 录音最多 durationMs 毫秒，生成 WAV 数据（16kHz/16bit/mono，PSRAM 分配）
// stopFlag 非空时：置 true 则提前结束（“按住说话”松手）
// *wavOut 指向分配缓冲区（含 44 字节 WAV 头），*wavLen 为总长度；用完调用 rec_free 释放
// 录音过短（< 200ms）返回 false
bool rec_record(size_t durationMs, uint8_t** wavOut, size_t* wavLen, volatile bool* stopFlag = nullptr);

void rec_free(uint8_t* p);
