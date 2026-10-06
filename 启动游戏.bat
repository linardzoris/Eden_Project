@echo off
setlocal EnableDelayedExpansion

set "ROOT=%~dp0"
set "CACHE_DIR=%ROOT%_profiles\shaders_cache"

:menu
cls
echo ========================================
echo            Eden 引擎启动器
echo ========================================
echo.
echo   [1] Release
echo   [2] ReleaseAVX
echo   [3] Mixed      ^(开发者模式，将清理着色器缓存^)
echo   [4] MixedAVX   ^(开发者模式，将清理着色器缓存^)
echo.
echo   [0] 退出
echo.
echo ========================================
choice /c 12340 /n /m "请选择启动项: "

if errorlevel 5 exit /b 0
if errorlevel 4 set "CFG=MixedAVX" & goto launch
if errorlevel 3 set "CFG=Mixed" & goto launch
if errorlevel 2 set "CFG=ReleaseAVX" & goto launch
if errorlevel 1 set "CFG=Release" & goto launch
goto menu

:launch
set "EXE=%ROOT%bins\DXLegacy\%CFG%\xrEngine.exe"

if not exist "%EXE%" (
    echo.
    echo [错误] 未找到引擎文件:
    echo        %EXE%
    echo.
    echo 请先将 %CFG% 构建输出部署到 bins\DXLegacy\%CFG%\ 目录。
    echo.
    pause
    goto menu
)

rem Mixed / MixedAVX 启动前删除着色器缓存，避免旧缓存与新二进制不兼容
if /i "%CFG%"=="Mixed" goto clearcache
if /i "%CFG%"=="MixedAVX" goto clearcache
goto startengine

:clearcache
if exist "%CACHE_DIR%" (
    echo 正在清理着色器缓存: %CACHE_DIR%
    rd /s /q "%CACHE_DIR%"
)

:startengine
echo 正在启动 %CFG% ...
start "" /D "%ROOT%" "%EXE%"

rem 引擎已启动，退出控制台，不影响游戏运行
endlocal
exit /b 0