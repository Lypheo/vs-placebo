#!/usr/bin/env sh
set -e

brew update

# macOS runner issue, see:
#   - https://github.com/mpv-player/mpv/pull/18557
#   - https://github.com/asimov-platform/actions/pull/3
if brew list openssl@1.1 &> /dev/null; then brew rm openssl@1.1; fi

brew install shaderc glslang dovi_tool
brew install libplacebo --HEAD
