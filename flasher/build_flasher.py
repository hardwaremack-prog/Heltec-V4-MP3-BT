#!/usr/bin/env python3
"""Build flasher/index.html with the firmware and esptool-js embedded (works offline).

usage: build_flasher.py <merged.bin> <version> <esptool-js bundle.js>
The merged .bin is trimmed of trailing 0xFF padding before embedding.
"""
import base64, os, sys

if len(sys.argv) != 4:
    sys.exit(__doc__)
merged, version, bundle = sys.argv[1:]
here = os.path.dirname(os.path.abspath(__file__))
fw = open(merged, 'rb').read()
n = (len(fw.rstrip(b'\xff')) + 4095) // 4096 * 4096
fw = fw[:n]
js = open(bundle, encoding='utf-8').read()
assert '</script' not in js.lower()
t = open(os.path.join(here, 'template.html'), encoding='utf-8').read()
t = (t.replace('__ESPTOOL__', js)
      .replace('__FW_B64__', base64.b64encode(fw).decode())
      .replace('__VERSION__', version)
      .replace('__SIZE__', str(len(fw) // 1024)))
open(os.path.join(here, 'index.html'), 'w', encoding='utf-8').write(t)
print(f'flasher/index.html built: firmware v{version}, {len(fw)//1024} KB')
