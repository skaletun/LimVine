@echo off
rem ===========================================================================
rem  LimVine — production-сборка и сборка инсталлятора Windows (NSIS .exe)
rem
rem  Требования:
rem    * Visual Studio 2022 (рабочая нагрузка «Desktop development with C++»)
rem      или standalone cmake + ninja;
rem    * CMake >= 3.20 в PATH;
rem    * NSIS 3.x (makensis.exe) — опционально, только для генератора ZIP
rem      (портативный пакет). Полноценный мастер установки .exe собирается
rem      CPack/NSIS на Linux-раннерах GitHub Actions (см. .github/workflows).
rem
rem  Использование:
rem    build_installer.bat            # Release x64 + dist\LimVine-<ver>-win64.zip
rem    build_installer.bat Debug      # конфигурация Debug
rem    build_installer.bat clean      # полная пересборка с нуля
rem
rem  Что происходит:
rem    1. Конфигурирование: MSVC /utf-8, динамический CRT (/MD), тесты включены.
rem    2. Сборка limvine.lib, lvrun.exe, lvcook.exe.
rem    3. Прогон ctest — без зелёных тестов инсталлятор не выпускается.
rem    4. cpack -G ZIP — портативная копия установочного дерева в dist\.
rem       (NSIS-мастер с ярлыками, uninstall.exe и записями реестра формирует
rem        CI-пайплайн тем же cpack, но с -G NSIS.)
rem ===========================================================================
setlocal
cd /d "%~dp0"

set "CONF=Release"
if /I "%~1"=="Debug" set "CONF=Debug"
if /I "%~1"=="debug" set "CONF=Debug"

if /I "%~1"=="clean" (
    rmdir /s /q build 2>nul
    rmdir /s /q dist 2>nul
)

echo [1/4] Конфигурирование...
cmake -B build -S . -DCMAKE_BUILD_TYPE=%CONF% ^
    -DLV_BUILD_TESTS=ON -DLV_BUILD_TOOLS=ON -DLV_WERROR=OFF
if errorlevel 1 goto :fail

echo [2/4] Сборка...
cmake --build build --config %CONF% --parallel
if errorlevel 1 goto :fail

echo [3/4] Тесты...
ctest --test-dir build -C %CONF% --output-on-failure
if errorlevel 1 goto :fail

echo [4/4] Пакет...
if not exist dist mkdir dist
cpack -C build -G ZIP ^
    -CPACK_PACKAGE_FILE_NAME "LimVine-%CONF%-win64-portable"
if errorlevel 1 goto :fail

echo.
echo Готово: dist\LimVine-%CONF%-win64-portable.zip
echo Для полноценного мастера установки (.exe с uninstall) выполните:
echo     cpack -C build -G NSIS
echo (нужен установленный NSIS 3.x; так же делает CI: .github/workflows/release.yml)
exit /b 0

:fail
echo.
echo СБОРКА ОСТАНОВЛЕНА: см. вывод выше.
exit /b 1
