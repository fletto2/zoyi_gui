#!/bin/bash
# One-time MSYS2 UCRT64 setup for the Windows build. Run in a login shell:
# MSYSTEM=UCRT64 C:\msys64\usr\bin\bash.exe -l setup_msys2.sh
set -e
pacman -Syu --noconfirm || true
pacman -Syu --noconfirm
pacman -S --needed --noconfirm make \
  mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-qt6-static mingw-w64-ucrt-x86_64-freetype \
  mingw-w64-ucrt-x86_64-harfbuzz mingw-w64-ucrt-x86_64-graphite2 \
  mingw-w64-ucrt-x86_64-libpng mingw-w64-ucrt-x86_64-brotli \
  mingw-w64-ucrt-x86_64-bzip2 mingw-w64-ucrt-x86_64-zlib \
  mingw-w64-ucrt-x86_64-pcre2 mingw-w64-ucrt-x86_64-libb2 \
  mingw-w64-ucrt-x86_64-openssl
pacman -Q mingw-w64-ucrt-x86_64-qt6-static mingw-w64-ucrt-x86_64-gcc make
