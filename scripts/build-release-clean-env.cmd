@echo off
set "ALL_PROXY="
set "all_proxy="
set "HTTP_PROXY="
set "http_proxy="
set "HTTPS_PROXY="
set "https_proxy="
set "NO_PROXY="
set "no_proxy="

cmake -S "%~dp0.." -B "%~dp0..\out\build-v05"
if errorlevel 1 exit /b %errorlevel%

cmake --build "%~dp0..\out\build-v05" --config Release
exit /b %errorlevel%
