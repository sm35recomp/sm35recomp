#!/bin/bash
# usage: recomp/tools/make_icons.sh   (macOS: sips, iconutil, python3)
# Every platform's app icon from the one master, docs/branding/app-icon.png (1024 square, no alpha; drawn by
# docs/branding/draw_icon.py: original pixel art), into docs/branding/ (committed: the release builds run where sips
# isn't):
#   app-icon-1024.png             the master's size, for pages and the README
#   app-icon.icns                 macOS (16-1024, iconutil)
#   app-icon.ico                  Windows: 16 / 32 / 48 / 256 as PNG entries (the exe's icon resource, the zip)
#   app-icon-256.png              Linux (the tarball's .desktop icon)
#   ios/AppIcon<size>.png         iOS / iPadOS: 60@2x/3x (iPhone), 76@2x and 83.5@2x (iPad), 1024 (App Store slot)
#   android/mipmap-<dpi>/ic_launcher.png   Android launcher: 48 / 72 / 96 / 144 / 192
set -euo pipefail
cd "$(dirname "$0")/../.."
M=docs/branding/app-icon.png
O=docs/branding
[ -f "$M" ] || { echo "make_icons: no $M"; exit 1; }
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
png() { sips -s format png -z "$1" "$1" "$M" --out "$2" >/dev/null; }  # (size, out)

# macOS
mkdir -p "$W/app-icon.iconset"
for s in 16 32 128 256 512; do
    png $s "$W/app-icon.iconset/icon_${s}x${s}.png"
    png $((s * 2)) "$W/app-icon.iconset/icon_${s}x${s}@2x.png"
done
iconutil -c icns "$W/app-icon.iconset" -o "$O/app-icon.icns"

# Windows: an ICO whose entries are PNGs (Vista and later read them at every size)
for s in 16 32 48 256; do png $s "$W/ico-$s.png"; done
python3 - "$O/app-icon.ico" "$W"/ico-16.png "$W"/ico-32.png "$W"/ico-48.png "$W"/ico-256.png <<'EOF'
import struct, sys
out, files = sys.argv[1], sys.argv[2:]
datas = [open(f, 'rb').read() for f in files]
sizes = [struct.unpack('>II', d[16:24])[0] for d in datas]  # (IHDR width)
head = struct.pack('<HHH', 0, 1, len(datas))
off = 6 + 16 * len(datas)
entries = b''
for d, s in zip(datas, sizes):
    entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(d), off)
    off += len(d)
open(out, 'wb').write(head + entries + b''.join(datas))
EOF

# Linux; the README
png 256 "$O/app-icon-256.png"
png 1024 "$O/app-icon-1024.png"

# iOS / iPadOS (bundle icon files, named in Info.plist's CFBundleIcons: tools/make_ios_app.sh)
mkdir -p "$O/ios"
png 120 "$O/ios/AppIcon60x60@2x.png"
png 180 "$O/ios/AppIcon60x60@3x.png"
png 152 "$O/ios/AppIcon76x76@2x~ipad.png"
png 167 "$O/ios/AppIcon83.5x83.5@2x~ipad.png"
png 1024 "$O/ios/AppIcon1024x1024.png"

# Android
for d in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    mkdir -p "$O/android/mipmap-${d%%:*}"
    png "${d#*:}" "$O/android/mipmap-${d%%:*}/ic_launcher.png"
done
du -sh "$O"
