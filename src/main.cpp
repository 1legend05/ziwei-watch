// main.cpp —— 紫微斗数 AI 圆屏固件入口
// 硬件：Waveshare ESP32-S3-Touch-AMOLED-1.75C
// 依赖：LVGL 8.4 + GFX_Library_for_Arduino (CO5300) + SensorLib (CST9217)
#include <Arduino.h>
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include "TouchDrvCSTXXX.hpp"
#include "lv_conf.h"
#include "HWCDC.h"
#include <Wire.h>

#include "app_config.h"
#include "net.h"
#include "ui.h"
#include "audio/recorder.h"
#include <WiFi.h>

HWCDC USBSerial;
#define LVGL_TICK_MS 2

uint32_t screenWidth, screenHeight;
static lv_disp_draw_buf_t draw_buf;

Arduino_DataBus* bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300* gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

TouchDrvCST92xx touch;
int16_t tx[5], ty[5];
volatile bool touchTriggered = false;

// 触摸中断
void IRAM_ATTR onTouchIrq() { touchTriggered = true; }

static void lvglTickCb(void*) { lv_tick_inc(LVGL_TICK_MS); }

// 圆屏：确保刷新区块为偶数对齐
static void rounderCb(lv_disp_drv_t* drv, lv_area_t* area) {
  (void)drv;
  if (area->x1 % 2 != 0) area->x1--;
  if (area->y1 % 2 != 0) area->y1--;
  if (area->x2 % 2 == 0) area->x2++;
  if (area->y2 % 2 == 0) area->y2++;
}

static void dispFlush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t*)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)&color_p->full, w, h);
#endif
  lv_disp_flush_ready(disp);
}

static void touchRead(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  (void)drv;
  if (touchTriggered) {
    uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
    if (n) {
      if (data->state != LV_INDEV_STATE_PR) USBSerial.printf("[touch] PR at %d,%d\n", tx[0], ty[0]);
      data->state = LV_INDEV_STATE_PR;
      data->point.x = tx[0];
      data->point.y = ty[0];
      touchTriggered = false;
      ui_notifyActivity();   // 触摸=交互活动：空闲自动降亮后立即恢复全亮
    } else {
      data->state = LV_INDEV_STATE_REL;
    }
  }
}

// WiFi 连接后台任务（不阻塞 UI）
static void wifiTask(void*) {
  USBSerial.printf("[wifi] task start heap=%d\n", (int)ESP.getFreeHeap());
  for (;;) {
    if (!net_isConnected()) {
      USBSerial.printf("[wifi] begin connect...\n");
      net_connectWifi(g_config.wifi_ssid, g_config.wifi_pass, 15000);
      if (net_isConnected()) {
        USBSerial.printf("[wifi] connected, IP=%s RSSI=%d\n",
                         WiFi.localIP().toString().c_str(), WiFi.RSSI());
      } else {
        USBSerial.println("[wifi] connect FAIL, will retry");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

void setup() {
  USBSerial.begin(115200);
  delay(100);
  USBSerial.println("ziwei-watch boot");
  delay(1200);   // 等待 USB/电源稳定，缓解首次上电偶发 IPC1 栈崩溃

  g_config.begin();
  USBSerial.printf("wifi=%s server=%s\n", g_config.wifi_ssid.c_str(), g_config.server.c_str());

  // 触摸
  Wire.begin(IIC_SDA, IIC_SCL);
  digitalWrite(TP_RST, LOW);
  delay(30);
  digitalWrite(TP_RST, HIGH);
  delay(50);
  delay(1000);
  Wire.begin(IIC_SDA, IIC_SCL);
  touch.setPins(TP_RST, TP_INT);
  bool tOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
  USBSerial.printf("touch online: %d, model: %s\n", tOk, touch.getModelName());
  touch.setMaxCoordinates(466, 466);
  touch.setMirrorXY(true, true);

  // 屏幕（先初始化 SPI 屏，再装触摸中断，避免初始化期 IPC 栈压力叠加）
  gfx->begin();
  gfx->setBrightness(g_config.bright);   // 亮度档位来自设置页（默认 220）
  screenWidth = gfx->width();
  screenHeight = gfx->height();
  attachInterrupt(TP_INT, onTouchIrq, FALLING);

  // LVGL 双缓冲 1/16 屏 ×2：在 1/10 屏基础上再缩小，为常驻网络任务栈和 WiFi/SHA 腾出内部 SRAM
  lv_init();
  const lv_coord_t BUF_PIX = screenWidth * screenHeight / 16;
  lv_color_t* buf1 = (lv_color_t*)heap_caps_malloc(BUF_PIX * sizeof(lv_color_t), MALLOC_CAP_DMA);
  lv_color_t* buf2 = (lv_color_t*)heap_caps_malloc(BUF_PIX * sizeof(lv_color_t), MALLOC_CAP_DMA);
  if (!buf1 || !buf2) {
    USBSerial.println("LVGL DMA buffer alloc failed");
    while (1) delay(100);
  }
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, BUF_PIX);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = dispFlush;
  disp_drv.rounder_cb = rounderCb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touchRead;
  lv_indev_drv_register(&indev_drv);

  esp_timer_handle_t tickTimer = nullptr;
  esp_timer_create_args_t tickArgs = { .callback = lvglTickCb, .name = "lvgl_tick" };
  esp_timer_create(&tickArgs, &tickTimer);
  esp_timer_start_periodic(tickTimer, LVGL_TICK_MS * 1000);

  // 麦克风（ES7210 + I2S）：在主线程初始化，避免与触摸 I2C 并发
  bool micOk = rec_begin();
  USBSerial.printf("mic init: %d\n", micOk);

  // 开机自检录音：录 1 秒并打印数据量与音量，验证麦克风数据通路
  if (micOk) {
    USBSerial.printf("[selftest] psram total=%d free=%d\n",
                     (int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                     (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    uint8_t* wav = nullptr;
    size_t wavLen = 0;
    bool ok = rec_record(1000, &wav, &wavLen);
    if (ok && wav) {
      int16_t* pcm = (int16_t*)(wav + 44);
      int samples = (wavLen - 44) / 2;
      int32_t peak = 0; int32_t sum = 0;
      for (int i = 0; i < samples; i++) {
        int v = pcm[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
        sum += v;
      }
      USBSerial.printf("[selftest] rec OK: %d samples, peak=%d avg=%d\n", samples, (int)peak, (int)(samples ? sum / samples : 0));
      rec_free(wav);
    } else {
      USBSerial.println("[selftest] rec FAIL (no audio data path)");
    }
  }

  // UI + WiFi
  ui_init();
  BaseType_t r = xTaskCreatePinnedToCore(wifiTask, "wifi", 8192, nullptr, 1, nullptr, 1);
  USBSerial.printf("[wifi] task created=%d heap=%d\n", (int)r, (int)ESP.getFreeHeap());
  USBSerial.println("setup done");
}

void loop() {
  lv_timer_handler();
  ui_poll();
  // 每 5s 打印 WiFi 状态与内部堆，便于诊断
  static unsigned long lastDbg = 0;
  if (millis() - lastDbg > 5000) {
    lastDbg = millis();
    USBSerial.printf("[dbg] wifi=%d heap=%d\n", (int)WiFi.status(), (int)ESP.getFreeHeap());
  }
  delay(5);
}

// 屏幕亮度（gfx 在全局作用域）：供 ui 空闲自动降亮度（OLED 保护）/触摸恢复
void ui_setBrightness(uint8_t v) { gfx->setBrightness(v); }
