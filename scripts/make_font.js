// 生成 LVGL 中文字体（绕过 CLI 命令行长度限制，直接调用 convert API）
// 用法: node make_font.js
'use strict';
const fs = require('fs');
const path = require('path');
const convert = require(path.join(__dirname, 'node_modules', 'lv_font_conv', 'lib', 'convert'));

const symbols = fs.readFileSync(path.join(__dirname, 'chars.txt'), 'utf8').replace(/[\r\n]/g, '');
const fontPath = path.join(__dirname, 'NotoSansSC-Regular.otf');

const args = {
  size: 28,
  bpp: 4,
  format: 'lvgl',
  font: [
    {
      source_path: fontPath,
      source_bin: fs.readFileSync(fontPath),
      ranges: [{ symbols }],
    },
  ],
  output: path.join(__dirname, '..', 'src', 'fonts', 'ziwei_font_28.c'),
  lv_font_name: 'ziwei_font_28',
  lv_fallback: null,
  // LVGL 8.4 兼容：必须不压缩（bitmap_format=0 PLAIN）。
  // lv_font_conv 1.5.3 默认输出压缩格式（bitmap_format=1），
  // LVGL 8.4 的 LV_FONT_FMT_TXT_COMPRESSED 分支需要 LV_USE_FONT_COMPRESSED，
  // 未启用时字形渲染返回空 → 文字全变方块。
  no_compress: true,
  no_prefilter: false,
  no_kerning: false,
  fast_kerning: false,
  use_color_info: false,
  full_info: false,
  lcd: false,
};

convert(args)
  .then((files) => {
    for (const [filename, data] of Object.entries(files)) {
      fs.writeFileSync(filename, data);
      console.log('written', filename, data.length, 'bytes');
    }
  })
  .catch((err) => {
    console.error(err.stack || err.message);
    process.exit(1);
  });
