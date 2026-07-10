# Configuration

TARGETS := 'WioTrackerL1_companion_radio_ble GAT562_mesh_trial_tracker_companion_radio_ble GAT562_mesh_tracker_pro_companion_radio_ble GAT562_30s_mesh_kit_companion_radio_ble Meshtiny_companion_radio_ble RAK_4631_companion_radio_ble heltec_v3_companion_radio_ble heltec_v4_companion_radio_ble Heltec_t096_companion_radio_ble heltec_tracker_v2_companion_radio_ble fobe_idea_mesh_tracker_c1_companion_radio_ble ProMicro_companion_radio_ble ProMicro_companion_sx1268_radio_ble tinylora_c3_mv_sx1262_companion_radio_ble'

# fobe_quill_nrf52840_mesh_companion_radio_ble
# Heltec_t114_companion_radio_ble
# m5stack_cardputer_adv_sx1262_companion_radio_ble

VENV_DIR := 'venv'
VENV_BIN := VENV_DIR + '/bin'
PIO := VENV_BIN + '/pio'
PIP := VENV_BIN + '/pip'
PYTHON := VENV_BIN + '/python'
NRFUTIL := VENV_BIN + '/adafruit-nrfutil'
ESPTOOL := VENV_BIN + '/esptool'
COMMIT_HASH := `git rev-parse --short HEAD 2>/dev/null || echo dirty`
FIRMWARE_BUILD_DATE := `date '+%d-%b-%Y'`
FIRMWARE_VERSION := '1.4.3'
TARGET := 'heltec_v3_companion_radio_ble'
FIRMWARE_VERSION_STRING := FIRMWARE_VERSION + '-' + COMMIT_HASH
RELEASE_DIR := FIRMWARE_VERSION + '-' + COMMIT_HASH
PREFIX := '-whisper-'
SUFFIX := ''
FIRMWARE_NAME := replace(TARGET, '_companion_radio', '') + PREFIX + FIRMWARE_VERSION_STRING + SUFFIX
BUILD_DIR := '.pio/build/' + TARGET
FIRMWARE_ARTIFACT := RELEASE_DIR + '/' + FIRMWARE_NAME
WHISPER_BUILD_FLAGS := "-DCOMMIT_HASH='\\\"" + COMMIT_HASH + "\\\"' -DFIRMWARE_VERSION='\\\"" + FIRMWARE_VERSION + "\\\"' -DFIRMWARE_BUILD_DATE='\\\"" + FIRMWARE_BUILD_DATE + "\\\"' -DBLE_NAME_PREFIX='\\\"Whisper-\\\"'"

# Release and build

## Build every release target and collect versioned artifacts.
## Usage: just release
release:
    #!/usr/bin/env bash
    set -euo pipefail

    mkdir -p '{{ RELEASE_DIR }}'
    base_platformio_build_flags="${PLATFORMIO_BUILD_FLAGS:-}"
    failed=0

    for target in {{ TARGETS }}; do
      export PLATFORMIO_BUILD_FLAGS="$base_platformio_build_flags {{ WHISPER_BUILD_FLAGS }}"
      echo "Building release for target: $target $PLATFORMIO_BUILD_FLAGS"
      if ! just TARGET="$target" FIRMWARE_VERSION='{{ FIRMWARE_VERSION }}' COMMIT_HASH='{{ COMMIT_HASH }}' PREFIX='{{ PREFIX }}' SUFFIX='{{ SUFFIX }}' build; then
        echo "Build failed for target: $target" >&2
        failed=1
      fi
    done

    exit "$failed"

## Toggle ESP32 power-management sdkconfig defaults for the selected target.
## Usage: just configure-power-management
configure-power-management:
    #!/usr/bin/env bash
    set -euo pipefail

    if [[ '{{ TARGET }}' == *tinylora_c3_mv* ]]; then
      sed -i \
        -e 's/^  ;\?CONFIG_PM_ENABLE=y$/  ;CONFIG_PM_ENABLE=y/' \
        -e 's/^  ;\?CONFIG_FREERTOS_USE_TICKLESS_IDLE=y$/  ;CONFIG_FREERTOS_USE_TICKLESS_IDLE=y/' \
        -e 's/^  ;\?CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3$/  ;CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3/' \
        platformio.ini
    else
      sed -i \
        -e 's/^  ;CONFIG_PM_ENABLE=y$/  CONFIG_PM_ENABLE=y/' \
        -e 's/^  ;CONFIG_FREERTOS_USE_TICKLESS_IDLE=y$/  CONFIG_FREERTOS_USE_TICKLESS_IDLE=y/' \
        -e 's/^  ;CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3$/  CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3/' \
        platformio.ini
    fi

