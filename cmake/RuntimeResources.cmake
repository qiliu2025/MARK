# Runtime resource list shared by build and install.
include_guard(GLOBAL)

option(GNSS_DOWNLOADER_INSTALL_DEVELOPMENT_TOOLS
    "Install the legacy SQLite application and maintenance tools" OFF)

function(gnss_install_development_target target_name)
    if(GNSS_DOWNLOADER_INSTALL_DEVELOPMENT_TOOLS)
        install(TARGETS "${target_name}" RUNTIME DESTINATION . COMPONENT Development)
    endif()
endfunction()

function(gnss_runtime_resource_files source_dir output_variable)
    set(resource_files
        ui/index.html
        ui/ui.css
        ui/ui.js
        ui/release-info.js
        ui/weather-preview.js
        ui/weather-preview.css
        ui/coverage.js
        ui/runtime-log.js
        ne_110m_land.geojson
        ne_50m_admin_0_countries.geojson
        country_flags.bmp
        reference_frames/IGS14_core.txt
        reference_frames/IGb14_core.txt
        reference_frames/IGS20_core.txt
        reference_frames/IGb20_core.txt
        reference_frames/IGc20_core.txt
        reference_frames/README.md)

    # Workbench assets are required as a group when mark-brand.css is present.
    if(EXISTS "${source_dir}/ui/mark-brand.css")
        list(APPEND resource_files
            ui/mark-brand.css
            ui/mark-brand.js
            ui/mark-icon.png
            ui/station-selection.css
            ui/station-selection.js
            ui/workbench-layout.css
            ui/responsive-layout.css
            ui/country-policy.js)
    endif()

    # Bundle postprocessing tools with their licenses when the module is present.
    if(EXISTS "${source_dir}/download-tools/7z.exe"
            OR EXISTS "${source_dir}/download-tools/CRX2RNX.exe")
        list(APPEND resource_files
            download-tools/7z.exe
            download-tools/7z.dll
            download-tools/CRX2RNX.exe
            download-tools/7zip-LICENSE.txt
            download-tools/RNXCMP-LICENSE.txt
            download-tools/README.md
            download-tools/RNXCMP-README.txt)
    endif()

    # Copy optional notices individually.
    foreach(notice IN ITEMS
            download-tools/LGPL-2.1.txt
            map-data-README.md
            map-sources/natural-earth-public-domain-README-20260929.txt)
        if(EXISTS "${source_dir}/${notice}")
            list(APPEND resource_files "${notice}")
        endif()
    endforeach()

    foreach(resource_file IN LISTS resource_files)
        if(NOT EXISTS "${source_dir}/${resource_file}"
                OR IS_DIRECTORY "${source_dir}/${resource_file}")
            message(FATAL_ERROR "Missing runtime resource: ${source_dir}/${resource_file}")
        endif()
    endforeach()
    set(${output_variable} "${resource_files}" PARENT_SCOPE)
endfunction()

function(gnss_runtime_resolve_output_path input_path output_variable)
    get_filename_component(existing_path "${input_path}" ABSOLUTE)
    set(missing_suffix "")
    while(NOT EXISTS "${existing_path}")
        if(IS_SYMLINK "${existing_path}")
            message(FATAL_ERROR "Resource output path contains a dangling link: ${input_path}")
        endif()
        get_filename_component(path_name "${existing_path}" NAME)
        get_filename_component(parent_path "${existing_path}" DIRECTORY)
        if(parent_path STREQUAL existing_path)
            message(FATAL_ERROR "Cannot resolve resource output path: ${input_path}")
        endif()
        set(missing_suffix "${path_name}/${missing_suffix}")
        set(existing_path "${parent_path}")
    endwhile()
    file(REAL_PATH "${existing_path}" resolved_path)
    set(resolved_path "${resolved_path}/${missing_suffix}")
    cmake_path(NORMAL_PATH resolved_path)
    set(${output_variable} "${resolved_path}" PARENT_SCOPE)
endfunction()

function(gnss_runtime_validate_destination source_dir destination_dir)
    file(REAL_PATH "${source_dir}" source_real)
    gnss_runtime_resolve_output_path("${destination_dir}" destination_real)
    string(TOLOWER "${source_real}" source_compare)
    string(TOLOWER "${destination_real}" destination_compare)
    cmake_path(IS_PREFIX source_compare "${destination_compare}" NORMALIZE inside_source)
    if(inside_source)
        message(FATAL_ERROR "Runtime resource destination must be outside source resources")
    endif()

    gnss_runtime_resource_files("${source_dir}" runtime_paths)
    foreach(relative_path IN LISTS runtime_paths)
        gnss_runtime_resolve_output_path("${destination_real}/${relative_path}" candidate_real)
        string(TOLOWER "${candidate_real}" candidate_compare)
        cmake_path(IS_PREFIX destination_compare "${candidate_compare}" NORMALIZE inside_destination)
        if(NOT inside_destination)
            message(FATAL_ERROR "Resource path resolves outside the destination: ${relative_path}")
        endif()
    endforeach()
endfunction()

function(gnss_install_runtime_resources)
    set(resource_source "${CMAKE_SOURCE_DIR}/resources")
    gnss_runtime_resource_files("${resource_source}" resource_files)
    install(CODE "
        include(\"${CMAKE_CURRENT_FUNCTION_LIST_FILE}\")
        gnss_runtime_validate_destination(
            \"${resource_source}\"
            \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/resources\")
    ")
    foreach(resource_file IN LISTS resource_files)
        get_filename_component(resource_subdir "${resource_file}" DIRECTORY)
        install(FILES "${resource_source}/${resource_file}"
            DESTINATION "resources/${resource_subdir}")
    endforeach()
endfunction()
