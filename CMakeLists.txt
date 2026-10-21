cmake_minimum_required(VERSION 3.24)

project(IGSDownloader VERSION 1.0.0 LANGUAGES CXX RC)

# Visual Studio uses cl.exe for both languages.  Some existing build trees were
# originally C++-only, so seed the C compiler from the already-detected C++
# compiler before enabling C for the official SQLite amalgamation.
if(MSVC AND NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "${CMAKE_CXX_COMPILER}" CACHE FILEPATH "C compiler" FORCE)
endif()
enable_language(C)

# Keep every bundled static dependency on the same CRT as the application.
# This must be set before adding libcurl's subdirectory.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Use repository-relative paths in application and dependency diagnostics.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:/experimental:deterministic>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:/pathmap:${CMAKE_SOURCE_DIR}=MARK>")
endif()

# Build libcurl and LibreSSL into the application instead of spawning curl.exe
# for file transfers. Terras currently fails with Schannel on some Windows
# installations, while LibreSSL interoperates with the same endpoint.
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(LIBRESSL_APPS OFF CACHE BOOL "" FORCE)
set(LIBRESSL_TESTS OFF CACHE BOOL "" FORCE)
set(LIBRESSL_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIMES ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/libressl-src EXCLUDE_FROM_ALL)

# LibreSSL exposes the OpenSSL-compatible API consumed by curl. Supplying the
# existing in-tree targets avoids a machine-wide OpenSSL installation.
add_library(OpenSSL::Crypto INTERFACE IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/include"
    INTERFACE_LINK_LIBRARIES crypto)
add_library(OpenSSL::SSL INTERFACE IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/include"
    INTERFACE_LINK_LIBRARIES "ssl;OpenSSL::Crypto")
set(OPENSSL_FOUND TRUE)
set(OPENSSL_VERSION "4.3.2")
set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/curl-src EXCLUDE_FROM_ALL)

add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
endif()

# Use the bundled static transport for application targets.
link_libraries(CURL::libcurl_static)

