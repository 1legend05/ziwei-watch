#include "net.h"
#include "app_config.h"
#include <WiFi.h>
#include <HTTPClient.h>

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  #define DBG_USB Serial
#else
  #define DBG_USB Serial
#endif

bool net_connectWifi(const String& ssid, const String& pass, unsigned long timeoutMs) {
  if (ssid.length() == 0 || ssid == "YOUR_WIFI_SSID") return false;
  WiFi.mode(WIFI_STA);
  // 提升发射功率到最大（弱信号下增强上行可靠性）
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  // 清理残留连接状态，避免反复 begin 触发 0x3007 (ESP_ERR_WIFI_CONN)
  WiFi.disconnect(true, false);
  delay(150);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeoutMs) return false;
    delay(200);
  }
  return true;
}

bool net_isConnected() { return WiFi.status() == WL_CONNECTED; }

int net_scanNetworks(String* out, int max) {
  WiFi.mode(WIFI_STA);   // 扫描需 STA 模式
  int n = WiFi.scanNetworks();   // 同步扫描，阻塞约 2-4 秒
  if (n < 0) n = 0;
  int cnt = 0;
  for (int i = 0; i < n && cnt < max; i++) {
    String ssid = WiFi.SSID(i);
    ssid.trim();
    if (ssid.length() == 0) continue;      // 隐藏网络跳过
    out[cnt++] = ssid;
  }
  WiFi.scanDelete();
  return cnt;
}

static String buildUrl(const String& path) {
  String url = g_config.server;
  if (!path.startsWith("/")) url += "/";
  url += path;
  return url;
}

String net_postJson(const String& path, const String& jsonBody, unsigned long timeoutMs) {
  if (!net_isConnected()) {
    DBG_USB.printf("[http] %s SKIP not connected\n", path.c_str());
    return "";
  }
  // 弱 WiFi（RSSI ~-70dBm）下长请求（服务器 20-40s）可能连接中断：失败自动重试一次
  for (int attempt = 1; attempt <= 2; attempt++) {
    HTTPClient http;
    http.setTimeout(timeoutMs);
    http.begin(buildUrl(path));
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(jsonBody);
    DBG_USB.printf("[http] %s code=%d (try %d)\n", path.c_str(), code, attempt);
    if (code > 0) {
      String resp = http.getString();
      http.end();
      return resp;
    }
    http.end();
    if (attempt == 1) vTaskDelay(pdMS_TO_TICKS(1500));
  }
  return "";
}

String net_postAudio(const String& path, const uint8_t* data, size_t len,
                     const String& filename, unsigned long timeoutMs) {
  if (!net_isConnected()) return "";
  const String boundary = "----ziweiWifiBoundary7Ma4";
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.begin(buildUrl(path));
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  // 构造 multipart body
  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n"
                "Content-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t total = head.length() + len + tail.length();
  // 直接 POST 字节流：用 http.sendRequest 或流式
  WiFiClient* stream = http.getStreamPtr();
  (void)stream;

  // HTTPClient 支持 POST(uint8_t*, size_t) 重载；body 放 PSRAM，避免占用内部 SRAM
  uint8_t* body = (uint8_t*)ps_malloc(total);
  if (!body) { http.end(); return ""; }
  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), data, len);
  memcpy(body + head.length() + len, tail.c_str(), tail.length());

  int code = http.POST(body, total);
  String resp = (code > 0) ? http.getString() : "";
  heap_caps_free(body);   // ps_malloc(PSRAM) 必须用 heap_caps_free
  http.end();
  return (code > 0) ? resp : "";
}

// 裸字节 POST（用于屏幕快照等二进制上传；body 保持原样，服务器直接存）
String net_postRaw(const String& path, const uint8_t* data, size_t len, unsigned long timeoutMs) {
  if (!net_isConnected()) return "";
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.begin(buildUrl(path));
  http.addHeader("Content-Type", "application/octet-stream");
  int code = http.POST(const_cast<uint8_t*>(data), len);
  DBG_USB.printf("[http] %s code=%d\n", path.c_str(), code);
  String resp = (code > 0) ? http.getString() : "";
  http.end();
  return (code > 0) ? resp : "";
}

