#include "recorder.h"
#include <driver/i2s.h>
#include "es7210.h"
#include "pin_config.h"
#include "HWCDC.h"

extern HWCDC USBSerial;

// 调试日志开关：串口打印录音各步骤结果，便于排查“录音失败”
#define REC_DEBUG 1
#if REC_DEBUG
#define RLOG(fmt, ...) USBSerial.printf(fmt "\n", ##__VA_ARGS__)
#else
#define RLOG(fmt, ...)
#endif

#define SAMPLE_RATE 16000
#define I2S_CH I2S_NUM_1

static bool initialized = false;

bool rec_begin() {
  if (initialized) { RLOG("[rec] already initialized"); return true; }
  Wire.begin(IIC_SDA, IIC_SCL);
  delay(200);

  audio_hal_codec_config_t cfg = {
    .adc_input = AUDIO_HAL_ADC_INPUT_ALL,
    .codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE,
    .i2s_iface = {
      .mode = AUDIO_HAL_MODE_SLAVE,
      .fmt = AUDIO_HAL_I2S_NORMAL,
      .samples = AUDIO_HAL_16K_SAMPLES,
      .bits = AUDIO_HAL_BIT_LENGTH_16BITS,
    },
  };

  uint32_t ret = ESP_OK;
  ret |= es7210_adc_init(&Wire, &cfg);
  RLOG("[rec] es7210_adc_init ret=%08x", ret);
  ret |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  RLOG("[rec] es7210_config_i2s ret=%08x", ret);
  ret |= es7210_adc_set_gain(
    (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2),
    (es7210_gain_value_t)GAIN_24DB);
  RLOG("[rec] es7210_gain12 ret=%08x", ret);
  ret |= es7210_adc_set_gain(
    (es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
    (es7210_gain_value_t)GAIN_37_5DB);
  RLOG("[rec] es7210_gain34 ret=%08x", ret);
  ret |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
  RLOG("[rec] es7210_ctrl ret=%08x", ret);
  if (ret != ESP_OK) { RLOG("[rec] es7210 init FAIL"); return false; }

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ALL_LEFT,
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

  i2s_pin_config_t pins = {0};
  pins.bck_io_num = PIN_ES7210_BCLK;
  pins.ws_io_num = PIN_ES7210_LRCK;
  pins.data_in_num = PIN_ES7210_DIN;
  pins.mck_io_num = PIN_ES7210_MCLK;
  if (i2s_driver_install(I2S_CH, &i2s_config, 0, NULL) != ESP_OK) { RLOG("[rec] i2s install FAIL"); return false; }
  i2s_set_pin(I2S_CH, &pins);
  i2s_zero_dma_buffer(I2S_CH);

  initialized = true;
  RLOG("[rec] init done");
  return true;
}

// 写 WAV 头（44 字节，PCM 16bit mono）
static void writeWavHeader(uint8_t* h, uint32_t dataLen) {
  uint32_t chunkSize = 36 + dataLen;
  uint32_t byteRate = SAMPLE_RATE * 2;  // 16bit mono
  h[0] = 'R'; h[1] = 'I'; h[2] = 'F'; h[3] = 'F';
  h[4] = chunkSize & 0xFF; h[5] = (chunkSize >> 8) & 0xFF;
  h[6] = (chunkSize >> 16) & 0xFF; h[7] = (chunkSize >> 24) & 0xFF;
  h[8] = 'W'; h[9] = 'A'; h[10] = 'V'; h[11] = 'E';
  h[12] = 'f'; h[13] = 'm'; h[14] = 't'; h[15] = ' ';
  h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;      // fmt chunk 16
  h[20] = 1; h[21] = 0;                               // PCM
  h[22] = 1; h[23] = 0;                               // mono
  h[24] = SAMPLE_RATE & 0xFF; h[25] = (SAMPLE_RATE >> 8) & 0xFF;
  h[26] = (SAMPLE_RATE >> 16) & 0xFF; h[27] = (SAMPLE_RATE >> 24) & 0xFF;
  h[28] = byteRate & 0xFF; h[29] = (byteRate >> 8) & 0xFF;
  h[30] = (byteRate >> 16) & 0xFF; h[31] = (byteRate >> 24) & 0xFF;
  h[32] = 2; h[33] = 0;                               // block align
  h[34] = 16; h[35] = 0;                              // bits per sample
  h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
  h[40] = dataLen & 0xFF; h[41] = (dataLen >> 8) & 0xFF;
  h[42] = (dataLen >> 16) & 0xFF; h[43] = (dataLen >> 24) & 0xFF;
}

bool rec_record(size_t durationMs, uint8_t** wavOut, size_t* wavLen, volatile bool* stopFlag) {
  if (!initialized) { RLOG("[rec] record FAIL: not initialized"); return false; }
  size_t pcmBytes = (size_t)SAMPLE_RATE * 2 * durationMs / 1000;
  size_t total = 44 + pcmBytes;
  uint8_t* buf = (uint8_t*)ps_malloc(total);
  if (!buf) { RLOG("[rec] record FAIL: ps_malloc(%d) null", (int)total); return false; }
  RLOG("[rec] record start: %d ms, buf %d bytes, psram free=%d",
       (int)durationMs, (int)total, (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  size_t readBytes = 0;
  int16_t chunk[512];
  int idleCount = 0;   // 连续无数据次数，超限判硬件无数据
  // 静音自动停止（VAD）：检测到过语音后，停顿 2 秒自动结束录音，
  // 避免用户说完话不按停止键时录满 60s 环境音，把说话内容淹没在静音里
  int voiceDetected = 0;
  int silenceMs = 0;
  while (readBytes < pcmBytes) {
    if (stopFlag && *stopFlag) break;   // “按住说话”松手提前结束
    size_t want = pcmBytes - readBytes;
    if (want > sizeof(chunk)) want = sizeof(chunk);
    size_t got = 0;
    // 用短超时轮询，避免硬件无数据时永久阻塞
    esp_err_t err = i2s_read(I2S_CH, (char*)(buf + 44 + readBytes), want, &got, pdMS_TO_TICKS(300));
    if (err == ESP_ERR_TIMEOUT) {
      if (++idleCount > 10) {          // 3 秒无任何数据 → 硬件异常
        RLOG("[rec] record FAIL: no i2s data (idle)");
        heap_caps_free(buf);
        return false;
      }
      continue;
    }
    if (err != ESP_OK) {
      RLOG("[rec] record FAIL: i2s_read err=%d", (int)err);
      heap_caps_free(buf);
      return false;
    }
    idleCount = 0;
    readBytes += got;
    // VAD：本段内任一采样幅值超阈值视为有语音；检测到语音后连续静音 2s 自动停
    bool voiced = false;
    int16_t* p = (int16_t*)(buf + 44 + readBytes - got);
    for (size_t i = 0; i < got / 2; i++) {
      int v = p[i]; if (v < 0) v = -v;
      if (v > 800) { voiced = true; break; }
    }
    if (voiced) { voiceDetected = 1; silenceMs = 0; }
    else if (voiceDetected) {
      silenceMs += (int)(got * 1000 / (SAMPLE_RATE * 2));
      if (silenceMs >= 2000) break;   // 说完停顿 2s → 自动停止并识别
    }
  }
  RLOG("[rec] record end: readBytes=%d (%.1f ms)", (int)readBytes, readBytes * 1000.0 / (SAMPLE_RATE * 2));
  // 太短的录音视为失败（< 200ms）
  if (readBytes < SAMPLE_RATE * 2 / 5) {
    RLOG("[rec] record FAIL: too short");
    heap_caps_free(buf);
    return false;
  }
  writeWavHeader(buf, readBytes);
  *wavOut = buf;
  *wavLen = 44 + readBytes;
  RLOG("[rec] record OK: wav %d bytes", (int)(44 + readBytes));
  return true;
}

void rec_free(uint8_t* p) { if (p) heap_caps_free(p); }   // ps_malloc(PSRAM) 必须用 heap_caps_free，普通 free() 会损坏堆
