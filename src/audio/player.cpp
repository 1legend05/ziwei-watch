#include "player.h"
#include "pin_config.h"
#include <Arduino.h>
#include "driver/i2s.h"
#include "esp_log.h"
#include "esp_err.h"
#include "Wire.h"
#include "es8311.h"
#include "HWCDC.h"
extern HWCDC USBSerial;
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PL_TAG   "PLAY"
#define PL_RATE  16000u

static es8311_handle_t g_es = NULL;
static bool      g_inited = false;
static volatile bool g_playing = false;
static volatile bool g_paused = false;   // 暂停请求（播报中随时可暂停/继续，播放循环内生效）
static volatile bool g_stop = false;     // 打断请求（pl_stop 置位，播放循环检测后立即退出）

// 录音侧 I2S1（RX）引脚 —— 播放结束后恢复占用，保证录音链路不被破坏（旧版 i2s_pin_config_t 命名）
static const i2s_pin_config_t s_recPins = {
    .mck_io_num = PIN_ES7210_MCLK,
    .bck_io_num = PIN_ES7210_BCLK,
    .ws_io_num = PIN_ES7210_LRCK,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = PIN_ES7210_DIN,
};

// 播放侧 I2S0（TX）引脚 —— ES8311 与 ES7210 共用 BCLK/LRCK/MCLK，DOUT 独占 GPIO8
static const i2s_pin_config_t s_playPins = {
    .mck_io_num = PIN_ES7210_MCLK,
    .bck_io_num = PIN_ES7210_BCLK,
    .ws_io_num = PIN_ES7210_LRCK,
    .data_out_num = PIN_ES8311_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE,
};

// 播放侧 I2S0（TX）配置 —— 全局保存，供每次播放前重装驱动
static i2s_config_t s_tx;

static void pl_log(const char *m) {
    USBSerial.println(m);   // USB CDC 输出（监控 COM8 可见；ESP_LOGW 走 UART0 无法监控）
}

// 重配 ES8311 采样率（含内部时钟分频）
static bool pl_codec_rate(uint32_t rate) {
    if (!g_es) return false;
    return es8311_sample_frequency_config(g_es, (int)(rate * 256), (int)rate) == ESP_OK;
}

