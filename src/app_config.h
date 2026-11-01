#pragma once
// app_config.h —— 运行配置（WiFi / 服务器地址），存储在 NVS（Preferences）

#include <Arduino.h>
#include <Preferences.h>

// ─── 默认值（首次启动使用；可在设备"设置"页修改并保存）───
// 注意：以下为开源占位符，请勿提交真实 WiFi 凭据。首次烧录后请在设备
// "设置"页填入你自己的 WiFi / 服务器地址并保存（写入 NVS，重启保留）。
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID "YOUR_WIFI_SSID"
#endif
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif
#ifndef DEFAULT_SERVER_URL
#define DEFAULT_SERVER_URL "http://192.168.1.100:3000"
#endif

struct AppConfig {
  String wifi_ssid;
  String wifi_pass;
  String server;        // 服务器根地址，如 http://192.168.1.207:3000
  String birthInfo;     // 最近一次生辰，格式 "1990-1-1-0-male"
  uint8_t bright;       // 屏幕亮度（30-255），设置页可调，重启保留
};

class AppConfigStore {
 public:
  void begin() {
    prefs.begin("ziwei", false);
    // NVS 中可能存了空值（旧版 placeholder 误保存导致），空值一律回退默认
    String s = prefs.getString("wifi_ssid", "");
    wifi_ssid = s.length() ? s : DEFAULT_WIFI_SSID;
    String p = prefs.getString("wifi_pass", "");
    wifi_pass = p.length() ? p : DEFAULT_WIFI_PASS;
    String sv = prefs.getString("server", "");
    server = sv.length() ? sv : DEFAULT_SERVER_URL;
    birth = prefs.getString("birth", "");
    if (server.endsWith("/")) server.remove(server.length() - 1);
    uint8_t b = prefs.getUChar("bright", 0);
    bright = (b >= 30 && b <= 255) ? b : 220;   // 无记录/异常值回退全亮 220
  }
  void saveWifi(const String& ssid, const String& pass) {
    wifi_ssid = ssid; wifi_pass = pass;
    prefs.putString("wifi_ssid", ssid);
    prefs.putString("wifi_pass", pass);
  }
  void saveServer(const String& s) {
    server = s;
    if (server.endsWith("/")) server.remove(server.length() - 1);
    prefs.putString("server", server);
  }
  void saveBirth(const String& b) {
    birth = b;
    prefs.putString("birth", b);
  }
  void saveBright(uint8_t b) {
    bright = b;
    prefs.putUChar("bright", b);
  }

  String wifi_ssid, wifi_pass, server, birth;
  uint8_t bright;

 private:
  Preferences prefs;
};

extern AppConfigStore g_config;
