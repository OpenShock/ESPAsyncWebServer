#!/bin/bash

set -e

if [ ! -z "$TRAVIS_BUILD_DIR" ]; then
	export GITHUB_WORKSPACE="$TRAVIS_BUILD_DIR"
	export GITHUB_REPOSITORY="$TRAVIS_REPO_SLUG"
elif [ -z "$GITHUB_WORKSPACE" ]; then
	export GITHUB_WORKSPACE="$PWD"
	export GITHUB_REPOSITORY="me-no-dev/ESPAsyncWebServer"
fi

TARGET_PLATFORM="$1"
CHUNK_INDEX=$2
CHUNKS_CNT=$3
BUILD_PIO=0
if [ "$#" -lt 1 ]; then
	TARGET_PLATFORM="esp32"
fi
if [ "$#" -lt 3 ] || [ "$CHUNKS_CNT" -le 0 ]; then
	CHUNK_INDEX=0
	CHUNKS_CNT=1
elif [ "$CHUNK_INDEX" -gt "$CHUNKS_CNT" ]; then
	CHUNK_INDEX=$CHUNKS_CNT
elif [ "$CHUNK_INDEX" -eq "$CHUNKS_CNT" ]; then
	BUILD_PIO=1
fi

# PlatformIO Test
source ./.github/scripts/install-platformio.sh

python -m platformio lib --storage-dir "$GITHUB_WORKSPACE" install
echo "Installing ArduinoJson ..."
python -m platformio lib -g install https://github.com/bblanchon/ArduinoJson.git > /dev/null 2>&1
if [[ "$TARGET_PLATFORM" == "esp32" ]]; then
	BOARD="esp32dev"
	echo "Installing AsyncTCP ..."
	python -m platformio lib -g install https://github.com/me-no-dev/AsyncTCP.git > /dev/null 2>&1
	echo "BUILDING ESP32 EXAMPLES"
fi
build_pio_sketches "$BOARD" "$GITHUB_WORKSPACE/examples"