// 将 I2S 引脚占用交给播放（I2S0 TX）；播放完成后调用 pl_release_bus 还回录音（I2S1 RX）
// 每次播放前彻底重装 I2S0：beep 首次播放成功、后续 wav 播放失败的根因是驱动在重复 i2s_set_pin
// 切换后状态残留（时钟未恢复），卸载→重装→配 pin 保证每次都是全新干净状态
static bool pl_take_bus(void) {
    i2s_driver_uninstall(I2S_NUM_0);
    if (i2s_driver_install(I2S_NUM_0, &s_tx, 0, NULL) != ESP_OK) {
        pl_log("take_bus install fail");
        return false;
    }
    if (i2s_set_pin(I2S_NUM_0, &s_playPins) != ESP_OK) {
        pl_log("take_bus set_pin fail");
        return false;
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    return true;
}

static void pl_release_bus(void) {
    i2s_set_pin(I2S_NUM_1, &s_recPins);
}

bool pl_init(void) {
    if (g_inited) return true;

    Wire.begin(IIC_SDA, IIC_SCL);
    pinMode(PA, OUTPUT);
    digitalWrite(PA, HIGH); // 功放使能（高电平）

    g_es = es8311_create(0, ES8311_ADDRESS_0);
    if (!g_es) { pl_log("es8311_create fail"); return false; }

    const es8311_clock_config_t clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = (int)(PL_RATE * 256),
        .sample_frequency = (int)PL_RATE,
    };
    if (es8311_init(g_es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        pl_log("es8311_init fail"); return false;
    }
    // 官方 demo 07 在 init 之后必须显式设置采样频率分频，否则 DAC 时钟可能未生效导致语音播放异常
    es8311_sample_frequency_config(g_es, clk.mclk_frequency, clk.sample_frequency);
    // 官方关闭麦克风通路，确保 DAC 输出干净
    es8311_microphone_config(g_es, false);
    es8311_voice_volume_set(g_es, 85, NULL);
    es8311_voice_mute(g_es, false);

    // 与录音侧一致的结构（旧版 I2S 驱动）；播放用 I2S0 TX，录音保持 I2S1 RX
    i2s_config_t tx = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = (int)PL_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
        .chan_mask = (i2s_channel_t)(I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1),
    };
    s_tx = tx;   // 保存全局，供每次播放前重装驱动
    if (i2s_driver_install(I2S_NUM_0, &tx, 0, NULL) != ESP_OK) {
        pl_log("i2s0 install fail"); return false;
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    g_inited = true;
    return true;
}

bool pl_playing(void) { return g_playing; }

// 播报暂停/继续：只置跨核标志，播放循环内检测后阻塞等待；暂停点位置保存在循环局部变量 done
bool pl_pause(void)  { if (!g_playing) return false; g_paused = true; return true; }
bool pl_resume(void) { g_paused = false; return true; }
bool pl_paused(void) { return g_paused; }
bool pl_stop(void)   { g_stop = true; g_paused = false; return true; }   // 置停止并解除暂停阻塞，播放循环立即退出

// 播放 16bit PCM（单声道，内部复制为双声道输出），阻塞直到写完
bool pl_play_pcm16(const int16_t *pcm, size_t sampleCount) {
    USBSerial.printf("PCM16-ENTER n=%d\n", (int)sampleCount);
    if (!g_inited && !pl_init()) { pl_log("pl not inited"); return false; }
    if (g_playing) { pl_log("already playing"); return false; }
    g_stop = false;          // 复位打断标志（本次播放从头开始）
    g_playing = true;

    bool ok = true;
    if (!pl_take_bus()) { g_playing = false; return false; }

    // 单声道 → 双声道：每样本写 L/R 两份
    int16_t pair[2];
    const size_t CHUNK = 2048;
    // 预分配双声道缓冲一次，避免循环内反复 ps_malloc 导致中途失败/PSRAM 碎片化中断播放
    int16_t *pairBuf = (int16_t *)ps_malloc(CHUNK * 2 * sizeof(int16_t));
    if (!pairBuf) { pl_log("pairBuf malloc fail"); pl_release_bus(); g_playing = false; return false; }
    size_t done = 0;
    while (done < sampleCount && !g_stop) {
        // 暂停支持：暂停请求时清空 DMA 输出并等待恢复（不退出播放循环，进度保存在 done）
        if (g_paused) {
            i2s_zero_dma_buffer(I2S_NUM_0);
            USBSerial.println("PAUSE-ENTER");
            while (g_paused && !g_stop) vTaskDelay(pdMS_TO_TICKS(50));
            i2s_zero_dma_buffer(I2S_NUM_0);   // 恢复前清缓冲，避免上一段残留导致爆音
            USBSerial.printf("PAUSE-RESUME done=%d/%d\n", (int)done, (int)sampleCount);
        }
        if (g_stop) break;   // 打断请求：立即退出（不恢复暂停，不继续播放）
        size_t want = sampleCount - done;
        if (want > CHUNK) want = CHUNK;
        for (size_t i = 0; i < want; i++) {
            pair[0] = pcm[done + i];
            pair[1] = pcm[done + i];
            memcpy(pairBuf + i * 2, pair, 4);
        }
        // 按实际写入字节推进 + 超时加大(1s) + 失败重试，杜绝中途写入超时/部分写入错位导致的截断
        size_t remain = want * 4;
        size_t base = 0;
        int retry = 0;
        bool chunkOk = true;
        while (remain > 0) {
            size_t written = 0;
            esp_err_t e = i2s_write(I2S_NUM_0, (const char*)pairBuf + base, remain, &written, pdMS_TO_TICKS(1000));
            if (e != ESP_OK || written == 0) {
                if (++retry >= 3) { chunkOk = false; break; }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            base += written;
            remain -= written;
            retry = 0;
        }
        if (!chunkOk) { ok = false; break; }
        done += want;
        if ((done & 0x1FFF) == 0) USBSerial.printf("pcm16 done=%d/%d\n", (int)done, (int)sampleCount);
    }
    free(pairBuf);
    i2s_zero_dma_buffer(I2S_NUM_0);
    pl_release_bus();
    g_playing = false;
    USBSerial.printf("PCM16-EXIT ok=%d done=%d/%d\n", (int)ok, (int)done, (int)sampleCount);
    return ok;
}

// 播放完整 WAV：解析头后提取 16bit PCM 样本，复用 pl_play_pcm16（与提示音完全相同的播放路径，已实测出声）
bool pl_play_wav(const uint8_t *wav, size_t len) {
    USBSerial.println("PLAYWAV-ENTER");
    if (!wav || len < 44) { pl_log("wav too short"); return false; }
    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        pl_log("not riff/wave"); return false;
    }
    USBSerial.println("PLAYWAV-RIFF-OK");
    uint32_t rate = PL_RATE;
    uint16_t channels = 1;
    uint16_t bits = 16;
    size_t dataOff = 0, dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= len) {
        char id[4]; memcpy(id, wav + pos, 4);
        uint32_t sz = (uint32_t)(wav[pos + 4]) | ((uint32_t)wav[pos + 5] << 8) |
                      ((uint32_t)wav[pos + 6] << 16) | ((uint32_t)wav[pos + 7] << 24);
        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            uint16_t fmt = wav[pos + 8] | (wav[pos + 9] << 8);
            channels = wav[pos + 10] | (wav[pos + 11] << 8);
            rate = (uint32_t)wav[pos + 12] | ((uint32_t)wav[pos + 13] << 8) |
                   ((uint32_t)wav[pos + 14] << 16) | ((uint32_t)wav[pos + 15] << 24);
            bits = wav[pos + 22] | (wav[pos + 23] << 8);
            if (fmt != 1) { pl_log("not pcm"); return false; }
        } else if (memcmp(id, "data", 4) == 0) {
            dataOff = pos + 8;
            dataLen = sz;
            if (dataOff + dataLen > len) dataLen = len - dataOff;
            break;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (!dataOff || !dataLen || bits != 16) { pl_log("no data"); return false; }
    if (channels != 1 && channels != 2) { pl_log("unsupported ch"); return false; }
    USBSerial.printf("wav parsed rate=%d ch=%d bits=%d dataLen=%d\n", (int)rate, (int)channels, (int)bits, (int)dataLen);

    // 采样率与 I2S 不同时重配 codec（TTS 恒为 16k 不触发）
    if (rate != PL_RATE) {
        if (!pl_codec_rate(rate)) { pl_log("codec rate fail"); return false; }
    }

    // 提取 16bit 样本：mono 直接引用 wav data 区（零拷贝，长文本不额外占用 PSRAM）；stereo 取左声道
    size_t frameBytes = (bits / 8) * channels;
    size_t sampleCount = dataLen / frameBytes;
    if (sampleCount == 0) { pl_log("no samples"); return false; }
    int16_t *samples = nullptr;
    if (channels == 1) {
        samples = (int16_t *)(wav + dataOff);   // 直接引用，播放完不 free
    } else {
        samples = (int16_t *)ps_malloc(sampleCount * sizeof(int16_t));
        if (!samples) { pl_log("samples malloc fail"); return false; }
        for (size_t i = 0; i < sampleCount; i++) {
            memcpy(&samples[i], wav + dataOff + i * 4, 2);   // 取左声道
        }
    }
    // 样本内容检查：前 8 个样本值 + 全段非零统计（确认 wav 数据在固件侧有效）
    int nz = 0;
    for (size_t i = 0; i < sampleCount && i < 40000; i++) if (samples[i] != 0) nz++;
    USBSerial.printf("SAMPLES n=%d nz0-40k=%d head=%d,%d,%d,%d\n", (int)sampleCount, nz,
                     samples[0], samples[1], samples[2], samples[3]);

    bool ok = pl_play_pcm16(samples, sampleCount);   // 与提示音同一播放路径
    if (channels != 1) free(samples);                 // 零拷贝 mono 不释放
    USBSerial.printf("wav playback ok=%d\n", (int)ok);
    return ok;
}

// 内置提示音：1200Hz 短促“叮”，带衰减包络，约 300ms
bool pl_play_beep(void) {
    const uint32_t rate = PL_RATE;
    const uint32_t ms = 300;
    size_t n = rate * ms / 1000;
    int16_t *buf = (int16_t *)ps_malloc(n * sizeof(int16_t));
    if (!buf) return false;
    for (size_t i = 0; i < n; i++) {
        double t = (double)i / rate;
        double env = 1.0 - 0.75 * (double)i / (double)n;
        double v = 0.55 * env * sin(2.0 * M_PI * 1200.0 * t);
        buf[i] = (int16_t)(v * 32767.0);
    }
    bool ok = pl_play_pcm16(buf, n);
    free(buf);
    return ok;
}
