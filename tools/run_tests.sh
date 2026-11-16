#!/bin/sh
# Host-side tests: C++ frame codec + AT parsers, then C++ <-> Python byte parity. No hardware needed.
set -e
cd "$(dirname "$0")/.."
mkdir -p .pio/native
clang++ -std=c++17 -Wall -Wextra -Werror -I lib/wxlink \
  lib/wxlink/wx_frame.cpp lib/wxlink/e5_parse.cpp test/native/test_link.cpp \
  -o .pio/native/test_link
.pio/native/test_link
python3 tools/test_parity.py .pio/native/test_link
