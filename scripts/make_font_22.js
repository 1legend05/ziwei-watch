// 生成 LVGL 22px 中文字体（结果页/问答页小字号，一屏信息密度更高）
'use strict';
const fs = require('fs');
const path = require('path');
const convert = require(path.join(__dirname, 'node_modules', 'lv_font_conv', 'lib', 'convert'));

const symbols = fs.readFileSync(path.join(__dirname, 'chars.txt'), 'utf8').replace(/[\r\n]/g, '');
const fontPath = path.join(__dirname, 'NotoSansSC-Regular.otf');

const args = {
  size: 22,
  bpp: 4,
  format: 'lvgl',
  font: [
    {
      source_path: fontPath,
      source_bin: fs.readFileSync(fontPath),
      ranges: [{ symbols }],
    },
  ],
  output: path.join(__dirname, '..', 'src', 'fonts', 'ziwei_font_22.c'),
  lv_font_name: 'ziwei_font_22',
  lv_fallback: null,
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
