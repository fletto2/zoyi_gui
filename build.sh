#!/bin/bash
# Windows build wrapper, for running from PowerShell or cmd:
#   $env:MSYSTEM='UCRT64'; & C:\msys64\usr\bin\bash.exe -l <path>\build.sh
# Builds into build/win unless ZT_BUILD names another (space-free) folder.
# make cannot handle spaces in the path, so keep the checkout in a folder
# without them, or set ZT_BUILD.
set -e
cd "$(dirname "$0")"
exec make ${ZT_BUILD:+BUILD="$ZT_BUILD"} "$@"
