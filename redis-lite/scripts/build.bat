@echo off
REM ============================================================================
REM redis-lite — Windows 开发构建脚本（MinGW-w64 / g++）
REM
REM 用途：在本机做功能验证与调试。
REM 注意：Windows 后端是 WSAPoll（电平触发），性能数据一律以 Linux/epoll 为准。
REM
REM 用法:
REM   scripts\build.bat          编译服务端 + 压测工具 + 单元测试
REM   scripts\build.bat test     编译并运行单元测试
REM   scripts\build.bat run      编译并启动服务端（后台端口 6379）
REM ============================================================================

setlocal enabledelayedexpansion

set ROOT=%~dp0..
set OUT=%ROOT%\build-win
set CXX=g++
REM -mconsole      : MinGW 必须显式声明 console 子系统，否则链接期去找 WinMain
REM -D_WIN32_WINNT : MinGW 的 _mingw.h 默认锁在 XP(0x502)，会让 inet_pton 未声明
set CXXFLAGS=-std=c++17 -O2 -mconsole -Wall -Wextra -D_WIN32_WINNT=0x0600 -Wno-unused-parameter -Wno-missing-field-initializers
set INCLUDE=-I"%ROOT%\include" -I"%ROOT%\tests"
set LIBS=-lws2_32

if not exist "%OUT%" mkdir "%OUT%"

REM 收集 src 下除 main.cc 外的全部实现文件。
set SOURCES=
for %%f in ("%ROOT%\src\*.cpp") do (
    if not "%%~nxf"=="main.cc" set SOURCES=!SOURCES! "%%f"
)
if "%SOURCES%"=="" (
    echo ERROR: no sources found under %ROOT%\src
    exit /b 1
)

echo ==^> compiling core + server
%CXX% %CXXFLAGS% %INCLUDE% %SOURCES% "%ROOT%\src\main.cc" -o "%OUT%\redis-lite.exe" %LIBS%
if errorlevel 1 goto :fail

echo ==^> compiling bench tool
%CXX% %CXXFLAGS% %INCLUDE% %SOURCES% "%ROOT%\tools\bench.cc" -o "%OUT%\rl-bench.exe" %LIBS%
if errorlevel 1 goto :fail

echo ==^> compiling unit tests
%CXX% %CXXFLAGS% %INCLUDE% %SOURCES% "%ROOT%\tests\test_main.cc" "%ROOT%\tests\test_resp.cc" "%ROOT%\tests\test_store.cc" "%ROOT%\tests\test_aof.cc" -o "%OUT%\rl-tests.exe" %LIBS%
if errorlevel 1 goto :fail

echo.
echo ==^> build OK: %OUT%
dir /b "%OUT%"

if /I "%1"=="test" (
    echo.
    echo ==^> running unit tests
    "%OUT%\rl-tests.exe"
    exit /b %errorlevel%
)

if /I "%1"=="run" (
    echo.
    echo ==^> starting server on port 6379 (Ctrl+C to stop)
    "%OUT%\redis-lite.exe" --port 6379 --threads 4 --log-level info
    exit /b 0
)

echo.
echo 启动服务端:  %OUT%\redis-lite.exe --port 6379 --threads 4
echo 跑单元测试:  %OUT%\rl-tests.exe
echo 压测:        %OUT%\rl-bench.exe -c 50 -n 200000 -P 16 -d 64 -t set
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