## Build one TARGET and copy its bin/merged-bin/zip/uf2 release artifacts.
## Usage: just TARGET=heltec_v4_companion_radio_ble build
build: venv
    #!/usr/bin/env bash
    set -euo pipefail

    echo '{{ FIRMWARE_VERSION_STRING }}' > version.txt
    mkdir -p '{{ RELEASE_DIR }}'

    {{ PIO }} run -e '{{ TARGET }}'

    if [ -f {{ BUILD_DIR }}/firmware.bin ]; then
      {{ PIO }} run -t mergebin -e '{{ TARGET }}'
    fi

    if [ -f {{ BUILD_DIR }}/firmware.zip ] && [ -f {{ BUILD_DIR }}/firmware.hex ]; then
      {{ PYTHON }} bin/uf2conv/uf2conv.py {{ BUILD_DIR }}/firmware.hex -c -o '{{ FIRMWARE_ARTIFACT }}.uf2' -f 0xADA52840
    fi

    if [ -f {{ BUILD_DIR }}/firmware.zip ]; then
      mv {{ BUILD_DIR }}/firmware.zip '{{ FIRMWARE_ARTIFACT }}.zip'
    fi

    if [ -f {{ BUILD_DIR }}/firmware.bin ]; then
      mv {{ BUILD_DIR }}/firmware.bin '{{ FIRMWARE_ARTIFACT }}.bin'
    fi

    if [ -f {{ BUILD_DIR }}/firmware-merged.bin ]; then
      mv {{ BUILD_DIR }}/firmware-merged.bin '{{ FIRMWARE_ARTIFACT }}-merged.bin'
    fi

# Flashing

## Build and flash the selected PlatformIO TARGET through its upload backend.
## Usage: just TARGET=heltec_v4_companion_radio_ble upload
upload: venv
    #!/usr/bin/env bash
    set -euo pipefail

    export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:-} {{ WHISPER_BUILD_FLAGS }}"
    {{ PIO }} run -e '{{ TARGET }}' --target upload

## Build, merge, and flash the selected PlatformIO TARGET as a full image.
## Usage: just TARGET=heltec_v4_companion_radio_ble merged-upload
merged-upload: venv
    #!/usr/bin/env bash
    set -euo pipefail

    export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:-} {{ WHISPER_BUILD_FLAGS }}"
    {{ PIO }} run -e '{{ TARGET }}' --target upload -t mergebin

## Flash an nRF52 DFU zip over serial.
## Usage: just nrf52-flash /dev/cu.usbmodem101
nrf52-flash zip_file serial_port='/dev/cu.usbmodem101' baud='115200' package='': venv
    #!/usr/bin/env bash
    set -euo pipefail

    {{ NRFUTIL }} \
        --verbose dfu serial \
        --package '{{ zip_file }}' \
        -p '{{ serial_port }}' \
        -b '{{ baud }}' \
        --singlebank \
        --touch 1200

## Flash an ESP app or merged bin with chip/flash settings detected or kept by esptool.
## Usage: just esp32-flash /path/to/firmware.bin /dev/cu.usbmodem2101
esp32-flash bin_file serial_port baud='921600' offset='' chip='auto' flash_mode='keep' flash_freq='keep' flash_size='detect': venv
    #!/usr/bin/env bash
    set -euo pipefail

    flash_offset='{{ offset }}'
    if [ -z "$flash_offset" ]; then
      case '{{ bin_file }}' in
        *-merged.bin) flash_offset='0x0' ;;
        *) flash_offset='0x10000' ;;
      esac
    fi

    {{ ESPTOOL }} \
        --chip '{{ chip }}' \
        --port '{{ serial_port }}' \
        --baud '{{ baud }}' \
        --before default-reset \
        --after hard-reset \
        write-flash -z \
        --flash-mode '{{ flash_mode }}' \
        --flash-freq '{{ flash_freq }}' \
        --flash-size '{{ flash_size }}' \
        "$flash_offset" '{{ bin_file }}'

## Run native tests with the same firmware metadata defines used by builds.
## Usage: just test
test: venv
    #!/usr/bin/env bash
    set -euo pipefail

    export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:-} {{ WHISPER_BUILD_FLAGS }}"
    {{ PIO }} test -e native

