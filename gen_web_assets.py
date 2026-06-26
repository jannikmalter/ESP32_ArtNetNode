# Generates src/web_assets.h (a null-terminated C array of src/index.html) so the
# web UI page can be embedded in flash and served by start_webserver() in main.c.
#
# Runs as a PlatformIO pre-build script (extra_scripts = pre:gen_web_assets.py) and
# also standalone (python gen_web_assets.py). Only rewrites the header when the
# page actually changed, so it doesn't force a recompile on every build.
#
# To add more web assets later (e.g. an OTA upload page), emit additional arrays
# here from the same source dir.
import os

try:
    Import("env")  # provided by PlatformIO/SCons
    PROJ = env["PROJECT_DIR"]
except Exception:
    PROJ = os.path.dirname(os.path.abspath(__file__))

SRC = os.path.join(PROJ, "src", "index.html")
DST = os.path.join(PROJ, "src", "web_assets.h")

with open(SRC, "rb") as f:
    data = f.read()

out = [
    "/* AUTO-GENERATED from src/index.html by gen_web_assets.py - do not edit. */\n",
    "#pragma once\n",
    "static const char index_html[] = {\n",
]
line = "\t"
for b in data:
    line += "0x%02x," % b
    if len(line) >= 100:
        out.append(line + "\n")
        line = "\t"
out.append(line + "0x00\n};\n")
text = "".join(out)

old = None
if os.path.exists(DST):
    with open(DST, "r", encoding="utf-8") as f:
        old = f.read()

if text != old:
    with open(DST, "w", encoding="utf-8") as f:
        f.write(text)
    print("gen_web_assets: wrote web_assets.h (%d bytes from index.html)" % len(data))
else:
    print("gen_web_assets: web_assets.h up to date")
