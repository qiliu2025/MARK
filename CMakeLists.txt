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

# Share the bundled static transport between the application and tests.
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

add_executable(UiPreferencesScopeTests tests/ui_preferences_scope_test.cpp)
target_include_directories(UiPreferencesScopeTests PRIVATE src)
target_compile_features(UiPreferencesScopeTests PRIVATE cxx_std_20)
target_compile_definitions(UiPreferencesScopeTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(UiPreferencesScopeTests PRIVATE /utf-8 /W4 /permissive- /EHsc)
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

add_executable(ObservationPostprocessTest
    tools/observation_postprocess_test.cpp src/ObservationPostprocess.cpp)
target_include_directories(ObservationPostprocessTest PRIVATE src)
target_compile_features(ObservationPostprocessTest PRIVATE cxx_std_20)
target_compile_definitions(ObservationPostprocessTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(ObservationPostprocessTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()

# Read-only DuckDB compatibility smoke test.  The full DuckDB GUI target is
# built separately after the compatibility layer has passed this test; keeping
# this target small prevents the legacy SQLite build from changing.
set(GNSS_DOWNLOADER_DB_KEY "" CACHE STRING
    "Private DuckDB knowledge-base encryption key (do not commit)")
mark_as_advanced(GNSS_DOWNLOADER_DB_KEY)
option(GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB
    "Require native DuckDB encryption in the private release" OFF)
include("${CMAKE_SOURCE_DIR}/cmake/Setup.cmake")
# Exercise the DuckDB adapter rather than the legacy SQLite test library.
add_executable(DuckDbCompatRegressionTest
    tests/duckdb_compat_regression_test.cpp
    third_party/duckdb_sqlite_compat/sqlite3.cpp)
target_include_directories(DuckDbCompatRegressionTest PRIVATE
    third_party/duckdb_sqlite_compat third_party/duckdb)
target_compile_features(DuckDbCompatRegressionTest PRIVATE cxx_std_20)
target_compile_definitions(DuckDbCompatRegressionTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(DuckDbCompatRegressionTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(DuckDbCompatRegressionTest PRIVATE "${CMAKE_SOURCE_DIR}/third_party/duckdb/duckdb.lib")
add_custom_command(TARGET DuckDbCompatRegressionTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/third_party/duckdb/duckdb.dll"
    "$<TARGET_FILE_DIR:DuckDbCompatRegressionTest>/duckdb.dll")

add_executable(DuckDbCompatSmokeTest
    tools/duckdb_compat_smoke_test.cpp
    third_party/duckdb_sqlite_compat/sqlite3.cpp
)
target_include_directories(DuckDbCompatSmokeTest PRIVATE
    third_party/duckdb_sqlite_compat
    third_party/duckdb)
target_compile_features(DuckDbCompatSmokeTest PRIVATE cxx_std_20)
target_compile_definitions(DuckDbCompatSmokeTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# Use the same private key in the maintainer-only smoke binary when one is
# supplied.  Public builds leave this empty and continue to test env-key mode.
if(NOT GNSS_DOWNLOADER_DB_KEY STREQUAL "")
    target_compile_definitions(DuckDbCompatSmokeTest PRIVATE
        GNSS_DOWNLOADER_COMPILED_DB_KEY="${GNSS_DOWNLOADER_DB_KEY}")
endif()
if(GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB)
    target_compile_definitions(DuckDbCompatSmokeTest PRIVATE
        GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB)
endif()
if(MSVC)
    target_compile_options(DuckDbCompatSmokeTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(DuckDbCompatSmokeTest PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/duckdb/duckdb.lib")
add_custom_command(TARGET DuckDbCompatSmokeTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/duckdb/duckdb.dll"
        "$<TARGET_FILE_DIR:DuckDbCompatSmokeTest>/duckdb.dll")

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


option(IGS_DOWNLOADER_BUILD_TESTS "Build the live-data smoke test" ON)
if(IGS_DOWNLOADER_BUILD_TESTS)
    add_executable(CountryPolicySelfTest tests/country_policy_test.cpp)
    target_include_directories(CountryPolicySelfTest PRIVATE src)
    target_compile_features(CountryPolicySelfTest PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(CountryPolicySelfTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
    endif()
    add_executable(IGSDownloaderSelfTest
        tests/self_test.cpp
        src/DateUtil.cpp
        src/DownloadConcurrency.cpp
        src/HttpClient.cpp
        src/IgsAvailabilityDb.cpp
        src/NetworkAvailabilityDb.cpp
        src/IgsData.cpp
        src/SpaceWeather.cpp
        resources/test_resources.rc
    )
    target_include_directories(
        IGSDownloaderSelfTest PRIVATE src third_party/sqlite)
    target_compile_features(IGSDownloaderSelfTest PRIVATE cxx_std_20)
    target_compile_definitions(IGSDownloaderSelfTest PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
        target_compile_options(IGSDownloaderSelfTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
    endif()
    target_link_libraries(
        IGSDownloaderSelfTest PRIVATE wininet bcrypt sqlite3)

endif()

add_executable(PreciseProductsTest
    tests/precise_products_test.cpp
    src/DateUtil.cpp
    src/PreciseProducts.cpp
)
target_include_directories(PreciseProductsTest PRIVATE src)
target_compile_features(PreciseProductsTest PRIVATE cxx_std_20)
target_compile_definitions(PreciseProductsTest PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
if(MSVC)
    target_compile_options(
        PreciseProductsTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()

add_executable(PreciseBundlePlannerTest
    tests/precise_bundle_planner_test.cpp
    src/DateUtil.cpp
    src/PreciseBundlePlanner.cpp
)
target_include_directories(PreciseBundlePlannerTest PRIVATE src)
target_compile_features(PreciseBundlePlannerTest PRIVATE cxx_std_20)
target_compile_definitions(PreciseBundlePlannerTest PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
if(MSVC)
    target_compile_options(
        PreciseBundlePlannerTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()

# Read-only smoke test for the real local product indexes.  It is intentionally
# separate from the pure planner test so CI/developers can verify database
# adapters and range-query performance without launching the Win32 GUI.
add_executable(PreciseBundleCatalogSmoke
    tests/precise_bundle_catalog_smoke.cpp
    src/DateUtil.cpp
    src/PreciseBundlePlanner.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/Sp3Availability.cpp
    src/ErpAvailability.cpp
    src/OsbAvailability.cpp
    src/AntexAvailability.cpp
    src/UpdAvailability.cpp
)
target_include_directories(PreciseBundleCatalogSmoke PRIVATE src third_party/sqlite)
target_compile_features(PreciseBundleCatalogSmoke PRIVATE cxx_std_20)
target_compile_definitions(PreciseBundleCatalogSmoke PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(PreciseBundleCatalogSmoke PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(PreciseBundleCatalogSmoke PRIVATE
    wininet bcrypt sqlite3)

add_executable(Sp3AvailabilityTest
    tests/sp3_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/Sp3Availability.cpp
    resources/test_resources.rc
)
target_include_directories(Sp3AvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(Sp3AvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(Sp3AvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(Sp3AvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(Sp3AvailabilityTest PRIVATE wininet bcrypt sqlite3)


add_executable(ClkAvailabilityTest
    tests/clk_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/Sp3Availability.cpp
    resources/test_resources.rc
)
target_include_directories(ClkAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(ClkAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(ClkAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(ClkAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(ClkAvailabilityTest PRIVATE wininet bcrypt sqlite3)

add_executable(AttAvailabilityTest
    tests/att_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/Sp3Availability.cpp
    resources/test_resources.rc
)
target_include_directories(AttAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(AttAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(AttAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(AttAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(AttAvailabilityTest PRIVATE wininet bcrypt sqlite3)

add_executable(BroadcastAvailabilityTest
    tests/broadcast_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/BroadcastAvailability.cpp
    resources/test_resources.rc
)
target_include_directories(BroadcastAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(BroadcastAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(BroadcastAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(BroadcastAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(BroadcastAvailabilityTest PRIVATE wininet bcrypt sqlite3)

add_executable(DcbAvailabilityTest
    tests/dcb_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/DcbAvailability.cpp
    resources/test_resources.rc
)
target_include_directories(DcbAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(DcbAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(DcbAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(DcbAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(DcbAvailabilityTest PRIVATE wininet bcrypt sqlite3)

add_executable(OsbAvailabilityTest
    tests/osb_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/OsbAvailability.cpp
    resources/test_resources.rc
)
target_include_directories(OsbAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(OsbAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(OsbAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(OsbAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(OsbAvailabilityTest PRIVATE wininet bcrypt sqlite3)

add_executable(IonexAvailabilityTest
    tests/ionex_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/IonexAvailability.cpp
    resources/test_resources.rc
)
target_include_directories(IonexAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(IonexAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(IonexAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(IonexAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(IonexAvailabilityTest PRIVATE wininet bcrypt sqlite3)

add_executable(PreciseSearchAvailabilityTest
    tests/precise_search_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/DcbAvailability.cpp
    src/Sp3Availability.cpp
    src/TroAvailability.cpp
    src/SinexAvailability.cpp
    src/BroadcastAvailability.cpp
    src/PreciseSearchAvailability.cpp
    src/ErpAvailability.cpp
    src/AntexAvailability.cpp
    src/OsbAvailability.cpp
    src/UpdAvailability.cpp
    src/IonexAvailability.cpp
    resources/test_resources.rc
)
target_include_directories(PreciseSearchAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(PreciseSearchAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(PreciseSearchAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(PreciseSearchAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(PreciseSearchAvailabilityTest PRIVATE wininet bcrypt sqlite3)

# DuckDB-backed performance/regression variant of the precise-summary test.
# It is kept beside the legacy SQLite test so the two backends can be measured
# with exactly the same date range and family-bit assertions.
add_executable(PreciseSearchAvailabilityDuckDbTest
    tests/precise_search_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/DcbAvailability.cpp
    src/Sp3Availability.cpp
    src/TroAvailability.cpp
    src/SinexAvailability.cpp
    src/BroadcastAvailability.cpp
    src/PreciseSearchAvailability.cpp
    src/ErpAvailability.cpp
    src/AntexAvailability.cpp
    src/OsbAvailability.cpp
    src/UpdAvailability.cpp
    src/IonexAvailability.cpp
    third_party/duckdb_sqlite_compat/sqlite3.cpp
    resources/test_resources.rc
)
target_include_directories(PreciseSearchAvailabilityDuckDbTest PRIVATE
    src third_party/duckdb_sqlite_compat third_party/duckdb)
target_compile_features(PreciseSearchAvailabilityDuckDbTest PRIVATE cxx_std_20)
target_compile_definitions(PreciseSearchAvailabilityDuckDbTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    GNSS_DOWNLOADER_DUCKDB_BACKEND)
if(NOT GNSS_DOWNLOADER_DB_KEY STREQUAL "")
    target_compile_definitions(PreciseSearchAvailabilityDuckDbTest PRIVATE
        GNSS_DOWNLOADER_COMPILED_DB_KEY="${GNSS_DOWNLOADER_DB_KEY}")
endif()
if(GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB)
    target_compile_definitions(PreciseSearchAvailabilityDuckDbTest PRIVATE
        GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB)
endif()
if(MSVC)
    target_compile_options(PreciseSearchAvailabilityDuckDbTest PRIVATE
        /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(PreciseSearchAvailabilityDuckDbTest PRIVATE
    wininet bcrypt ws2_32
    "${CMAKE_SOURCE_DIR}/third_party/duckdb/duckdb.lib")
add_custom_command(TARGET PreciseSearchAvailabilityDuckDbTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/duckdb/duckdb.dll"
        "$<TARGET_FILE_DIR:PreciseSearchAvailabilityDuckDbTest>/duckdb.dll")

add_executable(VmfAvailabilityTest
    tests/vmf_availability_test.cpp
    src/VmfAvailability.cpp
    src/DateUtil.cpp
    src/IgsAvailabilityDb.cpp
    src/HttpClient.cpp
    third_party/duckdb_sqlite_compat/sqlite3.cpp
    resources/test_resources.rc)
target_include_directories(VmfAvailabilityTest PRIVATE
    src third_party/duckdb_sqlite_compat third_party/duckdb)
target_compile_features(VmfAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(VmfAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    GNSS_DOWNLOADER_DUCKDB_BACKEND)
if(NOT GNSS_DOWNLOADER_DB_KEY STREQUAL "")
    target_compile_definitions(VmfAvailabilityTest PRIVATE
        GNSS_DOWNLOADER_COMPILED_DB_KEY="${GNSS_DOWNLOADER_DB_KEY}")
endif()
if(GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB)
    target_compile_definitions(VmfAvailabilityTest PRIVATE
        GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB)
endif()
if(MSVC)
    target_compile_options(VmfAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(VmfAvailabilityTest PRIVATE
    wininet bcrypt ws2_32 "${CMAKE_SOURCE_DIR}/third_party/duckdb/duckdb.lib")
add_custom_command(TARGET VmfAvailabilityTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/duckdb/duckdb.dll"
        "$<TARGET_FILE_DIR:VmfAvailabilityTest>/duckdb.dll")

add_executable(ErpAvailabilityTest
    tests/erp_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/Sp3Availability.cpp
    src/ErpAvailability.cpp
    resources/test_resources.rc
)
target_include_directories(ErpAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(ErpAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(ErpAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(ErpAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(ErpAvailabilityTest PRIVATE wininet bcrypt sqlite3)

add_executable(SinexAvailabilityTest
    tests/sinex_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/Sp3Availability.cpp
    src/SinexAvailability.cpp
    resources/test_resources.rc
)
target_include_directories(SinexAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(SinexAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(SinexAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(SinexAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(SinexAvailabilityTest PRIVATE wininet bcrypt sqlite3)

add_executable(TroAvailabilityTest
    tests/tro_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/Sp3Availability.cpp
    src/TroAvailability.cpp
    resources/test_resources.rc
)
target_include_directories(TroAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(TroAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(TroAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(TroAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(TroAvailabilityTest PRIVATE wininet bcrypt sqlite3)

add_executable(AntexAvailabilityTest
    tests/antex_availability_test.cpp
    src/DateUtil.cpp
    src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp
    src/AntexAvailability.cpp
    resources/test_resources.rc
)
target_include_directories(AntexAvailabilityTest PRIVATE src third_party/sqlite)
target_compile_features(AntexAvailabilityTest PRIVATE cxx_std_20)
target_compile_definitions(AntexAvailabilityTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(AntexAvailabilityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(AntexAvailabilityTest PRIVATE wininet bcrypt sqlite3)

add_executable(StationSelectionTest
    tests/station_selection_test.cpp
    src/DateUtil.cpp
    src/StationSelection.cpp
    src/StationSelectionCoverage.cpp
)
add_executable(SiteLogDbTest
    tests/site_log_test.cpp
    src/SiteLogDb.cpp
    src/HttpClient.cpp
)
target_include_directories(SiteLogDbTest PRIVATE src third_party/sqlite)
target_compile_features(SiteLogDbTest PRIVATE cxx_std_20)
target_compile_definitions(SiteLogDbTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(SiteLogDbTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(SiteLogDbTest PRIVATE wininet bcrypt ws2_32 sqlite3)

# Regression coverage for time-aware receiver/antenna Site Log history.  Keep
# this separate from the legacy parser test so migrations can be exercised
# without changing the existing smoke-test fixture.
add_executable(SiteLogHistoryTest
    tests/site_log_history_test.cpp
    src/SiteLogDb.cpp
    src/HttpClient.cpp
)
target_include_directories(SiteLogHistoryTest PRIVATE src third_party/sqlite)
target_compile_features(SiteLogHistoryTest PRIVATE cxx_std_20)
target_compile_definitions(SiteLogHistoryTest PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(SiteLogHistoryTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(SiteLogHistoryTest PRIVATE wininet bcrypt ws2_32 sqlite3)

if(IGS_DOWNLOADER_BUILD_TESTS)
    # Regression test for reused four-character station codes.  It builds a
    # tiny synthetic set of source databases and verifies that cross-network
    # physical stations remain separate while the explicit SOPAC/PANGA STAR
    # alias merges.
    add_executable(ObservationSearchIdentityTest
        tests/observation_search_identity_test.cpp
        src/ObservationSearchDb.cpp
        src/SpaceWeather.cpp
        src/DateUtil.cpp
        src/SiteLogDb.cpp
        src/HttpClient.cpp
        src/DownloadConcurrency.cpp
        src/IgsAvailabilityDb.cpp
        src/NetworkAvailabilityDb.cpp
        src/IgsData.cpp
        resources/test_resources.rc
    )
    target_include_directories(
        ObservationSearchIdentityTest PRIVATE src third_party/sqlite)
    target_compile_features(ObservationSearchIdentityTest PRIVATE cxx_std_20)
    target_compile_definitions(ObservationSearchIdentityTest PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(
            ObservationSearchIdentityTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
    endif()
    target_link_libraries(
        ObservationSearchIdentityTest PRIVATE wininet bcrypt ws2_32 sqlite3)

    # High-rate checkpoint revisions are tested against a tiny temporary DB;
    # this target needs no archive access or application resources.
    add_executable(IgsHighrateCacheTest
        tests/igs_highrate_cache_test.cpp
        src/IgsAvailabilityDb.cpp
        src/DateUtil.cpp
    )
    target_include_directories(
        IgsHighrateCacheTest PRIVATE src third_party/sqlite)
    target_compile_features(IgsHighrateCacheTest PRIVATE cxx_std_20)
    target_compile_definitions(IgsHighrateCacheTest PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(
            IgsHighrateCacheTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
    endif()
    target_link_libraries(IgsHighrateCacheTest PRIVATE sqlite3)
endif()

add_executable(GeonetDownloadSmoke
    tests/geonet_download_smoke.cpp
    src/HttpClient.cpp
)

add_executable(DownloadJournalTests tests/download_journal_test.cpp)
target_include_directories(DownloadJournalTests PRIVATE src)
target_compile_features(DownloadJournalTests PRIVATE cxx_std_20)

add_executable(DownloadPlanTests tests/download_plan_test.cpp)
target_include_directories(DownloadPlanTests PRIVATE src)
target_compile_features(DownloadPlanTests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(DownloadPlanTests PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_compile_definitions(DownloadJournalTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(DownloadJournalTests PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_include_directories(GeonetDownloadSmoke PRIVATE src)
target_compile_features(GeonetDownloadSmoke PRIVATE cxx_std_20)
target_compile_definitions(GeonetDownloadSmoke PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(
        GeonetDownloadSmoke PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(GeonetDownloadSmoke PRIVATE wininet bcrypt)
add_custom_command(TARGET GeonetDownloadSmoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/curl/curl-libressl.exe"
        "$<TARGET_FILE_DIR:GeonetDownloadSmoke>/curl-libressl.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/curl/curl-ca-bundle.crt"
        "$<TARGET_FILE_DIR:GeonetDownloadSmoke>/curl-ca-bundle.crt")

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
add_dependencies(GeonetDownloadSmoke GnssSftpAskPass)
install(TARGETS GnssSftpAskPass RUNTIME DESTINATION .)

add_executable(ObservationEquipmentHistoryTest tests/observation_equipment_history_test.cpp)
target_include_directories(ObservationEquipmentHistoryTest PRIVATE src)
target_compile_features(ObservationEquipmentHistoryTest PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(ObservationEquipmentHistoryTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()

# Archive parser regression matrix; never accesses a live server.
add_executable(ArchiveFilenameTest
    tests/archive_filename_test.cpp
    src/DateUtil.cpp src/DownloadConcurrency.cpp src/HttpClient.cpp
    src/IgsAvailabilityDb.cpp src/NetworkAvailabilityDb.cpp
    src/SpaceWeather.cpp src/BroadcastAvailability.cpp
    src/Sp3Availability.cpp src/ErpAvailability.cpp src/OsbAvailability.cpp
    src/SinexAvailability.cpp src/IonexAvailability.cpp src/TroAvailability.cpp resources/test_resources.rc)
target_include_directories(ArchiveFilenameTest PRIVATE src third_party/sqlite)
target_compile_features(ArchiveFilenameTest PRIVATE cxx_std_20)
target_compile_definitions(ArchiveFilenameTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(ArchiveFilenameTest PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
target_link_libraries(ArchiveFilenameTest PRIVATE wininet bcrypt sqlite3)