# Environment

## Create the local Python virtual environment used by PlatformIO and flash tools.
## Usage: just venv
venv:
    @test -d {{ VENV_DIR }} || python3 -m venv {{ VENV_DIR }}

## Install Python tooling required for builds, nRF52 DFU, ESP flashing, and MeshCore.
## Usage: just install
install: venv
    {{ PIP }} install --upgrade pip
    {{ PIP }} install platformio adafruit-nrfutil esptool meshcore

## List PlatformIO environments available in the project.
## Usage: just envs
envs: venv
    {{ PIO }} project config | grep 'env:' | sed 's/env://'

# Maintenance

## Remove generated release directories, PlatformIO build output, and transient config files.
## Usage: just clean
clean:
    @rm -rf {{ RELEASE_DIR }}
    @rm -rf .pio/build
    @rm -rf .dummy CMakeLists.txt dependencies.lock managed_components sdkconfig.* version.txt

## Format the Whisper UI/helper source files with the project clang-format command.
## Usage: just lint
lint:
    #!/usr/bin/env bash
    set -euo pipefail

    cd examples/companion_radio/whisper
    clang-format -i \
      UITask.cpp UITask.h \
      icons.h \
      screens/*.h \
      text_input/*.h \
      *.h \
      ../../../src/helpers/esp32/SerialBLEInterface.h ../../../src/helpers/esp32/SerialBLEInterface.cpp \
      ../../../src/helpers/ui/MorseCodeInput.cpp ../../../src/helpers/ui/MorseCodeInput.h \
      ../../../src/helpers/ui/ES8311Audio.cpp ../../../src/helpers/ui/ES8311Audio.h \
      ../../../src/helpers/ui/TCA8418Keyboard.h \
      ../../../src/helpers/ui/SSD1306MonoDisplay.cpp ../../../src/helpers/ui/SSD1306MonoDisplay.h \
      ../../../src/helpers/ui/SH1106MonoDisplay.cpp ../../../src/helpers/ui/SH1106MonoDisplay.h \
      ../../../src/helpers/ui/ST7789MonoDisplay.cpp ../../../src/helpers/ui/ST7789MonoDisplay.h \
      ../../../src/helpers/ui/ST7735MonoDisplay.cpp ../../../src/helpers/ui/ST7735MonoDisplay.h \
      ../../../src/helpers/ui/UnicodeFont.cpp \
      ../CompanionAPIHandler.cpp ../CompanionAPIHandler.h \
      ../../../src/helpers/ui/UnicodeFont.h \
      ../../../src/helpers/ui/UnicodeRenderer.cpp \
      ../../../src/helpers/ui/UnicodeRenderer.h \
      ../../../src/helpers/ui/Locale.cpp ../../../src/helpers/ui/Locale.h \
      ../../../src/helpers/ui/I18n.h \
      ../../../src/PowerStatusProvider.h ../../../src/CardKeyboard.h

# Debug

## Print the resolved Just variables used by build and release recipes.
## Usage: just debug
debug:
    @echo VENV_DIR: {{ VENV_DIR }}
    @echo PATH: $PATH
    @echo COMMIT_HASH: {{ COMMIT_HASH }}
    @echo FIRMWARE_BUILD_DATE: {{ FIRMWARE_BUILD_DATE }}
    @echo FIRMWARE_VERSION: {{ FIRMWARE_VERSION }}
    @echo TARGET: {{ TARGET }}
    @echo FIRMWARE_VERSION_STRING: {{ FIRMWARE_VERSION_STRING }}
    @echo PLATFORMIO_BUILD_FLAGS: ${PLATFORMIO_BUILD_FLAGS:-}
    @echo SUFFIX: {{ SUFFIX }}
    @echo FIRMWARE_NAME: {{ FIRMWARE_NAME }}
    @echo BUILD_DIR: {{ BUILD_DIR }}
    @echo FIRMWARE_ARTIFACT: {{ FIRMWARE_ARTIFACT }}
    @echo WHISPER_BUILD_FLAGS: {{ WHISPER_BUILD_FLAGS }}

# Artifact sync

## Copy the current release artifact directory into the local nightly staging area.
## Usage: just upload-firmwares
upload-firmwares:
    rsync -ah --progress {{ RELEASE_DIR }} /dev/shm/nightly