# Password-only OpenSSH AskPass helper.  It reads one inherited environment
# variable and writes it to OpenSSH; credentials never touch a temporary file
# or the command line.
add_executable(GnssSftpAskPass WIN32 tools/sftp_askpass.cpp)
target_compile_features(GnssSftpAskPass PRIVATE cxx_std_20)
target_compile_definitions(GnssSftpAskPass PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(
        GnssSftpAskPass PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()

add_executable(IGSDownloader WIN32
    src/main.cpp
    src/WebWorkbenchHost.cpp
    src/DateUtil.cpp
    src/DownloadConcurrency.cpp
    src/ObservationPostprocess.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/SiteLogDb.cpp
    src/NetworkAvailabilityDb.cpp
    src/ObservationSearchDb.cpp
    src/IgsData.cpp
    src/MapView.cpp
    src/MainWindow.cpp
    src/PreciseProducts.cpp
    src/PreciseSearchAvailability.cpp
    src/PreciseBundlePlanner.cpp
    src/PreciseProductView.cpp
    src/ClkAntexInfo.cpp
    src/Sp3Availability.cpp
    src/BroadcastAvailability.cpp
    src/DcbAvailability.cpp
    src/OsbAvailability.cpp
    src/IonexAvailability.cpp
    src/ErpAvailability.cpp
    src/SinexAvailability.cpp
    src/TroAvailability.cpp
    src/VmfAvailability.cpp
    src/UpdAvailability.cpp
    src/AntexAvailability.cpp
    src/StationSelection.cpp
    src/StationSelectionCoverage.cpp
    src/SpaceWeather.cpp
    resources/app.rc
)

target_include_directories(IGSDownloader PRIVATE
    src
    third_party/sqlite
    third_party/webview2/sdk/build/native/include)
target_compile_features(IGSDownloader PRIVATE cxx_std_20)
target_compile_definitions(IGSDownloader PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(IGSDownloader PRIVATE /utf-8 /W4 /permissive- /EHsc)
    # app.rc already embeds the full Common Controls / DPI manifest as resource 1.
    target_link_options(IGSDownloader PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(IGSDownloader PRIVATE
    comctl32
    wininet
    bcrypt
    comdlg32
    shell32
    shlwapi
    ole32
    gdiplus
    uxtheme
    "${CMAKE_SOURCE_DIR}/third_party/webview2/sdk/build/native/x64/WebView2LoaderStatic.lib"
    sqlite3
)

set_target_properties(IGSDownloader PROPERTIES
    OUTPUT_NAME "GNSSDownloader"
)
add_dependencies(IGSDownloader GnssSftpAskPass)

set(GNSS_DOWNLOADER_DB_KEY "" CACHE STRING
    "Private DuckDB knowledge-base encryption key (do not commit)")
mark_as_advanced(GNSS_DOWNLOADER_DB_KEY)
option(GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB
    "Require native DuckDB encryption in the private release" OFF)
include("${CMAKE_SOURCE_DIR}/cmake/Setup.cmake")

# Desktop application using the native DuckDB adapter.
get_target_property(DUCKDB_GUI_SOURCES IGSDownloader SOURCES)
add_executable(GNSSDownloaderDuckDB WIN32
    ${DUCKDB_GUI_SOURCES}
    third_party/duckdb_sqlite_compat/sqlite3.cpp)
target_include_directories(GNSSDownloaderDuckDB PRIVATE
    src
    third_party/duckdb_sqlite_compat
    third_party/duckdb
    third_party/webview2/sdk/build/native/include)
target_compile_features(GNSSDownloaderDuckDB PRIVATE cxx_std_20)
target_compile_definitions(GNSSDownloaderDuckDB PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    GNSS_DOWNLOADER_DUCKDB_BACKEND)
# The real value is supplied only by the maintainer's private build command.
# It is intentionally absent from the public source and release scripts.
if(NOT GNSS_DOWNLOADER_DB_KEY STREQUAL "")
    target_compile_definitions(GNSSDownloaderDuckDB PRIVATE
        GNSS_DOWNLOADER_COMPILED_DB_KEY="${GNSS_DOWNLOADER_DB_KEY}")
endif()
if(GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB)
    target_compile_definitions(GNSSDownloaderDuckDB PRIVATE
        GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB)
endif()
if(MSVC)
    target_compile_options(GNSSDownloaderDuckDB PRIVATE /utf-8 /W4 /permissive- /EHsc)
    target_link_options(GNSSDownloaderDuckDB PRIVATE /MANIFEST:NO)
endif()
target_link_libraries(GNSSDownloaderDuckDB PRIVATE
    comctl32 wininet bcrypt comdlg32 shell32 shlwapi ole32 gdiplus uxtheme
    "${CMAKE_SOURCE_DIR}/third_party/webview2/sdk/build/native/x64/WebView2LoaderStatic.lib"
    "${CMAKE_SOURCE_DIR}/third_party/duckdb/duckdb.lib")
set_target_properties(GNSSDownloaderDuckDB PROPERTIES
    OUTPUT_NAME "MARK")
include("${CMAKE_SOURCE_DIR}/cmake/RuntimeResources.cmake")
add_custom_target(CopyDuckDbWorkbenchUi
    COMMAND "${CMAKE_COMMAND}"
        "-DGNSS_RESOURCE_SOURCE_DIR=${CMAKE_SOURCE_DIR}/resources"
        "-DGNSS_RESOURCE_DESTINATION=$<TARGET_FILE_DIR:GNSSDownloaderDuckDB>/resources"
        -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeResources.cmake"
    COMMENT "Synchronizing whitelisted DuckDB runtime resources"
    VERBATIM)
add_dependencies(GNSSDownloaderDuckDB GnssSftpAskPass CopyDuckDbWorkbenchUi)
add_custom_command(TARGET GNSSDownloaderDuckDB POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/duckdb/duckdb.dll"
        "$<TARGET_FILE_DIR:GNSSDownloaderDuckDB>/duckdb.dll")

if(EXISTS "${CMAKE_SOURCE_DIR}/data/gnss_availability.sqlite")
    add_custom_command(TARGET IGSDownloader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:IGSDownloader>/data"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/data/gnss_availability.sqlite"
            "$<TARGET_FILE_DIR:IGSDownloader>/data/gnss_availability.sqlite")
endif()

add_custom_command(TARGET IGSDownloader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:IGSDownloader>/data"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/curl/curl-libressl.exe"
        "$<TARGET_FILE_DIR:IGSDownloader>/curl-libressl.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/curl/curl-ca-bundle.crt"
        "$<TARGET_FILE_DIR:IGSDownloader>/curl-ca-bundle.crt"
    COMMAND "${CMAKE_COMMAND}"
        "-DGNSS_RESOURCE_SOURCE_DIR=${CMAKE_SOURCE_DIR}/resources"
        "-DGNSS_RESOURCE_DESTINATION=$<TARGET_FILE_DIR:IGSDownloader>/resources"
        -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeResources.cmake"
    VERBATIM)

install(TARGETS GNSSDownloaderDuckDB RUNTIME DESTINATION .)
install(FILES third_party/duckdb/duckdb.dll DESTINATION .)
gnss_install_development_target(IGSDownloader)
if(GNSS_DOWNLOADER_INSTALL_DEVELOPMENT_TOOLS)
    install(FILES data/gnss_availability.sqlite DESTINATION data OPTIONAL COMPONENT Development)
endif()
install(FILES
    third_party/curl/curl-libressl.exe
    third_party/curl/curl-ca-bundle.crt
    third_party/curl/COPYING.txt
    DESTINATION .)
gnss_install_runtime_resources()

# Restricted SOCKS5 stdio relay used only as OpenSSH ProxyCommand for the two
# official public-credential SFTP archives supported by HttpClient.cpp.
add_executable(GnssSocksRelay tools/socks_relay.cpp)
target_compile_features(GnssSocksRelay PRIVATE cxx_std_20)
target_compile_definitions(GnssSocksRelay PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(GnssSocksRelay PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(GnssSocksRelay PRIVATE ws2_32)
add_dependencies(IGSDownloader GnssSocksRelay)
add_dependencies(GNSSDownloaderDuckDB GnssSocksRelay)
install(TARGETS GnssSocksRelay RUNTIME DESTINATION .)

install(TARGETS GnssSftpAskPass RUNTIME DESTINATION .)

