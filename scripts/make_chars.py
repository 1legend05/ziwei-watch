# -*- coding: utf-8 -*-
"""生成 LVGL 中文字体所需字符集：ASCII + GB2312 一级汉字 + 常用中文标点"""
import io
import os

chars = set()

# ASCII 可打印字符
for c in range(0x20, 0x7F):
    chars.add(chr(c))

# GB2312 一级汉字区：0xB0A1 - 0xD7F9（3755 字）
# 行 = (高字节-0xB0)*94 + (低字节-0xA1)
def gb2312_dec(h, l):
    b = bytes([h, l])
    return b.decode('gb2312')

for h in range(0xB0, 0xD8):
    for l in range(0xA1, 0xFF):
        try:
            ch = gb2312_dec(h, l)
            if ch != '\ufffd':
                chars.add(ch)
        except Exception:
            pass

# 常用中文标点与符号
punct = "，。！？、；：“”‘’（）《》【】〈〉—…～·％＄＃＠＆＊＋－＝／＼｜×÷℃㎡①23紫微斗数命盘宫位天干地支五行阴阳甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥禄权科忌庙旺得利平不陷北斗南斗紫府相梁杀破狼日月同梁机阴贪廉武府相杀破廉贞贪狼紫微天机太阳武曲天同天相天梁七杀天府太阴巨门天魁天钺左辅右弼文昌文曲禄存擎羊陀罗火星铃星地空劫杀天马红鸾天喜天姚天刑天哭解神截空旬空命主身主大限流年流月流日流时本命命宫兄弟夫妻子女财帛疾厄迁移仆役官禄田宅福德父母生年四柱格局独坐空宫借星安宫真太阳时夏令时闰月晚子时早子时男命女命阳男阳女阴男阴女运限童限长生沐浴冠带临官帝旺衰病死墓绝胎养金四局木三局水二局火六局土五局"
for ch in punct:
    chars.add(ch)

# 输出字符集文件（每行一个字符；空行会被 lv_font_conv 忽略）
out = io.StringIO()
for ch in sorted(chars):
    out.write(ch + '\n')
data = out.getvalue()

with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'chars.txt'), 'w', encoding='utf-8') as f:
    f.write(data)

print('total chars:', len(chars))
