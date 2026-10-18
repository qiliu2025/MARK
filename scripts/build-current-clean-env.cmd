@echo off
set "ALL_PROXY="
set "all_proxy="
set "HTTP_PROXY="
set "http_proxy="
set "HTTPS_PROXY="
set "https_proxy="
set "NO_PROXY="
set "no_proxy="

cmake --build "%~dp0..\out\build" --config Release
exit /b %errorlevel%
