# Minimal ESP-IDF v4.4.7 environment for this project (dot-source it):
#   . runtime/firmware/tools/idf-env.ps1
# Uses only the ESP32-S3 compiler and IDF's Python venv installed by
#   py -3.11 tools/idf_tools.py install xtensa-esp32s3-elf
#   py -3.11 tools/idf_tools.py install-python-env
# plus the host's own CMake/Ninja. Espressif's export.ps1 would insist on the
# full tool set (RISC-V ULP compiler, debugger, OpenOCD) that this build never uses.
$env:IDF_PATH = Join-Path $HOME 'esp\esp-idf-v4.4.7'
$env:IDF_PYTHON_ENV_PATH = Join-Path $HOME '.espressif\python_env\idf4.4_py3.11_env'
$toolchain = Join-Path $HOME '.espressif\tools\xtensa-esp32s3-elf\esp-2021r2-patch5-8.4.0\xtensa-esp32s3-elf\bin'
foreach ($p in @($toolchain, (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts'))) {
    if (-not (Test-Path $p)) { throw "Missing $p - install the toolchain/venv first (see README)." }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -or -not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    throw 'CMake (>=3.16) and Ninja must be on PATH.'
}
$env:PATH = "$toolchain;$(Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts');$env:PATH"
$env:FW_NM = Join-Path $toolchain 'xtensa-esp32s3-elf-nm.exe'
function global:idf.py { python (Join-Path $env:IDF_PATH 'tools\idf.py') @args }
Write-Host "ESP-IDF v4.4.7 environment ready (python: $((Get-Command python).Source))"
