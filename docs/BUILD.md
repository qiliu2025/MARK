# Build MARK on Windows

## Requirements

- Windows 10 or 11, x64.
- Visual Studio 2022 (or Build Tools 2022), with **Desktop development with C++** and a Windows SDK.
- CMake 3.24 or newer on `PATH`.
- WebView2 Evergreen Runtime for running the built application; the compile-time WebView2 SDK is bundled in this repository.

curl and LibreSSL are built from the bundled source. SQLite's amalgamation, DuckDB's Windows x64 library and C API header, and the WebView2 native x64 headers and static loader are also bundled. Official DuckDB source URLs are recorded in `third_party/duckdb/SOURCE_ORIGIN.json`; its source tree is not needed to compile MARK. Python is not needed to build or run the desktop program.

## Build the encrypted DuckDB target

From the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-encrypted.ps1
```

The script configures an x64 Visual Studio project, requires encrypted DuckDB files, and builds the `GNSSDownloaderDuckDB` target. The output is `build-private/Release/MARK.exe` with the required DuckDB DLL and application resources alongside it.

Build and install steps copy the runtime assets, user documentation and license notices listed in `cmake/RuntimeResources.cmake`. Update this manifest when adding a runtime dependency.

Create a distribution software directory after building:

```powershell
cmake --install .\build-private --config Release --prefix .\dist\software
```

The default software installation contains `MARK.exe`, the SFTP and SOCKS helper programs, their dependencies and runtime resources. The full release installer also provides the separately licensed encrypted knowledge bases, the English PDF user manual and the corresponding MARK source archive.

The public source's database key defaults to an empty string. Without a key, the source can compile, but that executable cannot decrypt the official release knowledge bases. The supplied release executable already has the matching read capability and requires no end-user key entry.

For a developer's own encrypted databases, supply **your own** database key through the process environment variable `GNSS_DOWNLOADER_DB_KEY` before running the build script. Do not place the official key, your own private key, CMake cache, compiler files, or a keyed executable in the Git repository. The script avoids passing the key directly on the command line, but the private build directory can contain it in generated files.

Optional arguments:

```powershell
.\scripts\build-encrypted.ps1 -Configuration Debug -Jobs 4
.\scripts\build-encrypted.ps1 -ConfigureOnly
```

## Application distribution layout

```text
MARK/
├─ software/
│  ├─ MARK.exe
│  ├─ duckdb.dll
│  ├─ GnssSftpAskPass.exe
│  └─ resources/
├─ knowledge-base/
│  └─ encrypted DuckDB files and their accompanying metadata
└─ source/
   └─ MARK-v1.0.0-source.zip
```

The compiled target alone does not produce the full knowledge bases. For normal use, download and run the official Windows installer. A developer using different databases must build with a matching key and retain the application's expected knowledge-base directory structure.

## Tests

The repository includes application and database-adapter tests. Some tests use
small synthetic databases; tests that query provider endpoints require network
access. Build only the targets needed for a change. For example:

```powershell
cmake --build .\build-private --config Release --target DuckDbCompatRegressionTest
```

Use `scripts/build-encrypted.ps1` for the application build.

## Repository hygiene

`.gitignore` ignores only root build/output directories, rather than every directory called `build`. In particular, `third_party/webview2/sdk/build/native` must remain in the repository: it contains required headers and libraries. Required bundled `.exe`, `.dll`, and `.lib` dependency files are also retained.

Knowledge bases and full application bundles belong in Releases. The Windows installer is kept below GitHub's per-file attachment limit.

## Single-file Windows setup

Build the desktop application and setup executable with the same key:

```powershell
.\scripts\build-encrypted.ps1 -WithSetup
```

`MARKSetup.exe` embeds the bundled extraction tools and DuckDB library. The package builder appends an authenticated encrypted archive to this executable. Setup streams the archive to a temporary workspace, restores encrypted DuckDB files, verifies table contents and moves the prepared directory into place. Existing nonempty installation folders are not overwritten.

Packaging requires Python 3.12 or later, DuckDB 1.5.5 and `cryptography`. These are maintainer dependencies; users of the generated installer do not need Python. Allow 50 GiB of temporary packaging space.

```powershell
python -m pip install duckdb==1.5.5 cryptography
python .\tools\package_mark_source.py --output .\dist\MARK\source\MARK-v1.0.0-source.zip
python .\tools\package_mark_setup.py `
  --portable .\dist\MARK `
  --setup .\build-private\Release\MARKSetup.exe `
  --key-file .\private\knowledge-base.key `
  --work .\build-private\setup-work `
  --output .\release\MARK-v1.0.0-Windows-x64.exe
```

The portable input follows the directory layout above and includes documentation, licenses and the corresponding source archive. Create the source archive after finalizing the application, build scripts and documentation. The supplied key file must contain 64 hexadecimal characters and match both the input databases and setup executable. Packaging creates unencrypted temporary database copies, verifies them and removes its temporary workspace on completion. Keep the workspace private and out of the source repository.

The generated installer stores package sizes in its MARKSFX2 trailer. Its free-space check includes archive extraction, concurrent database conversion and a 3% allowance (at least 256 MiB). It uses one preparation worker when free space is below the two-worker budget. Setup displays the prepared size, including the source archive. The knowledge bases retain native DuckDB encryption. Source and knowledge-base licenses continue to apply separately.
