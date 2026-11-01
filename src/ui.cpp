#include "ui.h"
#include "app_config.h"
#include "net.h"
#include "audio/recorder.h"
#include "audio/player.h"
#include <Arduino.h>
#include "HWCDC.h"
#include "pin_config.h"
#include <Wire.h>
#include "XPowersLib.h"
#include "ui_hetu_luoshu.h"

extern HWCDC USBSerial;

// 由 app_config.cpp 提供
extern String g_lastSummary;

// ───────────────────────── 全局状态 ─────────────────────────

static lv_obj_t *scrHome, *scrSettings, *scrScan, *scrPowerOff = nullptr;
static lv_obj_t *mainScroll = nullptr;   // 单屏主滚动区（只垂直滚动，无左右滑动）
static lv_obj_t *curScr = nullptr;

// 主题色
#define CLR_BG      lv_color_make(0x14, 0x12, 0x26)   // 深紫黑背景
#define CLR_PANEL   lv_color_make(0x24, 0x20, 0x3e)
#define CLR_GOLD    lv_color_make(0xd4, 0xa9, 0x5f)   // 金色
#define CLR_PURPLE  lv_color_make(0x7b, 0x68, 0xee)   // 紫
#define CLR_TEXT    lv_color_make(0xf0, 0xee, 0xee)
#define CLR_DIM     lv_color_make(0x9a, 0x96, 0xaa)
#define CLR_RED     lv_color_make(0xff, 0x60, 0x60)   // 警示红（低电量状态行）

static lv_style_t stBg, stBtn, stBtnGold, stBtnDanger, stBtnActive, stLabel, stLabelDim, stLabel22;
// 按下反馈样式（独立样式 + LV_STATE_PRESSED selector，按下时背景变亮）
static lv_style_t stBtnP, stBtnGoldP, stBtnDangerP;

// ───────────────────────── 物理按键（板载侧键） ─────────────────────────
// PWR 键（电源键）：硬件接 AXP2101 电源管理芯片，通过 POWER KEY 短按中断读取 → 快捷录音/停止
// BOOT 键（启动键）：直接接 GPIO0，按下为低电平 → 返回主屏（Home）
static XPowersAXP2101 pmu;
static bool pmuOk = false;
static bool bootKeyDown = false;
static unsigned long lastPwrScan = 0;

// ───────────────────────── 排盘输入状态 ─────────────────────────
// 出生信息改为语音输入（服务器解析），不再手动点选
static lv_obj_t *voiceBtn = nullptr;   // 语音排盘按钮

// ───────────────────────── 请求状态机 ─────────────────────────
enum ReqKind { REQ_NONE, REQ_INTERPRET, REQ_CHAT, REQ_ASR, REQ_SCAN };
static String g_reqResult;
static String g_chatHistory = "";   // 对话展示文本
static String g_lastQuestion = "";
static String g_asrText = "";
static bool g_chatHasSummary = false;
static volatile bool g_recStop = false;   // 录音停止标志
static volatile bool g_voiceMode = false; // true=录音用于排盘（说出生信息），false=录音用于问答
static char g_askBuf[128];                // 提问/出生信息缓冲：UI(Core1) 写入，任务(Core0) 读取
static String scanAp[24];                 // 扫描结果 SSID 缓存
static int scanCnt = 0;

// ── UI(Core1) 与常驻 worker(Core0) 之间的请求/结果状态机 ──
static volatile ReqKind g_req = REQ_NONE;      // 请求类型：仅 UI 写入，worker 取走后清 NONE
static volatile bool g_busy = false;           // worker 正在处理某个请求（UI 据此拒绝新请求）
static volatile ReqKind g_busyReq = REQ_NONE;  // worker 正在处理的请求类型
static volatile bool g_reqDone = false;        // 结果就绪
static volatile ReqKind g_lastReqKind = REQ_NONE; // 本次结果对应的请求类型

// 加载指示
static lv_obj_t *lblLoading = nullptr;
static lv_obj_t *chatScroll = nullptr;
static lv_obj_t *chatHint = nullptr;
static uint32_t g_lastSnapMs = 0;   // 屏幕快照周期计时
static lv_obj_t *recBtn = nullptr;
static lv_obj_t *infoLbl = nullptr;   // 主页状态行：网络+电量合并为一个标签（防文字重合）
static lv_obj_t *briLo = nullptr, *briMid = nullptr, *briHi = nullptr;   // 设置页亮度档位键
static unsigned long g_lastActivity = 0;  // 最后一次交互时间（触摸/按键）：空闲自动降亮度依据
static uint8_t g_brightness = 220;        // 当前屏幕亮度（恢复目标 = g_config.bright）

// ── 播报滚动跟随（朗读进度 → 屏幕文字区下移） ──
// 播报由 Core0 worker 线程阻塞逐块执行，滚动只能由 Core1 ui_poll 消费执行：
// worker 在播放每块前写入目标（小节段索引，0=引言段，1..5=对应小节），ui_poll 消费后清 -1；
// 无小节标记的问答文本则请求滚动到对话区最新一条消息。
static volatile int g_scrollTarget = -1;          // 解读场景：目标小节段索引（<0=无请求）
static volatile bool g_scrollToChatTail = false;  // 问答场景：滚动到对话最新一条
static volatile bool g_askShown = false;          // 解读区顶部是否插入了用户问题段（段0）
static lv_obj_t* g_sectionLbl[8] = {nullptr};     // 解读区各小节段的 label（与朗读块段索引对齐）
static int g_sectionY[8];                         // 各小节段在 mainScroll 内容坐标中的目标滚动 Y
static int g_sectionCnt = 0;                      // 解读区实际段数

// ── 播报暂停/继续键（固定于屏幕底部，不随滚动；播报时显示，可随时触摸暂停/继续） ──
static volatile bool g_speaking = false;          // worker 正在朗读（Core0 置位，Core1 ui_poll 消费控制按钮显隐）
static volatile bool g_speakStop = false;         // 播报打断请求（Core1 PWR 键置位，worker 播报循环检测后放弃后续块）
static volatile int  g_speakCnt = 0;              // 播报源嵌套计数（多个播报源并发时，全部结束才清 g_speaking）
static lv_obj_t* pauseBtn = nullptr;

// 多轮补全出生信息：上一轮服务器返回的已收集 birth JSON（原样回传，下轮合并）
static String g_partialBirth = "";

// 恢复录音按钮文字
static void resetRecBtn() {
  if (recBtn) {
    lv_obj_t* lab = lv_obj_get_child(recBtn, 0);
    if (lab) lv_label_set_text(lab, "开始录音");
  }
}

// ───────────────────────── 样式工具 ─────────────────────────

static void ui_styleInit() {
  lv_style_init(&stBg);
  lv_style_set_bg_color(&stBg, CLR_BG);

  lv_style_init(&stBtn);
  lv_style_set_radius(&stBtn, 14);
  lv_style_set_bg_color(&stBtn, CLR_PANEL);
  lv_style_set_bg_opa(&stBtn, LV_OPA_COVER);
  lv_style_set_border_width(&stBtn, 1);
  lv_style_set_border_color(&stBtn, lv_color_make(0x44, 0x40, 0x66));
  lv_style_set_text_color(&stBtn, CLR_TEXT);
  lv_style_set_text_font(&stBtn, &ziwei_font_28);
  lv_style_set_pad_all(&stBtn, 8);

  lv_style_init(&stBtnGold);
  lv_style_set_radius(&stBtnGold, 18);
  lv_style_set_bg_color(&stBtnGold, CLR_GOLD);
  lv_style_set_bg_opa(&stBtnGold, LV_OPA_COVER);
  lv_style_set_text_color(&stBtnGold, lv_color_make(0x20, 0x18, 0x08));
  lv_style_set_text_font(&stBtnGold, &ziwei_font_28);
  lv_style_set_pad_all(&stBtnGold, 10);

  // 危险操作按钮（关机）：红底白字，与普通/金色按钮明显区分
  lv_style_init(&stBtnDanger);
  lv_style_set_radius(&stBtnDanger, 18);
  lv_style_set_bg_color(&stBtnDanger, lv_color_make(0xc0, 0x3a, 0x3a));
  lv_style_set_bg_opa(&stBtnDanger, LV_OPA_COVER);
  lv_style_set_text_color(&stBtnDanger, CLR_TEXT);
  lv_style_set_text_font(&stBtnDanger, &ziwei_font_28);
  lv_style_set_pad_all(&stBtnDanger, 10);

  lv_style_init(&stBtnActive);
  lv_style_set_radius(&stBtnActive, 14);
  lv_style_set_bg_color(&stBtnActive, CLR_PURPLE);
  lv_style_set_bg_opa(&stBtnActive, LV_OPA_COVER);
  lv_style_set_border_width(&stBtnActive, 2);
  lv_style_set_border_color(&stBtnActive, CLR_GOLD);
  lv_style_set_text_color(&stBtnActive, CLR_TEXT);
  lv_style_set_text_font(&stBtnActive, &ziwei_font_28);
  lv_style_set_pad_all(&stBtnActive, 8);

  lv_style_init(&stLabel);
  lv_style_set_text_color(&stLabel, CLR_TEXT);
  lv_style_set_text_font(&stLabel, &ziwei_font_28);

  lv_style_init(&stLabelDim);
  lv_style_set_text_color(&stLabelDim, CLR_DIM);
  lv_style_set_text_font(&stLabelDim, &ziwei_font_28);

  lv_style_init(&stLabel22);
  lv_style_set_text_color(&stLabel22, CLR_TEXT);   // 设置页标签：亮色，与输入框明显区分
  lv_style_set_text_font(&stLabel22, &ziwei_font_22);

  // 按下反馈：背景变亮（LVGL 8.4 用独立样式 + LV_STATE_PRESSED selector，按下自动应用）
  lv_style_init(&stBtnP);
  lv_style_set_bg_color(&stBtnP, lv_color_make(0x3c, 0x36, 0x5e));
  lv_style_set_bg_opa(&stBtnP, LV_OPA_COVER);
  lv_style_init(&stBtnGoldP);
  lv_style_set_bg_color(&stBtnGoldP, lv_color_make(0xec, 0xc4, 0x7c));
  lv_style_set_bg_opa(&stBtnGoldP, LV_OPA_COVER);
  lv_style_init(&stBtnDangerP);
  lv_style_set_bg_color(&stBtnDangerP, lv_color_make(0xe8, 0x56, 0x56));
  lv_style_set_bg_opa(&stBtnDangerP, LV_OPA_COVER);
}

static lv_obj_t* mkLabel(lv_obj_t* parent, const char* text, lv_style_t* st, lv_align_t align) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_add_style(l, st, 0);
  lv_label_set_text(l, text);
  if (align != LV_ALIGN_DEFAULT) lv_obj_align(l, align, 0, 0);
  return l;
}

