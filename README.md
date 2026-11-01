# Ziwei Watch · ESP32-S3 紫微斗数 AI 圆屏

基于 Waveshare **ESP32-S3-Touch-AMOLED-1.75C** 开发板的紫微斗数 AI 语音圆屏设备：语音排盘、AI 问答、紫微星盘与河图洛书显示一体。

![硬件](https://img.shields.io/badge/hardware-Waveshare%20ESP32--S3--Touch--AMOLED--1.75C-blue)
![Framework](https://img.shields.io/badge/framework-Arduino%20%2B%20LVGL%208.4-orange)
![License](https://img.shields.io/badge/license-MIT-green)

配套服务端：[1legend05/ziwei-doushu](https://github.com/1legend05/ziwei-doushu)（Next.js AI 排盘服务，方舟大模型 + 语音识别 + 语音合成）。

## 功能特性

- **语音排盘**：按住说话 → 服务器语音识别 → 紫微斗数排盘 → 屏幕显示星盘 + 语音播报解读
- **语音多轮补全**：出生信息不全时自动追问（年/月/日/时/性别逐项补全），支持对话上下文
- **AI 问答**：结合排盘结果与对话历史，回答命理、财运、事业等问题
- **河图洛书主页**：圆屏专属布局，量理象数界面
- **电量显示**：AXP2101 电源管理，5 秒节流刷新，低电量红色警示 + 语音提醒
- **30 秒空闲自动降亮度**，触摸/按键即恢复；设置页三档亮度（低/中/高 120/180/240）
- **播报打断**：朗读中按电源键立即打断，直接进入录音
- **录音提示音**：录音开始/结束提示音，未识别语音即时反馈
- **离线兜底**：断网时跳过录音直接返回离线排盘提示，ASR 失败三分支（离线/服务异常/未识别）各有屏幕 + 语音文案

## 硬件

| 项目 | 规格 |
|---|---|
| 主控 | ESP32-S3R8（8MB OPI PSRAM + 32MB Flash） |
| 屏幕 | 1.75 英寸 466×466 AMOLED（ST7701S 驱动） |
| 触摸 | CST9217 电容触摸 |
| 音频 | ES7210 双麦克风 + ES8311 功放 |
| 电源 | AXP2101 PMIC |

接线与板载外设见 [include/pin_config.h](include/pin_config.h)。

## 快速开始

### 1. 安装依赖

```bash
# PlatformIO 会自动从 Registry 拉取以下库（无需手动安装）：
#   lvgl 8.4.0 / XPowersLib 0.2.6 / GFX Library for Arduino 1.6.4 / SensorLib 0.3.3
# Network 库（ESP32 官方框架库）已随仓库提供在 lib/Network
```

### 2. 生成中文字体（首次必做）

固件依赖两个 LVGL 中文字体（`src/fonts/ziwei_font_28.c`、`ziwei_font_22.c`），为仓库瘦身未提交生成物：

```bash
cd scripts
# 1) 准备字体源文件（任选其一）：
#    - 从 Google Fonts 下载 Noto Sans SC 的 OTF 版，重命名为 NotoSansSC-Regular.otf
#    - 或安装 npm 依赖后运行生成脚本
npm install lv_font_conv          # 字体转换工具
python make_chars.py              # 生成字符表 chars.txt
node make_font.js                 # 生成 28px 字体 → src/fonts/ziwei_font_28.c
node make_font_22.js              # 生成 22px 字体 → src/fonts/ziwei_font_22.c
```

> 也可直接使用仓库 Release 附带的预生成字体文件。

### 3. 配置 WiFi 与服务器地址

编辑 [src/app_config.h](src/app_config.h) 的默认值（首次烧录生效；烧录后可在设备"设置"页修改并保存到 NVS，重启保留）：

```c
#define DEFAULT_WIFI_SSID   "YOUR_WIFI_SSID"        // 你的 WiFi 名
#define DEFAULT_WIFI_PASS   "YOUR_WIFI_PASSWORD"    // WiFi 密码
#define DEFAULT_SERVER_URL  "http://192.168.1.100:3000"  // 服务器根地址（完整 URL）
```

- 服务器地址必须是完整 URL（含 `http://` 或 `https://` 与端口）
- 公网部署填 `https://your-app.vercel.app`；局域网服务器填 `http://<服务器IP>:3000`
- 请勿提交真实 WiFi 凭据；烧录后建议在设备"设置"页改掉占位默认值（NVS 保存，优先于此处默认）

### 4. 编译烧录

```bash
pio run -t upload        # 需先连接开发板（USB）
pio device monitor      # 串口监视（115200 baud）
```

首次编译约 5~10 分钟（自动下载依赖库）。

## 服务器部署

见配套仓库 [1legend05/ziwei-doushu](https://github.com/1legend05/ziwei-doushu)：

```bash
git clone https://github.com/1legend05/ziwei-doushu
cd ziwei-doushu
cp .env.example .env.local   # 填入火山方舟 API Key 等
npm install && npm run dev   # 或 npm run build && npm start
```

## 目录结构

```
ziwei-watch/
├── src/
│   ├── main.cpp              # 入口
│   ├── ui.cpp / ui.h         # LVGL 界面逻辑
│   ├── ui_hetu_luoshu.h      # 河图洛书位图数据
│   ├── net.cpp / net.h       # HTTP 客户端（排盘/语音/截图上传）
│   ├── audio/                # ES7210 双麦录音 + ES8311 播报
│   └── fonts/                # LVGL 中文字体（生成）
├── include/
│   ├── lv_conf.h             # LVGL 配置
│   └── pin_config.h          # 引脚定义
├── lib/Network/              # ESP32 官方 Network 框架库（registry 无此库，随仓提供）
├── scripts/                  # 字体生成工具
├── partitions_32mb.csv       # 32MB Flash 分区表
└── platformio.ini            # PlatformIO 工程配置
```

## 常见问题

**编译报 `lv_conf.h` 找不到？** 检查 platformio.ini 的 build_flags 是否含 `-DLV_CONF_INCLUDE_SIMPLE -I include`。

**文字显示为方块？** 中文字体未生成或生成参数不对（LVGL 8.4 必须 `no_compress: true`），重新按上文生成。

**无法连接服务器？** 区分两种情况：
- 状态行显示 **"离线"**：WiFi 没连上，检查设置页 WiFi 名/密码
- 显示 **"在线"** 但排盘提示 **"服务异常，请稍后再试"**：WiFi 通了但服务器不可达——检查"服务器地址"是否填对（公网域名或 `http://<IP>:3000`）、服务器是否已启动；若公网域名在你网络直连受限，换网络（如手机热点）测试

## License

[MIT](LICENSE)