// GET 下载二进制资源（如 TTS WAV）到 PSRAM；成功返回 true 并填充 buf/len，调用方用 heap_caps_free 释放
// 兼容两种响应：带 Content-Length（按长度读完）与 chunked/无长度（读到流结束或数据空闲），
// 不依赖 http.getSize()——Next.js 对设备请求可能返回 chunked，getSize() 为 -1 导致此前 TTS 下载失败。
bool net_getBinary(const String& path, uint8_t** outBuf, size_t* outLen, unsigned long timeoutMs) {
  if (!net_isConnected()) return false;
  HTTPClient http;
  http.setTimeout(timeoutMs);
  if (!http.begin(buildUrl(path))) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    DBG_USB.printf("[http] GET %s code=%d\n", path.c_str(), code);
    http.end();
    return false;
  }
  int total = http.getSize();   // 可能 -1（chunked 响应）
  WiFiClient* stream = http.getStreamPtr();
  size_t cap = (total > 0) ? (size_t)total : 8192;
  uint8_t* buf = (uint8_t*)ps_malloc(cap);
  if (!buf) { http.end(); return false; }
  size_t got = 0;
  unsigned long t0 = millis();
  int idle = 0;                 // 无新数据的连续次数（chunked 读完判定）
  bool done = false;
  while ((millis() - t0) < timeoutMs && !done) {
    if (stream->available() > 0) {
      int avail = stream->available();
      // 动态扩容（PSRAM 堆）
      if (got + (size_t)avail > cap) {
        size_t ncap = cap;
        while (ncap < got + (size_t)avail) ncap *= 2;
        uint8_t* nb = (uint8_t*)heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM);
        if (!nb) { done = true; break; }
        buf = nb; cap = ncap;
      }
      int r = stream->read(buf + got, (size_t)avail);
      if (r <= 0) { done = true; break; }
      got += (size_t)r;
      idle = 0;
      t0 = millis();
    } else if (!stream->connected()) {
      done = true;              // 服务端关闭连接 = 数据读完
      break;
    } else if (++idle > 200) {  // 连续 ~4s 无数据才视为读完（chunked 结束；1s 窗口过短会误判长音频截断）
      done = true;
      break;
    } else {
      delay(20);
    }
  }
  http.end();
  if (got == 0) {
    heap_caps_free(buf);
    return false;
  }
  // chunked/HTTP 残留防护：Next.js 对二进制响应可能混入前导垃圾（实测 wav 多出 14 字节），
  // 定位数据区真正的 RIFF 头并剥离前导字节，保证 wav 解析/播放正常
  for (int i = 0; i < 64 && i + 4 <= (int)got; i++) {
    if (buf[i] == 'R' && buf[i + 1] == 'I' && buf[i + 2] == 'F' && buf[i + 3] == 'F') {
      if (i > 0) {
        memmove(buf, buf + i, got - (size_t)i);
        got -= (size_t)i;
        DBG_USB.printf("[http] stripped %d leading bytes, now %d\n", i, (int)got);
      }
      break;
    }
  }
  *outBuf = buf;
  *outLen = got;
  DBG_USB.printf("[http] GET %s ok %d bytes\n", path.c_str(), (int)got);
  return true;
}

// ── 业务 API ──

String api_interpret(int year, int month, int day, int hour, const char* gender) {
  String json = String("{\"year\":") + year +
                ",\"month\":" + month +
                ",\"day\":" + day +
                ",\"hour\":" + hour +
                ",\"gender\":\"" + gender + "\"}";
  return net_postJson("/api/ai/interpret", json, 60000);
}

String api_chat(const String& messagesJson, const String& summary) {
  String json = "{\"messages\":" + messagesJson;
  if (summary.length()) {
    // 转义 summary：反斜杠 → 引号 → 真实换行/回车（顺序不能反，否则 JSON 字符串非法）
    String s = summary;
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    s.replace("\r", "");
    s.replace("\n", "\\n");
    json += ",\"summary\":\"" + s + "\"";
  }
  json += "}";
  // 注意：HTTPClient::setTimeout(uint16_t) 上限 65535ms，超过会溢出成短超时导致读超时(-11)
  return net_postJson("/api/ai/chat", json, 60000);
}

String api_asr(const uint8_t* wav, size_t len) {
  // 录音最长 60s（~1.9MB WAV）：上传+方舟转写需要足够时间；65000ms 是 uint16_t 安全上限内的最大值
  return net_postAudio("/api/ai/asr", wav, len, "record.wav", 65000);
}

// 语音排盘：ASR 文本（+ 上轮已收集的 birth JSON，支持多轮补全）→ 服务器解析/追问或排盘 + AI 解读
String api_voiceInterpret(const String& text, const String& partial) {
  String t = text;
  t.replace("\\", "\\\\");
  t.replace("\"", "\\\"");
  String json = "{\"text\":\"" + t + "\"";
  if (partial.length()) json += ",\"partial\":" + partial;
  json += "}";
  return net_postJson("/api/ai/voice-interpret", json, 60000);
}