static lv_obj_t* mkBtn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, lv_style_t* st, int x, int y, int w, int h) {
  lv_obj_t* b = lv_btn_create(parent);
  lv_obj_add_style(b, st, 0);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_t* lab = lv_label_create(b);
  lv_obj_add_style(lab, st, 0);
  lv_label_set_text(lab, text);
  lv_obj_center(lab);
  // 按下反馈：根据按钮底色挂对应 pressed 样式（按下瞬间背景变亮，抬手恢复）
  if (st == &stBtn) lv_obj_add_style(b, &stBtnP, LV_STATE_PRESSED);
  else if (st == &stBtnGold) lv_obj_add_style(b, &stBtnGoldP, LV_STATE_PRESSED);
  else if (st == &stBtnDanger) lv_obj_add_style(b, &stBtnDangerP, LV_STATE_PRESSED);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

// 页面切换（单屏主界面 + 覆盖式设置/扫描/关机确认页）
static void ui_switch(lv_obj_t* scr) {
  if (scrHome) lv_obj_add_flag(scrHome, LV_OBJ_FLAG_HIDDEN);
  if (scrSettings) lv_obj_add_flag(scrSettings, LV_OBJ_FLAG_HIDDEN);
  if (scrScan) lv_obj_add_flag(scrScan, LV_OBJ_FLAG_HIDDEN);
  if (scrPowerOff) lv_obj_add_flag(scrPowerOff, LV_OBJ_FLAG_HIDDEN);
  if (scr) lv_obj_clear_flag(scr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(scr);
  curScr = scr;
}

void ui_showHome() { ui_switch(scrHome); }
void ui_showSettings() { ui_switch(scrSettings); }

// ───────────────────────── 主页（单屏：排盘 + 解读 + 问答 全部固定在一屏） ─────────────────────────

// 前向声明（回调定义在下文）
static void evVoiceInterpret(lv_event_t*);
static void evSettingsToPowerOff(lv_event_t*);   // 主页顶部"关机"键 → 关机确认页
static void refreshDate();
static lv_obj_t *resultBody;
#ifdef AUTO_TEST
static void taskAutoTest(void*);
#endif

static void evHomeToSettings(lv_event_t*) { ui_showSettings(); }

// ── 主页装饰：星点 + 分隔线 + 中央河图洛书（纯视觉，全部不可点击） ──
// hollow=true 画空心圆环（阳/奇数），false 画实心圆点（阴/偶数）
static lv_obj_t* decoDot(lv_obj_t* parent, int x, int y, int s, lv_color_t c, lv_opa_t opa, bool hollow) {
  lv_obj_t* d = lv_obj_create(parent);
  lv_obj_remove_style_all(d);
  lv_obj_set_size(d, s, s);
  lv_obj_set_pos(d, x, y);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  if (hollow) {
    lv_obj_set_style_bg_opa(d, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(d, 2, 0);
    lv_obj_set_style_border_color(d, c, 0);
    lv_obj_set_style_border_opa(d, opa, 0);
  } else {
    lv_obj_set_style_bg_color(d, c, 0);
    lv_obj_set_style_bg_opa(d, opa, 0);
  }
  lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
  return d;
}

// 任意两点连线（参考图点阵之间的连线；金线 LV_OPA_40，先画线后画点以盖住线端）
static void decoLine2(lv_obj_t* parent, int x1, int y1, int x2, int y2) {
  lv_obj_t* l = lv_line_create(parent);
  lv_obj_remove_style_all(l);
  lv_obj_set_style_line_color(l, CLR_GOLD, 0);
  lv_obj_set_style_line_opa(l, LV_OPA_40, 0);
  lv_obj_set_style_line_width(l, 2, 0);
  lv_point_t* pts = (lv_point_t*)lv_mem_alloc(2 * sizeof(lv_point_t));
  if (!pts) { lv_obj_del(l); return; }
  pts[0].x = x1; pts[0].y = y1;
  pts[1].x = x2; pts[1].y = y2;
  lv_line_set_points(l, pts, 2);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
}

static void ui_decoHome() {
  // 顶部：标题两侧对称星点
  decoDot(scrHome, 152, 18, 12, CLR_GOLD, LV_OPA_80, false);
  decoDot(scrHome, 302, 18, 12, CLR_GOLD, LV_OPA_80, false);
  // 标题下分隔线（y=84：按钮区之下、主滚动区之上）
  lv_obj_t* line = lv_obj_create(scrHome);
  lv_obj_remove_style_all(line);
  lv_obj_set_size(line, 172, 2);
  lv_obj_set_pos(line, 147, 84);
  lv_obj_set_style_bg_color(line, CLR_GOLD, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_50, 0);
  lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

  // 中央：标准河图洛书并排位图（左洛书右河图），两图同尺寸 140x185（各自裁去留白）
  // 整组继续左移：洛书左缘 x65（圆屏左边界 57，留 8px 余量），右缘 353 < 圆内边界 409
  // 洛书 pos(65,108)=屏幕(65,200)；河图 pos(213,108)=屏幕(213,200)
  lv_obj_t* imgLs = lv_img_create(mainScroll);
  lv_img_set_src(imgLs, &luoshu_img);
  lv_obj_set_pos(imgLs, 65, 108);
  lv_obj_clear_flag(imgLs, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* imgHt = lv_img_create(mainScroll);
  lv_img_set_src(imgHt, &hetu_img);
  lv_obj_set_pos(imgHt, 213, 108);
  lv_obj_clear_flag(imgHt, LV_OBJ_FLAG_CLICKABLE);
}

// ── 播报暂停/继续键：固定在屏幕底部（不随滚动），朗读时显示，随时可触摸暂停/继续 ──
// 创建于 scrHome 最后 → z-order 最上层，覆盖主滚动区；y400-442 位于圆屏底部圆内
// 按钮文字由 ui_poll 按 pl_paused 状态更新（lvgl 线程内），这里只切换暂停标志
static void evPauseToggle(lv_event_t*) {
  if (pl_paused()) pl_resume(); else pl_pause();
}

static void ui_buildHome() {
  scrHome = lv_obj_create(lv_scr_act());
  lv_obj_add_style(scrHome, &stBg, 0);
  lv_obj_set_size(scrHome, 466, 466);
  lv_obj_clear_flag(scrHome, LV_OBJ_FLAG_SCROLLABLE);   // 根屏禁滚动，杜绝任何方向滑动

  // 顶部固定区：全部在 y<92 内（主滚动区从 92 起，不会被遮挡）——标题居中、WiFi/设置第二行两侧对称
  lv_obj_t* title = mkLabel(scrHome, "紫微斗数", &stLabel, LV_ALIGN_DEFAULT);
  lv_obj_set_style_text_color(title, CLR_GOLD, 0);
  lv_obj_set_style_text_font(title, &ziwei_font_22, 0);
  lv_obj_set_pos(title, 189, 10);   // 22px 4字=88px，中心 233 正中

  // 状态行（网络+电量合并为一个标签，杜绝文字重合）：y36 行圆内弦宽约 x109-357
  // "在线 100%"≈x108-224 | 关机(228-304) | 设置(310-356)，全部落在圆内
  infoLbl = lv_label_create(scrHome);
  lv_obj_add_style(infoLbl, &stLabelDim, 0);
  lv_obj_set_style_text_font(infoLbl, &ziwei_font_22, 0);
  lv_label_set_text(infoLbl, "在线 --");
  lv_obj_set_pos(infoLbl, 108, 36);

  // 关机键（红）：点击进确认页防误触；状态行左、设置按钮右
  lv_obj_t* pwrOffBtn = mkBtn(scrHome, "关机", evSettingsToPowerOff, &stBtnDanger, 228, 36, 76, 40);
  lv_obj_t* pwrLab = lv_obj_get_child(pwrOffBtn, 0);
  lv_obj_set_style_text_font(pwrLab, &ziwei_font_22, 0);   // 按钮内文字 22px，避免溢出按钮

  lv_obj_t* setBtn = mkBtn(scrHome, "设置", evHomeToSettings, &stBtn, 310, 36, 46, 40);
  lv_obj_t* setLab = lv_obj_get_child(setBtn, 0);
  lv_obj_set_style_text_font(setLab, &ziwei_font_22, 0);   // 按钮内文字 22px，避免溢出按钮

  // 主滚动区：只允许垂直滚动（无左右滑动，禁用惯性/弹性）
  mainScroll = lv_obj_create(scrHome);
  lv_obj_set_pos(mainScroll, 0, 92);
  lv_obj_set_size(mainScroll, 466, 374);
  lv_obj_set_style_bg_color(mainScroll, CLR_BG, 0);
  lv_obj_set_style_bg_opa(mainScroll, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(mainScroll, 0, 0);
  lv_obj_set_scroll_dir(mainScroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(mainScroll, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(mainScroll, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);

  // 主页装饰（罗盘环在滚动区内、按钮之下；顶部星点/金线固定）
  ui_decoHome();

  // ── 语音排盘区：说出出生信息即可排盘解读（一屏内，重心进一步左移） ──
  voiceBtn = mkBtn(mainScroll, "语音排盘", nullptr, &stBtnGold, 48, 8, 306, 50);
  lv_obj_add_event_cb(voiceBtn, evVoiceInterpret, LV_EVENT_CLICKED, nullptr);

  // ── 中央状态提示（同一位置，排盘/问答时二选一显示，平时为常驻提问引导）
  // 位于语音排盘按钮(y8-58)与河图洛书位图(y108-293)之间的空当：pos(46,60)，22px 自动换行两行
  // 注意：不能先 align 再 set_pos（重布局时对齐会把坐标覆盖回左上角），必须 DEFAULT+set_pos
  lblLoading = mkLabel(mainScroll, "", &stLabelDim, LV_ALIGN_DEFAULT);
  lv_obj_set_style_text_font(lblLoading, &ziwei_font_22, 0);
  lv_obj_set_pos(lblLoading, 46, 60);
  lv_label_set_long_mode(lblLoading, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lblLoading, 302);
  chatHint = mkLabel(mainScroll, "", &stLabelDim, LV_ALIGN_DEFAULT);
  lv_obj_set_style_text_font(chatHint, &ziwei_font_22, 0);
  lv_obj_set_pos(chatHint, 46, 60);
  lv_label_set_long_mode(chatHint, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(chatHint, 302);
  lv_label_set_text(chatHint, "可直接提问或录音，出生年月日、出生地、时间、性别");

  // ── 解读结果区（解读完成后填充；高度随内容，由主滚动区自动滚动查看） ──
  resultBody = lv_obj_create(mainScroll);
  lv_obj_set_pos(resultBody, 42, 300);
  lv_obj_set_size(resultBody, 322, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(resultBody, CLR_PANEL, 0);
  lv_obj_set_style_bg_opa(resultBody, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(resultBody, 1, 0);
  lv_obj_set_style_border_color(resultBody, lv_color_make(0x44,0x40,0x66), 0);
  lv_obj_set_style_radius(resultBody, 14, 0);
  lv_obj_set_scroll_dir(resultBody, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(resultBody, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(resultBody, LV_OBJ_FLAG_HIDDEN);   // 解读前隐藏

  // ── AI 对话区（高度自适应，内容多了在主滚动区里滚） ──
  chatScroll = lv_obj_create(mainScroll);
  lv_obj_set_pos(chatScroll, 42, 316);
  lv_obj_set_size(chatScroll, 322, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(chatScroll, CLR_PANEL, 0);
  lv_obj_set_style_bg_opa(chatScroll, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(chatScroll, 14, 0);
  lv_obj_set_scroll_dir(chatScroll, LV_DIR_NONE);
  lv_obj_set_scrollbar_mode(chatScroll, LV_SCROLLBAR_MODE_OFF);

  // ── 播报暂停/继续键：固定在屏幕底部（不随滚动），朗读时显示，随时可触摸暂停/继续 ──
  // 创建于 scrHome 最后 → z-order 最上层，覆盖主滚动区；y358-400 在圆屏可视边界（圆底 y409）内
  pauseBtn = mkBtn(scrHome, "暂停", evPauseToggle, &stBtnGold, 188, 358, 90, 42);
  lv_obj_add_flag(pauseBtn, LV_OBJ_FLAG_HIDDEN);   // 播报时才显示

  refreshDate();
}

// ───────────────────────── 排盘输入 ─────────────────────────

static void refreshDate() {
  // 出生信息已改为语音输入，此函数保留为空实现（兼容引用）
}

// ───────────────────────── 语音排盘（说出生信息 → ASR → 服务器解析排盘解读） ─────────────────────────

// 语音排盘任务：ASR 文本 → voice-interpret（并入常驻 taskWorker，见对话区）
static void evVoiceInterpret(lv_event_t*) {
  if (g_busy || g_req != REQ_NONE) {
    USBSerial.printf("[vi] busy, busy=%d req=%d\n", (int)g_busy, (int)g_req);
    if (chatHint) lv_label_set_text(chatHint, "处理中，请稍候…");
    return;
  }
  USBSerial.printf("[vi] start, heap=%d\n", (int)ESP.getFreeHeap());
  g_voiceMode = true;         // 本次录音用于排盘
  g_req = REQ_ASR;
  g_reqDone = false;
  g_reqResult = "";
  g_recStop = false;
  if (voiceBtn) {
    lv_obj_t* lab = lv_obj_get_child(voiceBtn, 0);
    if (lab) lv_label_set_text(lab, "停止识别");
  }
  if (chatHint) lv_label_set_text(chatHint, "说出出生信息即可排盘");
}

// 恢复语音排盘按钮文字
static void resetVoiceBtn() {
  if (voiceBtn) {
    lv_obj_t* lab = lv_obj_get_child(voiceBtn, 0);
    if (lab) lv_label_set_text(lab, "语音排盘");
  }
}

// ───────────────────────── 解读结果（单屏内显示） ─────────────────────────

// 显示解读结果（22px 小字，按小节分段纵向排列，完整展示）。
// 分段与朗读分块的段索引对齐（同一小节标记 bounds 算法），供播报滚动跟随定位。
static void showInterpretResult(const String& text) {
  if (!resultBody) return;
  lv_obj_clear_flag(resultBody, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(resultBody);
  g_sectionCnt = 0;
  g_askShown = false;

  // 按小节标记分段（与 splitSpeakText 的 bounds 算法一致：从 0 起、递增去重）
  static const char* SECT_MARKS[] = {"【性格】", "【事业财运】", "【感情婚姻】", "【健康】", "【大限建议】"};
  const int SECT_N = 5;
  int bounds[8], nb = 0;
  bounds[nb++] = 0;
  for (int m = 0; m < SECT_N && nb < 8; m++) {
    int p = text.indexOf(SECT_MARKS[m]);
    if (p > bounds[nb - 1]) bounds[nb++] = p;
  }

  int y = 10;
  // 用户提问上屏（段0）：排盘时用户说了什么（出生信息），屏幕上同步可见，浅蓝色区分 AI 解读
  if (g_askBuf[0]) {
    lv_obj_t* q = lv_label_create(resultBody);
    lv_obj_add_style(q, &stLabel, 0);
    lv_obj_set_style_text_font(q, &ziwei_font_22, 0);
    lv_obj_set_style_text_color(q, lv_color_make(0x8F, 0xC0, 0xE0), 0);
    lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(q, 302);
    lv_label_set_text(q, (String("我：") + g_askBuf).c_str());
    lv_obj_set_pos(q, 10, y);
    lv_obj_update_layout(q);
    g_sectionY[g_sectionCnt] = 300 + y - 100;
    y += lv_obj_get_height(q) + 8;
    g_sectionLbl[g_sectionCnt++] = q;
    g_askShown = true;
  }
  for (int seg = 0; seg < nb && g_sectionCnt < 8; seg++) {
    int s = bounds[seg], e = (seg + 1 < nb) ? bounds[seg + 1] : (int)text.length();
    if (e <= s) continue;
    String part = text.substring(s, e);
    if (part.length() == 0) continue;
    lv_obj_t* lbl = lv_label_create(resultBody);
    lv_obj_add_style(lbl, &stLabel, 0);
    lv_obj_set_style_text_font(lbl, &ziwei_font_22, 0);   // 覆盖为小字号
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 302);
    lv_label_set_text(lbl, part.c_str());
    lv_obj_set_pos(lbl, 10, y);
    lv_obj_update_layout(lbl);
    // 段顶部在 mainScroll 内容坐标 = resultBody(300) + label(y)；滚动目标=段顶部 - 视口顶(92) - 8 余量
    g_sectionY[g_sectionCnt] = 300 + y - 100;
    y += lv_obj_get_height(lbl) + 8;
    g_sectionLbl[g_sectionCnt++] = lbl;
  }
  // 兜底：无小节标记（如错误信息/单段文本）→ 单 label
  if (g_sectionCnt == 0) {
    lv_obj_t* lbl = lv_label_create(resultBody);
    lv_obj_add_style(lbl, &stLabel, 0);
    lv_obj_set_style_text_font(lbl, &ziwei_font_22, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 302);
    lv_label_set_text(lbl, text.c_str());
    lv_obj_set_pos(lbl, 10, 10);
    lv_obj_update_layout(lbl);
    g_sectionY[g_sectionCnt] = 300 + 10 - 100;
    g_sectionLbl[g_sectionCnt++] = lbl;
  }
  lv_obj_update_layout(resultBody);
  // 对话区移到解读区下方，避免重叠
  if (chatScroll) lv_obj_set_pos(chatScroll, 42, 300 + lv_obj_get_height(resultBody) + 12);
  // 自动滚到解读结果区，用户无需手动滑动
  if (mainScroll) lv_obj_scroll_to_y(mainScroll, 298, LV_ANIM_OFF);
}

// ───────────────────────── 对话区 ─────────────────────────

static int chatY = 10;

// 对话历史（最近多轮，JSON 数组）：每轮 user/assistant 消息追加，AI 下次回答可结合此前问答
static String g_chatHistoryJson = "[]";

// 追加一条对话消息（自动转义）；超长时裁剪只保留最近 12 条，防内存膨胀
static void chatHistoryAppend(const char* role, const String& content) {
  String c = content;
  c.replace("\\", "\\\\");
  c.replace("\"", "\\\"");
  c.replace("\r", "");
  c.replace("\n", "\\n");
  String item = String("{\"role\":\"") + role + "\",\"content\":\"" + c + "\"}";
  if (g_chatHistoryJson.length() <= 2) g_chatHistoryJson = "[" + item + "]";
  else g_chatHistoryJson = g_chatHistoryJson.substring(0, g_chatHistoryJson.length() - 1) + "," + item + "]";
  // 裁剪：统计 "role" 出现次数，超过 12 条则删除最旧一条
  while (true) {
    int cnt = 0, idx = 0;
    while ((idx = g_chatHistoryJson.indexOf("\"role\"", idx)) >= 0) { cnt++; idx += 7; }
    if (cnt <= 12) break;
    int s = g_chatHistoryJson.indexOf('{');
    int e = g_chatHistoryJson.indexOf('}', s);
    if (s < 0 || e < 0) break;
    g_chatHistoryJson.remove(0, e + 1);
    if (g_chatHistoryJson.length() > 0 && g_chatHistoryJson[0] == ',') g_chatHistoryJson.remove(0, 1);
  }
}

// 滚动到对话最新一条：chatScroll 在 mainScroll 内，用显式坐标（scroll_to_view 对嵌套 LV_SIZE_CONTENT 容器不可靠）
static void scrollChatToLast() {
  if (!chatScroll || !mainScroll) return;
  int cnt = lv_obj_get_child_cnt(chatScroll);
  if (cnt <= 0) return;
  lv_obj_t* last = lv_obj_get_child(chatScroll, cnt - 1);
  int absY = lv_obj_get_y(chatScroll) + lv_obj_get_y(last);
  lv_obj_scroll_to_y(mainScroll, absY, LV_ANIM_OFF);
}

static void chatAppend(const String& who, const String& text) {
  if (!chatScroll) return;
  lv_obj_t* lbl = lv_label_create(chatScroll);
  lv_obj_add_style(lbl, &stLabel, 0);
  lv_obj_set_style_text_font(lbl, &ziwei_font_22, 0);   // 小字号，更多对话可见
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, 294);
  String line = who + text;
  lv_label_set_text(lbl, line.c_str());
  lv_obj_set_pos(lbl, 8, chatY);
  lv_obj_update_layout(lbl);
  chatY += lv_obj_get_height(lbl) + 12;
  // 单屏滚动：滚到新消息位置，用户无需手动滑动（显式坐标，可靠）
  scrollChatToLast();
  USBSerial.printf("[ui] chatAppend ok: %s chars=%d children=%d\n", who.c_str(),
                   (int)text.length(), lv_obj_get_child_cnt(chatScroll));
  // 关键事件：AI 回复/提问后立即截屏回传，便于远程查看显示结果
  g_lastSnapMs = millis() - 299000;   // 首次截图立即触发（此后每 5 分钟一次）
}

// ───────────────────────── 常驻网络任务（Core 0，一次分配栈，处理所有请求） ─────────────────────────

// ── 结果声音反馈（提示音 + TTS 朗读）辅助 ──

// URL 编码（UTF-8 字节级 %XX）
static String urlEncode(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
      out += buf;
    }
  }
  return out;
}

// 从 API 响应提取可朗读文本（chat: reply；interpret/asr: text），清洗换行并限长
static String extractSpeakText(const String& resp) {
  int t0 = resp.indexOf("\"reply\":\"");
  int contentStart = 9;   // "reply":" 长度
  if (t0 < 0) {
    t0 = resp.indexOf("\"text\":\"");
    contentStart = 8;     // "text":" 长度
  }
  if (t0 < 0) return String();
  int t1 = t0 + contentStart;
  int end = t1;
  while (end < (int)resp.length()) {
    if (resp[end] == '\\' && end + 1 < (int)resp.length()) { end += 2; continue; }
    if (resp[end] == '"') break;
    end++;
  }
  String s = resp.substring(t1, end);
  s.replace("\\n", " ");
  s.replace("\\\"", "\"");
  s.replace("\n", " ");
  s.trim();
  // 完整播报：voice-interpret 输出长度不稳定（实测 490~1300 字，含五个小节）；
  // 不能再按 490 截断（会把后几个小节砍掉），放宽到 1500，由 speakResult 按小节分块逐段朗读
  if ((int)s.length() > 1500) s = s.substring(0, 1500);
  return s;
}

// 判断第 i 个字节是否位于句末标点/空格处（UTF-8 中文标点的首字节匹配，按字节切不会切坏汉字）
static bool isChunkCut(const String& t, int i) {
  if (i < 0 || i >= (int)t.length()) return false;
  char c = t[i];
  if (c == ' ') return true;
  // 中文句末/停顿标点：。、 (E3 80 82 / E3 80 81) ；！？，；： (EF BC 81/9F/8C/9B/9A)
  unsigned char c1 = (unsigned char)c;
  if (c1 == 0xE3 && i + 2 < (int)t.length()) {
    unsigned char b2 = (unsigned char)t[i + 1], b3 = (unsigned char)t[i + 2];
    if (b2 == 0x80 && (b3 == 0x82 || b3 == 0x81)) return true;
  }
  if (c1 == 0xEF && i + 2 < (int)t.length()) {
    unsigned char b2 = (unsigned char)t[i + 1], b3 = (unsigned char)t[i + 2];
    if (b2 == 0xBC && (b3 == 0x81 || b3 == 0x9F || b3 == 0x8C || b3 == 0x9B || b3 == 0x9A)) return true;
  }
  return false;
}

// 把长文本切成 ≤450 字的朗读块：优先按【小节】标记切（每块含标记，语义完整）；
// 单块超长时再按句末标点/空格二次切分，避免句中截断。返回块数。
// chunkSec（可为 nullptr）：输出每块对应的段索引（0=引言段/无标记，1..5=对应小节），
// 供播报滚动跟随定位屏幕文字区。
static int splitSpeakText(const String& text, String chunks[], int maxChunks, int chunkSec[], int maxSecs) {
  static const char* MARKS[] = {"【性格】", "【事业财运】", "【感情婚姻】", "【健康】", "【大限建议】"};
  const int MARKS_N = 5;
  const int MAXLEN = 450;   // 低于服务器 TTS MAX_TEXT=500，单块 URL 也安全

  // 收集小节标记位置（按出现顺序；仅取递增位置，保证 bounds 单调）
  int bounds[16], nb = 0;
  bounds[nb++] = 0;
  for (int m = 0; m < MARKS_N && nb < 15; m++) {
    int p = text.indexOf(MARKS[m]);
    if (p > bounds[nb - 1]) bounds[nb++] = p;
  }

  int n = 0;
  int seg = 0;
  // 各标记段（含起点前的引言段）
  while (seg + 1 < nb && n < maxChunks) {
    int s = bounds[seg], e = bounds[seg + 1];
    if (e <= s) { seg++; continue; }
    int cs = s;
    while (cs < e && n < maxChunks) {
      int ce = cs + MAXLEN;
      if (ce >= e) { chunks[n] = text.substring(cs, e); if (chunkSec) chunkSec[n] = seg; n++; break; }
      int best = ce;
      for (int i = ce; i > cs; i--) if (isChunkCut(text, i)) { best = i + 1; break; }
      chunks[n] = text.substring(cs, best); if (chunkSec) chunkSec[n] = seg; n++;
      cs = best;
    }
    seg++;
  }
  // 最后一个标记到文本末尾（无标记时即整段，同样按 450+标点切）
  if (seg < nb && n < maxChunks) {
    int s = bounds[seg], e = text.length();
    if (e > s) {
      int cs = s;
      while (cs < e && n < maxChunks) {
        int ce = cs + MAXLEN;
        if (ce >= e) { chunks[n] = text.substring(cs, e); if (chunkSec) chunkSec[n] = seg; n++; break; }
        int best = ce;
        for (int i = ce; i > cs; i--) if (isChunkCut(text, i)) { best = i + 1; break; }
        chunks[n] = text.substring(cs, best); if (chunkSec) chunkSec[n] = seg; n++;
        cs = best;
      }
    }
  }
  return n;
}

// 播放一段 TTS 文本：下载 WAV 并阻塞播放（复用零拷贝播放路径）
static bool speakChunk(const String& chunk, int idx) {
  USBSerial.printf("[tts] chunk#%d chars=%d\n", idx, (int)chunk.length());
  String path = String("/api/ai/tts?text=") + urlEncode(chunk);
  uint8_t* wav = nullptr;
  size_t len = 0;
  if (!net_getBinary(path, &wav, &len, 45000)) {
    USBSerial.println("[tts] chunk fetch FAIL");
    return false;
  }
  USBSerial.printf("[tts] chunk#%d wav=%d bytes\n", idx, (int)len);
  bool p = pl_play_wav(wav, len);
  heap_caps_free(wav);
  USBSerial.printf("[tts] chunk#%d done ret=%d\n", idx, (int)p);
  return p;
}

// 结果出现 → 提示音 + 下载 TTS WAV 并朗读（阻塞播放，运行在 worker 线程，不卡 UI）
// 注意：必须先提取文本再播放提示音——提示音阻塞期间 UI 线程可能已清空 g_reqResult（线程竞态）
// 长文本（voice-interpret 五小节解读）按小节分块逐段播报，保证完整读完全文
static void speakResult(const String& resp) {
  g_speakStop = false;             // 复位打断标志（本次播报从头开始；上次 PWR 打断已被消费）
  String text = extractSpeakText(resp);   // 立即拷贝（此时 resp 尚完整）
  pl_play_beep();                          // 结果提示音（本地生成，无网络依赖）
  if (text.length() < 2) {
    USBSerial.println("[tts] no speakable text");
    return;
  }
  USBSerial.printf("[tts] speak %d chars: %s\n", (int)text.length(),
                   text.length() > 60 ? text.substring(0, 60).c_str() : text.c_str());
  String chunks[12];
  int chunkSec[12];
  int n = splitSpeakText(text, chunks, 12, chunkSec, 12);
  if (n <= 0) return;
  // 是否含小节标记：解读场景按段索引滚动；问答（无标记）滚动到对话最新一条
  bool hasMark = false;
  for (int i = 0; i < n; i++) if (chunkSec[i] > 0) { hasMark = true; break; }
  USBSerial.printf("[tts] chunks=%d hasMark=%d\n", n, (int)hasMark);
  // 嵌套计数防并发踩踏：多个播报源（worker / 自测）同时朗读时，仅当全部结束时才清 g_speaking
  g_speakCnt++;
  g_speaking = true;   // 播报进行中（Core0 置位，Core1 ui_poll 据此显示底部暂停键）
  for (int i = 0; i < n && !g_speakStop; i++) {   // PWR 打断后放弃后续块
    // 播放前通知 UI 滚动到对应文字区（跨核 volatile，ui_poll 消费后清）
    // 解读区顶部插入了用户问题段（段0）时，小节段索引整体 +1 对齐屏幕显示
    if (hasMark) { g_scrollTarget = g_askShown ? chunkSec[i] + 1 : chunkSec[i]; g_scrollToChatTail = false; }
    else         { g_scrollToChatTail = true;  g_scrollTarget = -1; }
    speakChunk(chunks[i], i);
    if (i + 1 < n) vTaskDelay(pdMS_TO_TICKS(200));   // 小节间短暂停顿
  }
  if (--g_speakCnt <= 0) {
    g_speakCnt = 0;
    g_speaking = false;   // 全部播报源结束才隐藏按钮
  }
  USBSerial.println("[tts] playback done");
}

// ── TTS 自动自测（无需用户操作）：开机约 45s（WiFi 已连）后自动播放一次测试语音，串口打印全链路日志 ──
static void taskTtsSelftest(void*) {
  vTaskDelay(pdMS_TO_TICKS(45000));
  for (int i = 0; i < 30 && !net_isConnected(); i++) vTaskDelay(1000);
  USBSerial.printf("[tts] SELFTEST begin wifi=%d heap=%d\n", (int)net_isConnected(), (int)ESP.getFreeHeap());
  speakResult("{\"ok\":true,\"reply\":\"你好，我是你的紫微斗数语音助手\"}");
  USBSerial.println("[tts] SELFTEST done");
  vTaskDelete(nullptr);
}

// 请求参数由 UI(Core1) 写入：g_req 类型 + g_askBuf 文本；结果回填 g_reqResult 并由 loop 侧 ui_poll 更新 UI。
// 常驻任务避免了反复 xTaskCreate/vTaskDelete 的栈内存竞争（内部堆不足会导致创建失败 -1）。
static void taskWorker(void*) {
  for (;;) {
    while (g_req == REQ_NONE) {
      // 人性化：电量低于 20% 且开机已 30s（电量 ADC 稳定）→ 语音提醒一次
      // （worker 内播报，天然避开与 Core1 的播报并发；开机后首 30s 不提醒，避免误报）
      static bool s_lowWarned = false;
      if (pmuOk && !s_lowWarned && millis() > 30000) {
        int bp = pmu.getBatteryPercent();
        if (bp >= 0 && bp < 20) {
          s_lowWarned = true;
          speakResult("{\"ok\":true,\"reply\":\"电量不足，请及时为设备充电\"}");
        }
      }
      vTaskDelay(pdMS_TO_TICKS(30));
    }
    ReqKind k = g_req;
    g_req = REQ_NONE;          // 取走请求（仅 UI 写 g_req，worker 不覆盖）
    g_busy = true;
    g_busyReq = k;

    if (k == REQ_ASR) {
      if (!net_isConnected()) {
        // 人性化：离线时直接返回，不录（避免用户白说一遍后静默失败）
        USBSerial.println("[asr] offline, skip record");
        g_reqResult = "{\"ok\":false,\"error\":\"offline\"}";
        g_recStop = false;
      } else {
        USBSerial.println("[asr] task start");
        pl_play_beep();   // 录音开始提示音（"叮"）：明确告知已开始录音、请说话（播完才开录音，不污染录音）
        uint8_t* wav = nullptr;
        size_t wavLen = 0;
        if (rec_begin() && rec_record(60000, &wav, &wavLen, &g_recStop)) {
          USBSerial.printf("[asr] recorded %d bytes, posting\n", (int)wavLen);
          String resp = api_asr(wav, wavLen);
          USBSerial.printf("[asr] api resp len=%d: %s\n", (int)resp.length(),
                           resp.length() > 120 ? resp.substring(0, 120).c_str() : resp.c_str());
          rec_free(wav);
          g_reqResult = resp;
        } else {
          USBSerial.println("[asr] record FAIL");
          g_reqResult = "{\"ok\":false,\"error\":\"录音失败\"}";
        }
        g_recStop = false;
      }
    } else if (k == REQ_CHAT) {
      String q = g_askBuf;
      chatHistoryAppend("user", q);   // 新问题入历史（含转义），让 AI 结合此前问答
      USBSerial.printf("[chat] start: %s\n", q.length() > 40 ? q.substring(0,40).c_str() : q.c_str());
      String resp = api_chat(g_chatHistoryJson, g_chatHasSummary ? g_lastSummary : "");
      USBSerial.printf("[chat] resp len=%d: %s\n", (int)resp.length(),
                       resp.length() > 100 ? resp.substring(0,100).c_str() : resp.c_str());
      g_reqResult = resp;
    } else if (k == REQ_INTERPRET) {
      String resp = api_voiceInterpret(g_askBuf, g_partialBirth);
      USBSerial.printf("[vi] resp len=%d: %s\n", (int)resp.length(),
                       resp.length() > 100 ? resp.substring(0,100).c_str() : resp.c_str());
      g_reqResult = resp;
    } else if (k == REQ_SCAN) {
      scanCnt = net_scanNetworks(scanAp, 24);
      g_reqResult = "";
    }

    g_lastReqKind = k;         // 结果归属
    g_reqDone = true;          // 完成标志（UI 读后自行清）
    g_busyReq = REQ_NONE;
    g_busy = false;
    USBSerial.println("[worker] done");
    // ── 声音反馈：AI 结果真正出现（排盘解读/对话回答）→ 提示音 + 语音朗读 ──
    // REQ_ASR 只是"识别完成"，随后会紧跟排盘或问答，不在此时提示
    if (k == REQ_INTERPRET || k == REQ_CHAT) {
      speakResult(g_reqResult);
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// 单屏内对话区重置：解读完成后清空旧对话并提示已载入命盘
static void ui_showChatReset() {
  if (chatScroll) lv_obj_clean(chatScroll);
  chatY = 10;
  // 排盘解读后自动带入命盘：显示已载入提示，提问直接基于该盘
  if (g_chatHasSummary && g_lastSummary.length()) {
    lv_obj_t* lbl = lv_label_create(chatScroll);
    lv_obj_add_style(lbl, &stLabelDim, 0);
    lv_obj_set_style_text_font(lbl, &ziwei_font_22, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 294);
    lv_label_set_text(lbl, "（已自动载入你的命盘解读，直接提问即可，无需输入生辰）");
    lv_obj_set_pos(lbl, 8, chatY);
    lv_obj_update_layout(lbl);
    chatY += lv_obj_get_height(lbl) + 12;
  }
  if (chatHint) lv_label_set_text(chatHint, "可直接提问或录音，出生年月日、出生地、时间、性别");
}

// ───────────────────────── 设置页 ─────────────────────────

static lv_obj_t *taSsid, *taPass, *taServer;
static lv_obj_t* kb;          // 九键键盘（lv_btnmatrix 实现，官方键盘同款内核）
static lv_obj_t* kbText;      // 键盘上方输入提示行
static lv_obj_t* kbTa;        // 当前焦点输入框
static int kb9Mode = 0;       // 0=小写字母多击  1=数字  2=大写字母
static int t9Key = -1;        // 当前正在循环的九键（-1 无）
static int t9Idx = 0;
static lv_timer_t* t9Timer = nullptr;

// 九键 1-9 的字符集（索引 0=键1 … 8=键9）：小写字母多击循环，末尾含数字便于输数字；CAP 模式转大写
static const char* kb9sets[9] = {"1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9"};

static void t9Tick(lv_timer_t* t) { t9Key = -1; t9Idx = 0; lv_timer_pause(t); }   // 超时确认并暂停计时

static void kb9Refresh() {   // 更新顶部提示行：模式 + 当前输入内容
  if (kbText && kbTa) {
    const char* m = kb9Mode == 0 ? "abc" : (kb9Mode == 1 ? "123" : "CAP");
    lv_label_set_text_fmt(kbText, "%s | %s", m, lv_textarea_get_text(kbTa));
  }
}

static void evTaFocus(lv_event_t* e) {
  lv_obj_t* ta = (lv_obj_t*)lv_event_get_user_data(e);
  if (!kb) return;
  kbTa = ta;
  lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);   // 光标固定到末尾，删除/插入行为一致
  lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);   // 显式弹出键盘
  lv_obj_move_foreground(kb);                  // 键盘置顶，确保可见可点
  if (kbText) {
    lv_obj_clear_flag(kbText, LV_OBJ_FLAG_HIDDEN);
  }
  // 键盘顶边略高于输入框顶（跟随输入框），底部 ≤360 永不压到"保存并重启"按钮（360 起），杜绝误触重启
  lv_coord_t ky = lv_obj_get_y(ta) - 24;
  if (ky < 97) ky = 97;    // 不超出圆屏顶部
  if (ky > 183) ky = 183;  // 键盘 378×175 底部（ky+175）≤ 358，与保存按钮（360 起）留 2px 间隙
  lv_obj_set_pos(kb, 44, ky);
  if (kbText) lv_obj_set_pos(kbText, 46, ky - 26);   // 提示行贴键盘顶部上方
  kb9Refresh();
  t9Key = -1;
  t9Idx = 0;
}

// 九键按钮点击（btnmatrix VALUE_CHANGED，按索引处理，不解析文本）
static void evKb9(lv_event_t* e) {
  if (!kbTa) return;
  lv_obj_t* bm = lv_event_get_target(e);
  uint16_t idx = lv_btnmatrix_get_selected_btn(bm);
  if (idx == LV_BTNMATRIX_BTN_NONE || idx > 14) return;

  if (idx == 9) {                       // ⌫ 删除（第 4 行第 1 列）
    lv_textarea_del_char(kbTa);
    t9Key = -1; t9Idx = 0;
  } else if (idx == 10) {               // 0 键：数字模式输 0，字母模式输空格
    lv_textarea_add_char(kbTa, kb9Mode == 1 ? '0' : ' ');
    t9Key = -1; t9Idx = 0;
  } else if (idx == 11) {               // √ 完成，收起键盘（第 4 行第 3 列）
    if (kb) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (kbText) lv_obj_add_flag(kbText, LV_OBJ_FLAG_HIDDEN);
    t9Key = -1; t9Idx = 0;
    return;
  } else if (idx == 12) {               // 模式切换：小写字母 → 数字 → 大写 → 循环（模式显示在提示行）
    kb9Mode = (kb9Mode + 1) % 3;
    t9Key = -1; t9Idx = 0;
  } else if (idx == 13) {               // 句点
    lv_textarea_add_char(kbTa, '.');
    t9Key = -1; t9Idx = 0;
  } else if (idx == 14) {               // 下划线
    lv_textarea_add_char(kbTa, '_');
    t9Key = -1; t9Idx = 0;
  } else {                              // 数字键 1-9（idx 0..8）
    int key = (int)idx + 1;
    if (kb9Mode == 1) {                 // 数字模式：直接输数字
      lv_textarea_add_char(kbTa, (char)('0' + key));
      t9Key = -1; t9Idx = 0;
    } else {                            // 字母模式：同键多击循环，换键追加
      const char* set = kb9sets[idx];
      int len = (int)strlen(set);
      char c;
      if (t9Key == key) {               // 同键再击 → 替换最后一个字符
        t9Idx = (t9Idx + 1) % len;
        lv_textarea_del_char(kbTa);
        c = set[t9Idx];
        if (kb9Mode == 2 && c >= 'a' && c <= 'z') c = (char)(c - 32);   // 大写模式
        lv_textarea_add_char(kbTa, c);
      } else {                          // 换键首击 → 追加第一个字符
        t9Key = key; t9Idx = 0;
        c = set[0];
        if (kb9Mode == 2 && c >= 'a' && c <= 'z') c = (char)(c - 32);
        lv_textarea_add_char(kbTa, c);
      }
    }
  }
  kb9Refresh();
  if (t9Timer) { lv_timer_resume(t9Timer); lv_timer_reset(t9Timer); }
}

static void evSave(lv_event_t*) {
  g_config.saveWifi(lv_textarea_get_text(taSsid), lv_textarea_get_text(taPass));
  g_config.saveServer(lv_textarea_get_text(taServer));
  if (chatHint) lv_label_set_text(chatHint, "已保存，重启生效");
  esp_restart();
}

// 亮度档位切换：即时生效 + NVS 保存（重启保留），并高亮当前档
static void evBrightSet(lv_event_t* e) {
  uint8_t v = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  g_config.saveBright(v);
  g_brightness = v;
  ui_setBrightness(v);
  if (briLo) lv_obj_set_style_bg_color(briLo, v == 120 ? CLR_PURPLE : CLR_PANEL, 0);
  if (briMid) lv_obj_set_style_bg_color(briMid, v == 180 ? CLR_PURPLE : CLR_PANEL, 0);
  if (briHi) lv_obj_set_style_bg_color(briHi, v == 240 ? CLR_PURPLE : CLR_PANEL, 0);
  USBSerial.printf("[bright] set=%d\n", v);
}

// ── 关机（真关机：AXP2101 关断全部电源通道；再次开机请长按 PWR 电源键） ──
static void doPowerOff() {
  USBSerial.println("[power] shutdown now");
  delay(300);   // 让串口日志发出去，再断电
  if (pmuOk) {
    pmu.shutdown();   // 关断所有电源通道 → 整机断电；按 PWR 键（约 512ms）重新开机
  } else {
    // PMU 异常时保底：深度睡眠（按 BOOT 键低电平唤醒），避免关不掉
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, LOW);
    esp_deep_sleep_start();
  }
  while (1) delay(1000);   // 理论不可达（断电/深睡后不会返回）
}

static void evPowerOffConfirm(lv_event_t*) { doPowerOff(); }
static void evPowerOffCancel(lv_event_t*) { ui_showSettings(); }
static void evSettingsToPowerOff(lv_event_t*) { ui_switch(scrPowerOff); }

static void ui_buildPowerOff() {
  scrPowerOff = lv_obj_create(lv_scr_act());
  lv_obj_add_style(scrPowerOff, &stBg, 0);
  lv_obj_set_size(scrPowerOff, 466, 466);
  lv_obj_add_flag(scrPowerOff, LV_OBJ_FLAG_HIDDEN);

  // 标题：DEFAULT + set_pos（勿 align，残留对齐标志会在重布局时覆盖回左上角/顶部居中）
  lv_obj_t* ttl = mkLabel(scrPowerOff, "关机", &stLabel, LV_ALIGN_DEFAULT);
  lv_obj_set_pos(ttl, 205, 30);   // 28px 两字宽 56，中心 233 正中

  // 提示两行：定宽 320 + 文字居中，横向以 x=73 中心 233 对齐
  lv_obj_t* tip = mkLabel(scrPowerOff, "确认关机？\n再次开机请长按电源键", &stLabelDim, LV_ALIGN_DEFAULT);
  lv_obj_set_style_text_font(tip, &ziwei_font_22, 0);
  lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(tip, 320);
  lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(tip, 73, 130);

  mkBtn(scrPowerOff, "确认关机", evPowerOffConfirm, &stBtnDanger, 83, 230, 280, 44);
  mkBtn(scrPowerOff, "取消", evPowerOffCancel, &stBtn, 83, 300, 280, 44);
}

static void evSettingsBack(lv_event_t*) { ui_showHome(); }

// ── WiFi 扫描选择 ──
static lv_obj_t *scanList, *scanHint;

static void evScanBack(lv_event_t*) { ui_showSettings(); }

static void evScanPick(lv_event_t* e) {
  const char* ssid = (const char*)lv_event_get_user_data(e);
  if (!ssid) return;
  if (taSsid) lv_textarea_set_text(taSsid, ssid);
  if (taPass) lv_textarea_set_text(taPass, "");   // 清空密码等待输入
  free((void*)ssid);   // 释放扫描时 strdup 的副本
  ui_showSettings();
}

static void evScan(lv_event_t*) {
  if (g_busy || g_req != REQ_NONE) return;
  ui_switch(scrScan);
  if (scanHint) lv_label_set_text(scanHint, "扫描中…");
  g_req = REQ_SCAN;
  g_reqDone = false;
  g_reqResult = "";
  // 由常驻 taskWorker 执行
}

static void ui_buildScan() {
  scrScan = lv_obj_create(lv_scr_act());
  lv_obj_add_style(scrScan, &stBg, 0);
  lv_obj_set_size(scrScan, 466, 466);
  lv_obj_add_flag(scrScan, LV_OBJ_FLAG_HIDDEN);

  mkLabel(scrScan, "选择 WiFi", &stLabel, LV_ALIGN_TOP_MID);
  lv_obj_set_pos(lv_obj_get_child(scrScan, lv_obj_get_child_cnt(scrScan) - 1), 268, 16);
  mkBtn(scrScan, "返回", evScanBack, &stBtn, 150, 18, 44, 36);

  scanHint = mkLabel(scrScan, "", &stLabelDim, LV_ALIGN_TOP_MID);
  lv_obj_set_y(scanHint, 62);

  scanList = lv_obj_create(scrScan);
  lv_obj_set_pos(scanList, 78, 92);
  lv_obj_set_size(scanList, 310, 300);
  lv_obj_set_style_bg_color(scanList, CLR_PANEL, 0);
  lv_obj_set_style_bg_opa(scanList, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(scanList, 14, 0);
  lv_obj_set_scroll_dir(scanList, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scanList, LV_SCROLLBAR_MODE_AUTO);
}

static void ui_buildSettings() {
  scrSettings = lv_obj_create(lv_scr_act());
  lv_obj_add_style(scrSettings, &stBg, 0);
  lv_obj_set_size(scrSettings, 466, 466);

  mkLabel(scrSettings, "设置", &stLabel, LV_ALIGN_TOP_MID);
  lv_obj_set_pos(lv_obj_get_child(scrSettings, lv_obj_get_child_cnt(scrSettings) - 1), 268, 16);
  mkBtn(scrSettings, "返回", evSettingsBack, &stBtn, 170, 18, 44, 36);   // 右移避开左侧"WiFi 名称"标签，确保完整显示

  auto mkTa = [](lv_obj_t* parent, const char* val, int y) -> lv_obj_t* {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, 60, y);
    lv_obj_set_size(ta, 270, 44);   // 宽 270，给右侧"扫描"按钮留出空间
    lv_obj_set_style_radius(ta, 10, 0);
    lv_obj_set_style_bg_color(ta, CLR_PANEL, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 2, 0);   // 亮边框，与上方标签文字明显区分
    lv_obj_set_style_border_color(ta, lv_color_make(0x88, 0x80, 0xcc), 0);
    lv_obj_set_style_border_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, CLR_TEXT, 0);
    lv_obj_set_style_text_font(ta, &ziwei_font_28, 0);
    // 用真实值填充（之前放 placeholder 导致保存后无法修改）
    lv_textarea_set_text(ta, val);
    lv_textarea_set_placeholder_text(ta, val[0] ? "" : "未设置");
    lv_textarea_set_one_line(ta, true);
    // 仅点击触发键盘（CLICKED 时输入框布局已就绪，位置计算最可靠；去掉 FOCUSED 避免时序错位）
    lv_obj_add_event_cb(ta, evTaFocus, LV_EVENT_CLICKED, ta);
    return ta;
  };

  // 设置页标签：亮色 22px + DEFAULT 定位（align 后再 set_pos 会在重布局时被覆盖回左上角，造成标签全部堆叠）
  // 标签下移，紧贴各自输入框正上方（4px 间隙），输入框位置保持不变
  lv_obj_t* lab1 = mkLabel(scrSettings, "WiFi 名称", &stLabel22, LV_ALIGN_DEFAULT);
  lv_obj_set_pos(lab1, 60, 44);
  taSsid = mkTa(scrSettings, g_config.wifi_ssid.c_str(), 70);
  mkBtn(scrSettings, "扫描", evScan, &stBtn, 342, 70, 74, 44);
  lv_obj_t* lab2 = mkLabel(scrSettings, "WiFi 密码", &stLabel22, LV_ALIGN_DEFAULT);
  lv_obj_set_pos(lab2, 60, 142);
  taPass = mkTa(scrSettings, g_config.wifi_pass.c_str(), 168);
  lv_obj_t* lab3 = mkLabel(scrSettings, "服务器地址", &stLabel22, LV_ALIGN_DEFAULT);
  lv_obj_set_pos(lab3, 60, 230);
  taServer = mkTa(scrSettings, g_config.server.c_str(), 256);

  // ── 亮度档位（y312-342 空当，位于服务器输入框下、保存键上）：低/中/高三档，NVS 保存重启保留 ──
  mkLabel(scrSettings, "亮度", &stLabel22, LV_ALIGN_DEFAULT);
  lv_obj_t* briLab = lv_obj_get_child(scrSettings, lv_obj_get_child_cnt(scrSettings) - 1);
  lv_obj_set_pos(briLab, 60, 312);
  briLo = mkBtn(scrSettings, "柔和", nullptr, &stBtn, 132, 312, 52, 30);
  briMid = mkBtn(scrSettings, "标准", nullptr, &stBtn, 188, 312, 52, 30);
  briHi = mkBtn(scrSettings, "明亮", nullptr, &stBtn, 244, 312, 52, 30);
  // 三键独立回调携带档位值；当前档高亮（紫底）
  lv_obj_add_event_cb(briLo, evBrightSet, LV_EVENT_CLICKED, (void*)120);
  lv_obj_add_event_cb(briMid, evBrightSet, LV_EVENT_CLICKED, (void*)180);
  lv_obj_add_event_cb(briHi, evBrightSet, LV_EVENT_CLICKED, (void*)240);
  lv_obj_set_style_bg_color(g_config.bright == 120 ? briLo : (g_config.bright == 180 ? briMid : briHi), CLR_PURPLE, 0);

  // 保存按钮：下移到 y360，与上方亮度键（底 342）拉开 18px 间隙，杜绝"调亮度误触重启"
  mkBtn(scrSettings, "保存并重启", evSave, &stBtnGold, 83, 360, 280, 44);

  // 九键键盘：lv_btnmatrix 实现（官方键盘同款内核，无闪退风险），3 列 × 5 行 = 15 键
  // 尺寸 378×175：键盘底部 ≤358，与"保存并重启"（360-404）零重叠，杜绝误触重启
  static const char* kb9map[] = {
    "1",      "2 ABC",  "3 DEF", "\n",
    "4 GHI",  "5 JKL",  "6 MNO", "\n",
    "7 PQRS", "8 TUV",  "9 WXYZ", "\n",
    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, "\n",
    "ABC/123", ".", "_",
    ""
  };
  kb = lv_btnmatrix_create(scrSettings);
  lv_obj_set_size(kb, 378, 175);
  lv_obj_set_pos(kb, 44, 150);     // 初始位置，实际随输入框在 evTaFocus 中定位
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_btnmatrix_set_map(kb, kb9map);
  lv_obj_set_style_bg_color(kb, lv_color_make(0x12, 0x12, 0x22), 0);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(kb, 0, 0);
  lv_obj_set_style_pad_all(kb, 0, 0);
  lv_obj_set_style_pad_gap(kb, 4, 0);
  lv_obj_set_style_bg_color(kb, lv_color_make(0x28, 0x28, 0x3a), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
  lv_obj_set_style_border_color(kb, lv_color_make(0x45, 0x45, 0x58), LV_PART_ITEMS);
  lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, lv_color_white(), LV_PART_ITEMS);   // 只设颜色不设字体，规避字体渲染空白
  lv_obj_add_event_cb(kb, evKb9, LV_EVENT_VALUE_CHANGED, nullptr);
  // 多击确认定时器：提前创建并暂停（不在点击事件里创建，杜绝点击路径上的动态分配）
  t9Timer = lv_timer_create(t9Tick, 1500, nullptr);
  lv_timer_pause(t9Timer);
  // 顶部输入提示行（独立 label，贴在键盘上方）：键盘覆盖输入框时仍能看到已输入文字
  kbText = lv_label_create(scrSettings);
  lv_label_set_text(kbText, "");
  lv_obj_set_pos(kbText, 46, 130);
  lv_obj_set_style_text_font(kbText, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(kbText, CLR_TEXT, 0);
  lv_obj_add_flag(kbText, LV_OBJ_FLAG_HIDDEN);
}

// ───────────────────────── 结果处理（loop 轮询） ─────────────────────────

// 屏幕快照上传（调试回传）：把真实屏幕内容发到服务器，我直接查看定位显示问题
static void ui_snapUpload() {
  if (!net_isConnected() || g_busy || g_req != REQ_NONE) return;
  lv_img_dsc_t* img = lv_snapshot_take(lv_scr_act(), LV_IMG_CF_TRUE_COLOR);
  if (!img) {
    USBSerial.println("[snap] take FAIL (内存不足?)");
    return;
  }
  uint32_t sz = img->header.w * img->header.h * 2;   // RGB565
  USBSerial.printf("[snap] %dx%d sz=%u heap=%d\n",
                   img->header.w, img->header.h, (unsigned)sz, (int)ESP.getFreeHeap());
  String resp = net_postRaw("/api/screen", (const uint8_t*)img->data, sz, 30000);
  USBSerial.printf("[snap] upload len=%d: %s\n", (int)resp.length(),
                   resp.length() > 80 ? resp.substring(0, 80).c_str() : resp.c_str());
  lv_img_buf_free(img);
}

// 触摸/按键交互通知（main.cpp touchRead / 按键回调调用）：记录活动时间并恢复亮度
void ui_notifyActivity() {
  g_lastActivity = millis();
  if (g_brightness != g_config.bright) { g_brightness = g_config.bright; ui_setBrightness(g_config.bright); }
}

// ── 物理按键轮询 ──
void ui_poll() {
  // BOOT 键（GPIO0）：短按返回主屏（任意页面一键回主页），边沿触发+防抖
  bool bootDown = (digitalRead(0) == LOW);
  if (bootDown && !bootKeyDown) {
    bootKeyDown = true;
    USBSerial.println("[keys] BOOT short -> home");
    g_lastActivity = millis();
    if (g_brightness != g_config.bright) { g_brightness = g_config.bright; ui_setBrightness(g_config.bright); }
    ui_showHome();
  }
  if (!bootDown) bootKeyDown = false;
  // PWR 键（AXP2101 POWER KEY 短按）：快捷录音/停止（等同"开始录音"按钮），100ms 节流轮询
  unsigned long now = millis();
  if (pmuOk && now - lastPwrScan >= 100) {
    lastPwrScan = now;
    uint64_t irq = pmu.getIrqStatus();
    if (pmu.isPekeyShortPressIrq()) {
      g_lastActivity = millis();
      if (g_brightness != g_config.bright) { g_brightness = g_config.bright; ui_setBrightness(g_config.bright); }
      USBSerial.println("[keys] PWR short -> record toggle");
      if (pl_playing()) {
        // 人性化：播报中按 PWR 键 = 打断朗读立即开始录音（有新问题/要补充不必等播完）
        USBSerial.println("[keys] PWR interrupt speech -> record");
        g_speakStop = true;   // worker 播报循环放弃后续块
        pl_stop();            // 立即退出当前播放（解除暂停阻塞，I2S 静音）
      }
      if (g_busy && g_busyReq == REQ_ASR) {
        // 录音中：停止并识别
        g_recStop = true;
      } else if (!g_busy && g_req == REQ_NONE) {
        // 无多轮收集状态 → 问答录音；排盘补全进行中（g_partialBirth 非空）→ 保持排盘模式继续收集
        if (!g_partialBirth.length()) g_voiceMode = false;
        g_req = REQ_ASR;
        g_reqDone = false;
        g_reqResult = "";
        g_recStop = false;
        if (recBtn) {
          lv_obj_t* lab = lv_obj_get_child(recBtn, 0);
          if (lab) lv_label_set_text(lab, "停止识别");
        }
        if (chatHint) lv_label_set_text(chatHint, "录音中…点击停止识别");
      }
    }
    pmu.clearIrqStatus();
  }
  // 每 5 分钟自动截屏回传（空闲时；调试通道，低频率避免持续占服务器/流量）
  // 开机 30s 内每 5s 重试首截（WiFi 未就绪时跳过，下轮再试），此后按 5 分钟周期
  static bool s_snapInit = false;
  static unsigned long s_bootMs = 0;
  if (!s_snapInit) { s_snapInit = true; s_bootMs = millis(); g_lastSnapMs = 0; }
  bool bootTry = (millis() - s_bootMs < 30000) && (millis() - g_lastSnapMs >= 5000);
  if (bootTry || millis() - g_lastSnapMs > 300000) {
    g_lastSnapMs = millis();
    ui_snapUpload();
  }
  // ── 播报滚动跟随：worker(Core0) 播放每块前写入目标，此处消费并执行滚动（LVGL 只能在 Core1 操作） ──
  if (g_scrollToChatTail) {
    g_scrollToChatTail = false;
    scrollChatToLast();
  } else if (g_scrollTarget >= 0) {
    int t = g_scrollTarget;
    g_scrollTarget = -1;
    if (t < g_sectionCnt && g_sectionLbl[t]) {
      USBSerial.printf("[ui] scrollTo section=%d y=%d\n", t, g_sectionY[t]);
      if (mainScroll) lv_obj_scroll_to_y(mainScroll, g_sectionY[t], LV_ANIM_OFF);
    }
  }
  // ── 播报暂停键：朗读中显示（文字随暂停状态切换），播完隐藏；只在状态变化时更新避免每轮重绘 ──
  if (pauseBtn) {
    static bool s_pauseBtnShown = false;
    static bool s_pauseState = false;
    bool sp = g_speaking;
    if (sp != s_pauseBtnShown) {
      s_pauseBtnShown = sp;
      if (sp) lv_obj_clear_flag(pauseBtn, LV_OBJ_FLAG_HIDDEN);
      else    lv_obj_add_flag(pauseBtn, LV_OBJ_FLAG_HIDDEN);
    }
    bool ps = pl_paused();
    if (ps != s_pauseState) {
      s_pauseState = ps;
      lv_obj_t* lab = lv_obj_get_child(pauseBtn, 0);
      if (lab) lv_label_set_text(lab, ps ? "继续" : "暂停");
    }
  }
  // 主页 WiFi 状态 + 电量：每 5s 刷新一次（避免每轮 set_text 触发重绘）
  static unsigned long lastInfo = 0;
  if (now - lastInfo >= 5000) {
    lastInfo = now;
    if (infoLbl) {
      // 状态行 = 网络 + 电量（合并一个标签，防重合）；开机 20s 内电量 ADC 未稳定显示 "--"
      const char* net = net_isConnected() ? "在线" : "离线";
      char buf[24];
      if (!pmuOk || millis() < 20000) {
        snprintf(buf, sizeof(buf), "%s --", net);
      } else {
        int bp = pmu.getBatteryPercent();
        uint16_t bv = pmu.getBattVoltage();
        USBSerial.printf("[bat] percent=%d volt=%d\n", bp, (int)bv);
        snprintf(buf, sizeof(buf), "%s %d%%", net, bp >= 0 ? bp : -1);
        if (bp < 0) snprintf(buf, sizeof(buf), "%s --", net);
        // 低电量警示：状态行文字变红（语音提醒在 worker 空闲时播报）
        lv_obj_set_style_text_color(infoLbl, (bp >= 0 && bp < 20) ? CLR_RED : CLR_DIM, 0);
      }
      lv_label_set_text(infoLbl, buf);
    }
  }
  // 空闲自动降亮度（AMOLED 保护 + 省电）：30s 无操作且不在播报/录音/请求中 → 微亮；触摸/按键恢复全亮
  bool screenBusy = g_speaking || g_busy || g_req != REQ_NONE;
  uint8_t want = (!screenBusy && now - g_lastActivity > 30000) ? 8 : g_config.bright;
  if (want != g_brightness) { g_brightness = want; ui_setBrightness(want); }
  if (!g_reqDone) return;
  g_reqDone = false;
  ReqKind kind = g_lastReqKind;
  g_lastReqKind = REQ_NONE;

  if (kind == REQ_INTERPRET) {
    String resp = g_reqResult;
    bool ok = resp.indexOf("\"ok\":true") >= 0;
    if (ok) {
      // 多轮补全：complete=false → 信息不全，播报追问话术并等待用户补充（不排盘）
      if (resp.indexOf("\"complete\":false") >= 0) {
        String ask = "";
        int a0 = resp.indexOf("\"ask\":\"");
        if (a0 >= 0) {
          int a1 = a0 + 7;
          int end = a1;
          while (end < (int)resp.length()) {
            if (resp[end] == '\\' && end + 1 < (int)resp.length()) { end += 2; continue; }
            if (resp[end] == '"') break;
            end++;
          }
          ask = resp.substring(a1, end);
          ask.replace("\\n", " ");
          ask.replace("\\\"", "\"");
        }
        // 已收集出生信息 birth JSON（原样保存，下一轮随文本上传继续合并）
        String birthJson = "";
        int b0 = resp.indexOf("\"birth\":");
        if (b0 >= 0) {
          int b1 = b0 + 8;
          int depth = 0; int end = b1;
          for (; end < (int)resp.length(); end++) {
            if (resp[end] == '{') depth++;
            else if (resp[end] == '}') { depth--; if (depth <= 0) { end++; break; } }
          }
          birthJson = resp.substring(b1, end);
        }
        g_partialBirth = birthJson;
        USBSerial.printf("[vi] need-more ask=%s partial=%s\n",
                         ask.length() > 60 ? ask.substring(0,60).c_str() : ask.c_str(),
                         birthJson.c_str());
        if (ask.length()) {
          chatAppend("助手: ", ask);
          String a = ask;
          a.replace("\\", "\\\\");
          a.replace("\"", "\\\"");
          speakResult("{\"ok\":true,\"text\":\"" + a + "\"}");
        }
        if (chatHint) lv_label_set_text(chatHint, "请继续说出缺失信息");
        if (lblLoading) lv_label_set_text(lblLoading, "");
        g_voiceMode = true;   // 保持排盘收集模式：下一轮录音仍走 REQ_INTERPRET（带 partial）
      } else {
        // 信息齐全：排盘解读完成
        g_partialBirth = "";   // 收集完成，清空多轮状态
        g_chatHistoryJson = "[]";  // 新命盘：清空旧对话历史，避免跨盘上下文串扰
        // 提取 summary 和 text
        int s0 = resp.indexOf("\"summary\":\"");
        String summary = "";
        if (s0 >= 0) {
          int s1 = s0 + 11;
          int s2 = resp.indexOf("\"text\"", s1);
          if (s2 > s1) summary = resp.substring(s1, s2);
          summary.replace("\\n", "\n");
          summary.replace("\\\"", "\"");
        }
        int t0 = resp.indexOf("\"text\":\"");
        String text = "";
        if (t0 >= 0) {
          int t1 = t0 + 8;
          // 直到未转义的引号
          int end = t1;
          while (end < (int)resp.length()) {
            if (resp[end] == '\\' && end + 1 < (int)resp.length()) { end += 2; continue; }
            if (resp[end] == '"') break;
            end++;
          }
          text = resp.substring(t1, end);
          text.replace("\\n", "\n");
          text.replace("\\\"", "\"");
        }
        if (summary.length()) g_lastSummary = summary;
        g_chatHasSummary = true;
        showInterpretResult(text.length() ? text : "（AI 返回为空，请检查方舟模型配置）");
        // 解读完成：重置对话区并提示命盘已载入（单屏内直接继续提问）
        ui_showChatReset();
      }
    } else {
      int e0 = resp.indexOf("\"error\":\"");
      String err = e0 >= 0 ? resp.substring(e0 + 9, resp.indexOf("\"}", e0)) : resp;
      String msg = "解读失败：" + err;
      showInterpretResult(msg.c_str());
    }
    if (lblLoading) lv_label_set_text(lblLoading, "");
  } else if (kind == REQ_CHAT) {
    USBSerial.printf("[ui] chat resp %d\n", (int)g_reqResult.length());
    String resp = g_reqResult;
    if (resp.indexOf("\"ok\":true") >= 0) {
      int t0 = resp.indexOf("\"reply\":\"");
      String reply = "";
      if (t0 >= 0) {
        int t1 = t0 + 9;   // "reply":" 是 9 个字符（含起始引号），t0+9 才指向内容
        int end = t1;
        while (end < (int)resp.length()) {
          if (resp[end] == '\\' && end + 1 < (int)resp.length()) { end += 2; continue; }
          if (resp[end] == '"') break;
          end++;
        }
        reply = resp.substring(t1, end);
        reply.replace("\\n", "\n");
        reply.replace("\\\"", "\"");
      }
      chatAppend("AI: ", reply.length() ? reply : "（无回复）");
      chatHistoryAppend("assistant", reply.length() ? reply : "（无回复）");   // 回答入历史，下轮可结合
    } else {
      int e0 = resp.indexOf("\"error\":\"");
      String err = e0 >= 0 ? resp.substring(e0 + 9, resp.indexOf("\"}", e0)) : resp;
      String msg = "出错：" + err;
      chatAppend("AI: ", msg);
    }
    if (chatHint) lv_label_set_text(chatHint, "可直接提问或录音，出生年月日、出生地、时间、性别");
    // AI 回答完成：恢复录音按钮文字
    resetRecBtn();
  } else if (kind == REQ_ASR) {
    USBSerial.printf("[ui] asr resp %d\n", (int)g_reqResult.length());
    String resp = g_reqResult;
    if (resp.indexOf("\"ok\":true") >= 0) {
      int t0 = resp.indexOf("\"text\":\"");
      String txt = "";
      if (t0 >= 0) {
        int t1 = t0 + 8;
        int end = t1;
        while (end < (int)resp.length()) {
          if (resp[end] == '\\' && end + 1 < (int)resp.length()) { end += 2; continue; }
          if (resp[end] == '"') break;
          end++;
        }
        txt = resp.substring(t1, end);
        txt.replace("\\n", "\n");
      }
      // 过滤无效识别文本：至少含 2 个汉字才算有效语音
      int cn = 0;
      for (size_t i = 0; i < txt.length(); i++) {
        if ((uint8_t)txt[i] >= 0xE0) { cn++; i += 2; }
      }
      if (txt.length() && cn >= 2) {
        strncpy(g_askBuf, txt.c_str(), sizeof(g_askBuf) - 1);
        g_askBuf[sizeof(g_askBuf) - 1] = 0;
        if (g_voiceMode) {
          // 排盘模式：出生信息 → 服务器解析/追问 + 排盘 + AI 解读（多轮补全时每轮上屏）
          g_voiceMode = false;
          chatAppend("我: ", txt);   // 本轮说的内容上屏，用户可见多轮收集进度
          g_req = REQ_INTERPRET;
          g_reqDone = false;
          g_reqResult = "";
          if (lblLoading) lv_label_set_text(lblLoading, "正在排盘解读…");
          if (chatHint) lv_label_set_text(chatHint, "AI 排盘解读中…");
        } else {
          // 问答模式：识别文本直接作为问题
          g_lastQuestion = txt;
          chatAppend("我: ", txt);
          g_req = REQ_CHAT;
          g_reqDone = false;
          g_reqResult = "";
          if (chatHint) lv_label_set_text(chatHint, "AI 思考中…");
        }
      } else {
        // 人性化：区分三类失败（离线 / 服务异常 / 识别无效），避免一律播"没听清"误导用户重复说话
        if (resp.indexOf("\"offline\"") >= 0) {
          if (chatHint) lv_label_set_text(chatHint, "网络未连接，请检查设置");
          speakResult("{\"ok\":true,\"reply\":\"网络未连接，请检查设置后再试\"}");
        } else if (resp.length() == 0 || resp.indexOf("\"ok\":false") >= 0) {
          if (chatHint) lv_label_set_text(chatHint, "服务异常，请稍后再试");
          speakResult("{\"ok\":true,\"reply\":\"抱歉，网络或服务出了点问题，请稍后再试\"}");
        } else {
          if (chatHint) lv_label_set_text(chatHint, "未识别，请再说一次");
          speakResult("{\"ok\":true,\"reply\":\"抱歉，我没有听清，请再说一次好吗？\"}");
        }
      }
    } else {
      int e0 = resp.indexOf("\"error\":\"");
      String err = e0 >= 0 ? resp.substring(e0 + 9, resp.indexOf("\"}", e0)) : resp;
      String msg = "语音识别失败：" + err;
      if (chatHint) lv_label_set_text(chatHint, msg.c_str());
    }
    // 恢复录音按钮文字
    resetRecBtn();
    resetVoiceBtn();
  } else if (kind == REQ_SCAN) {
    // 扫描完成：填充 WiFi 列表
    if (scanHint) lv_label_set_text(scanHint, scanCnt ? "找到附近 WiFi，点击选择：" : "未扫描到 WiFi，请检查网络");
    lv_obj_clean(scanList);
    for (int i = 0; i < scanCnt; i++) {
      lv_obj_t* b = lv_btn_create(scanList);
      lv_obj_add_style(b, &stBtn, 0);
      lv_obj_set_width(b, 298);
      lv_obj_set_height(b, 46);
      lv_obj_set_pos(b, 6, 6 + i * 54);
      lv_obj_t* lab = lv_label_create(b);
      lv_obj_add_style(lab, &stBtn, 0);
      lv_label_set_text(lab, scanAp[i].c_str());
      lv_obj_center(lab);
      lv_obj_add_event_cb(b, evScanPick, LV_EVENT_CLICKED, (void*)strdup(scanAp[i].c_str()));
    }
  }
}

// ───────────────────────── 初始化 ─────────────────────────

void ui_init() {
  ui_styleInit();
  lv_obj_clean(lv_scr_act());
  lv_obj_add_style(lv_scr_act(), &stBg, 0);

  ui_buildHome();
  ui_buildSettings();
  ui_buildScan();
  ui_buildPowerOff();

  // 物理按键初始化：BOOT（GPIO0）直接读取；PWR 走 AXP2101 POWER KEY 短按中断
  pinMode(0, INPUT_PULLUP);   // BOOT 键：按下=低电平
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (pmuOk) {
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);   // 只监听 PWR 键短按
    pmu.disableLongPressShutdown();                  // 长按 PWR 不关机，避免误触断电
    pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_512MS); // 关机后按 PWR 键 512ms 重新开机
    USBSerial.println("[keys] PWR+BOOT ready");
  } else {
    USBSerial.println("[keys] PMU not online");
  }

  // 默认显示主页
  ui_showHome();

  // 常驻网络任务（Core 0）：录音/ASR/chat/语音排盘/扫描 全部复用一个任务，避免反复创建任务导致内部堆耗尽。
  // 栈必须放内部 SRAM（lwIP 断言禁止 TCP 任务栈位于 PSRAM）；内部 RAM 空间由 LVGL 1/16 屏缓冲腾出。
  BaseType_t r = xTaskCreatePinnedToCore(taskWorker, "worker", 16384, nullptr, 1, nullptr, 0);
  USBSerial.printf("[ui] worker task=%d heap=%d\n", (int)r, (int)ESP.getFreeHeap());

  // TTS 自动自测任务不再创建：AUTO_TEST [1b] 已覆盖完整播报链路，
  // 独立 SELFTEST 会在播报时与真实朗读并发抢 g_speaking/播放器，导致按钮状态被覆盖。
  // （taskTtsSelftest 定义保留作参考，不创建实例）

#ifdef AUTO_TEST
  // ── 自动全链路自测（无需用户操作）：开机后 Core 0 依次执行，串口打印每步结果 ──
  xTaskCreatePinnedToCore(taskAutoTest, "autotest", 16384, nullptr, 1, nullptr, 0);
#endif
}

// ───────────────────────── 自动全链路自测（AUTO_TEST 宏启用） ─────────────────────────
// 重点：模拟"排盘成功 → 点财运"的完整 UI 状态机路径（g_req → worker → g_reqResult → ui_poll → chatAppend）
#ifdef AUTO_TEST
static void taskAutoTest(void*) {
  vTaskDelay(pdMS_TO_TICKS(2000));
  USBSerial.println("=== AUTO TEST BEGIN ===");
  for (int attempt = 0; attempt < 20 && !net_isConnected(); attempt++) vTaskDelay(1000);
  USBSerial.printf("[auto] wifi connected=%d heap=%d\n", (int)net_isConnected(), (int)ESP.getFreeHeap());

  // [1] 语音排盘（服务器端解析中文出生文本），提取 summary 注入 g_lastSummary 模拟排盘成功
  String vi = api_voiceInterpret("一九九零年五月十五日寅时男");
  USBSerial.printf("[auto][1] voice-interpret len=%d\n", (int)vi.length());
  String summary = "";
  if (vi.indexOf("\"ok\":true") >= 0) {
    int s0 = vi.indexOf("\"summary\":\"");
    if (s0 >= 0) {
      int s1 = s0 + 11;
      int s2 = vi.indexOf("\"text\"", s1);
      if (s2 > s1) summary = vi.substring(s1, s2);
      summary.replace("\\n", "\n");
      summary.replace("\\\"", "\"");
    }
  }
  g_lastSummary = summary;      // 与 ui_poll REQ_INTERPRET 分支相同
  g_chatHasSummary = summary.length() > 0;
  USBSerial.printf("[auto][1] summaryLen=%d has=%d\n", (int)summary.length(), (int)g_chatHasSummary);
  vTaskDelay(pdMS_TO_TICKS(500));

  // [1b] 完整五小节语音播报自测：
  // 先按真实 UI 路径把解读写入结果状态机（Core1 消费 → showInterpretResult 按小节分段渲染），
  // 再分块播报，验证"播到哪段屏幕滚到哪段"的滚动跟随。
  USBSerial.println("[auto][1b] show interpretation + speak full...");
  strncpy(g_askBuf, "一九九零年五月十五日寅时男", sizeof(g_askBuf) - 1);   // 模拟 ASR 识别结果（用户提问上屏段0）
  g_askBuf[sizeof(g_askBuf) - 1] = 0;
  g_reqResult = vi;
  g_lastReqKind = REQ_INTERPRET;
  g_reqDone = true;
  vTaskDelay(pdMS_TO_TICKS(1500));   // 等 Core1 ui_poll 消费并完成分段显示
  speakResult(vi);
  USBSerial.println("[auto][1b] speak done");
  vTaskDelay(pdMS_TO_TICKS(300));

  // [2] 模拟点"财运"：与 evQuick 完全相同的状态机写入（Core0 写，UI 主循环在 Core1 消费）
  strncpy(g_askBuf, "财运", sizeof(g_askBuf) - 1);
  g_askBuf[sizeof(g_askBuf) - 1] = 0;
  g_lastQuestion = "财运";
  g_req = REQ_CHAT;
  g_reqDone = false;
  g_reqResult = "";
  USBSerial.println("[auto][2] simulated tap 财运 -> waiting for worker+UI...");
  // 等待 worker 处理完且 UI 已消费（轮询 g_reqResult 非空 / g_reqDone 翻转 / 90s 上限）
  unsigned long t0 = millis();
  while (millis() - t0 < 90000) {
    if (!g_busy && g_req == REQ_NONE && g_reqDone == false && g_reqResult.length() > 0) break;
    vTaskDelay(500);
  }
  USBSerial.printf("[auto][2] after wait: reqResultLen=%d busy=%d req=%d done=%d\n",
                   (int)g_reqResult.length(), (int)g_busy, (int)g_req, (int)g_reqDone);
  vTaskDelay(pdMS_TO_TICKS(2500)); // 留时间给 Core1 ui_poll 执行 chatAppend
  USBSerial.printf("[auto][2] final: reqResultLen=%d heap=%d\n",
                   (int)g_reqResult.length(), (int)ESP.getFreeHeap());
  vTaskDelay(pdMS_TO_TICKS(300));

  // [3] 清空命盘后模拟"事业"（无 summary 路径，走相同状态机）
  g_chatHasSummary = false;
  g_lastSummary = "";
  strncpy(g_askBuf, "事业", sizeof(g_askBuf) - 1);
  g_askBuf[sizeof(g_askBuf) - 1] = 0;
  g_req = REQ_CHAT;
  g_reqDone = false;
  g_reqResult = "";
  t0 = millis();
  while (millis() - t0 < 60000) {
    if (!g_busy && g_req == REQ_NONE && g_reqDone == false && g_reqResult.length() > 0) break;
    vTaskDelay(500);
  }
  USBSerial.printf("[auto][3] nosummary: reqResultLen=%d heap=%d\n", (int)g_reqResult.length(), (int)ESP.getFreeHeap());
  vTaskDelay(pdMS_TO_TICKS(2500));

  // 4. 真实录音 → ASR（麦克风硬件验证；无语音输入时 ASR 返回空属正常）
  uint8_t* wav = nullptr;
  size_t wavLen = 0;
  USBSerial.println("[auto][4] recording 3s (麦克风环境测试)…");
  if (rec_begin() && rec_record(3000, &wav, &wavLen, nullptr)) {
    USBSerial.printf("[auto][4] recorded %d bytes\n", (int)wavLen);
    String asr = api_asr(wav, wavLen);
    USBSerial.printf("[auto][4] asr len=%d: %s\n", (int)asr.length(),
                     asr.length() > 100 ? asr.substring(0, 100).c_str() : asr.c_str());
    rec_free(wav);
  } else {
    USBSerial.println("[auto][4] record FAIL");
  }

  USBSerial.printf("[auto] heap=%d psram=%d\n",
                   (int)ESP.getFreeHeap(),
                   (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  USBSerial.println("=== AUTO TEST DONE ===");
  vTaskDelete(nullptr);
}
#endif
