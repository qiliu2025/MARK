configure_file("${CMAKE_SOURCE_DIR}/src/setup/SetupResources.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SetupResources.rc" @ONLY)
add_executable(MARKSetup WIN32
    src/setup/MarkSetup.cpp src/setup/PackageReader.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/SetupResources.rc")
target_include_directories(MARKSetup PRIVATE
    src/setup third_party/duckdb third_party/libressl-src/include)
target_compile_features(MARKSetup PRIVATE cxx_std_20)
target_compile_definitions(MARKSetup PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
if(NOT GNSS_DOWNLOADER_DB_KEY STREQUAL "")
    target_compile_definitions(MARKSetup PRIVATE
        GNSS_DOWNLOADER_COMPILED_DB_KEY="${GNSS_DOWNLOADER_DB_KEY}")
endif()
if(MSVC)
    target_compile_options(MARKSetup PRIVATE /utf-8 /W4 /permissive- /EHsc)
    target_link_options(MARKSetup PRIVATE /MANIFEST:NO)
endif()
target_link_libraries(MARKSetup PRIVATE crypto bcrypt comctl32 shell32 ole32 uuid)
set_target_properties(MARKSetup PROPERTIES OUTPUT_NAME "MARKSetup")

add_executable(MarkSetupTests tests/setup_space_test.cpp)
target_include_directories(MarkSetupTests PRIVATE src/setup)
target_compile_features(MarkSetupTests PRIVATE cxx_std_20)
target_compile_definitions(MarkSetupTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(MarkSetupTests PRIVATE /utf-8 /W4 /permissive- /EHsc)
endif()
