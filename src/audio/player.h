#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// 初始化 ES8311 解码器 + I2S0 TX 播放通道（每次播放前会自动抢占 I2S 引脚，
// 播放结束后释放回录音用的 I2S1，实现录音/播放分时复用同一条 I2S 总线）
bool pl_init(void);

// 播放一段 16bit/16kHz 单声道 PCM（内部会转成立体声左右声道），阻塞直到播完
bool pl_play_pcm16(const int16_t *pcm, size_t sampleCount);

// 播放完整 WAV（支持 16bit/8k~48k，单/双声道，自动读采样率并重配 codec），阻塞直到播完
bool pl_play_wav(const uint8_t *wav, size_t len);

// 播放内置提示音（“叮”，约 300ms，16kHz），阻塞直到播完
bool pl_play_beep(void);

// 是否正在播放（供 UI 屏蔽录音启动等）
bool pl_playing(void);

// 播报暂停/继续（播放循环内生效，随时可切换；暂停点保存在播放进度处，继续后接着读）
bool pl_pause(void);
bool pl_resume(void);
bool pl_paused(void);

// 打断播放：立即退出当前播放循环（解除暂停阻塞并置停止标志），供"播报中按 PWR 打断"使用
bool pl_stop(void);
