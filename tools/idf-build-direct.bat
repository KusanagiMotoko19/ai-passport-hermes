@echo off
rem Build Hermes-Buddy firmware directly, bypassing export.bat's tool validation.
rem Native cmd only; MSYSTEM must be cleared or idf_tools.py refuses to run.
set MSYSTEM=
set MSYS=
set "IDF_TOOLS_PATH=C:\esp\espressif"
set "IDF_PATH=C:\esp\esp-idf"
set "IDF_SKIP_CHECK_SUBMODULES=1"
set PYTHONUNBUFFERED=1
set "IDF_PYTHON_ENV_PATH=C:\esp\espressif\python_env\idf5.5_py3.12_env"
set "ESP_ROM_ELF_DIR=C:\esp\espressif\tools\esp-rom-elfs\20241011\"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;C:\esp\espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin;C:\esp\espressif\tools\cmake\3.30.2\bin;C:\esp\espressif\tools\ninja\1.12.1;%PATH%"
cd /d D:\ai-passport\firmware
if not exist sdkconfig (
    echo [set-target esp32c3]
    python "%IDF_PATH%\tools\idf.py" set-target esp32c3
    if errorlevel 1 echo SET_TARGET_FAILED & exit /b 1
)
python "%IDF_PATH%\tools\idf.py" build
if errorlevel 1 echo BUILD_FAILED & exit /b 1
echo BUILD_OK
dir /b build\*.bin
