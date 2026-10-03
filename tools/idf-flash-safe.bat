@echo off
rem Safe flash: bootloader + partition-table + app ONLY.
rem
rem Do NOT flash a merged image starting at 0x0. The gap between the
rem partition table (0x8000) and the app (0x10000) is filled with 0xFF, which
rem erases the nvs partition at 0x9000 - that is where the BLE bonding keys
rem live. The device then forgets the host while Windows keeps its stale key,
rem giving "connect -> immediately disconnect" and
rem "Could not start notify: Unreachable".
rem
rem Flashing the three regions separately leaves nvs untouched, so the pairing
rem survives re-flashing.
set MSYSTEM=
set MSYS=
set "IDF_TOOLS_PATH=C:\esp\espressif"
set "IDF_PATH=C:\esp\esp-idf"
set "IDF_SKIP_CHECK_SUBMODULES=1"
set PYTHONUNBUFFERED=1
set "IDF_PYTHON_ENV_PATH=C:\esp\espressif\python_env\idf5.5_py3.12_env"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;C:\esp\espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin;%PATH%"
cd /d D:\ai-passport\firmware
python -m esptool --chip esp32c3 -p COM9 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\FoloToy-AI-Passport.bin
if errorlevel 1 echo FLASH_SAFE_FAILED & exit /b 1
echo FLASH_SAFE_OK
