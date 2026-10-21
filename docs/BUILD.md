# Build MARK on Windows

## Requirements

- Windows 10 or 11, x64.
- Visual Studio 2022 or Build Tools 2022, with Desktop development with C++ and a Windows SDK.
- CMake 3.24 or newer on PATH.
- Microsoft Edge WebView2 Evergreen Runtime to run MARK.

The repository includes the sources, headers and libraries required by the build. Python and Node.js are not required to compile or run the desktop application.

## Build

Run from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-encrypted.ps1
```

The application is written to `build-private/Release/MARK.exe`, with its runtime dependencies and resources alongside it.

The public source builds without the private access configuration used by the official knowledge bases. For normal use with the supplied knowledge bases, use the official Windows release. A build using independently prepared compatible databases must provide its matching access configuration.

To collect the application and its runtime files in one directory:

```powershell
cmake --install .\build-private --config Release --prefix .\dist\software
```

Keep the runtime files listed in `cmake/RuntimeResources.cmake` together. Preserve bundled dependency files and their license notices, including the headers and libraries under `third_party/webview2/sdk/build/native`.

See the repository README for the software download and user manual.
