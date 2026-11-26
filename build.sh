#!/bin/sh
# Builds AppToMic.exe with the MinGW-w64 cross compiler (Linux, WSL or MSYS2).
set -e
cd "$(dirname "$0")/src"
mkdir -p ../build
x86_64-w64-mingw32-windres app.rc -O coff -o ../build/app.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -s -municode -mwindows -static -static-libgcc -static-libstdc++ \
  -Wall -Wno-missing-field-initializers -o ../build/AppToMic.exe AppToMic.cpp ../build/app.res \
  -lole32 -luuid -lcomctl32 -lavrt -ladvapi32 -luser32 -lgdi32
echo "Built build/AppToMic.exe"
