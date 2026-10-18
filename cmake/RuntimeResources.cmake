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

function(gnss_clean_legacy_runtime_resources source_dir destination_dir)
    file(REAL_PATH "${source_dir}" source_real)
    gnss_runtime_resolve_output_path("${destination_dir}" destination_real)
    # A case-insensitive prefix check prevents cleaning source resources.
    string(TOLOWER "${source_real}" source_compare)
    string(TOLOWER "${destination_real}" destination_compare)
    cmake_path(IS_PREFIX source_compare "${destination_compare}" NORMALIZE inside_source)
    if(inside_source)
        message(FATAL_ERROR "Runtime resource destination must be outside source resources")
    endif()

    # Cleanup is limited to the listed legacy output paths.
    set(legacy_paths
        ui/brand-options
        ui/brand-preview.html
        ui/ui
        ui/mark-logo.svg
        mark.ico
        country-policy.json
        station-area-evidence.json
        country-code-audit.csv
        special-area-audit.csv
        ne_10m_populated_places.geojson
        ne_50m_admin_1_states_provinces.geojson
        map-sources/offline-reference-sources-20260929.json
        map-sources/palestine-ocha-admin0-20260929.geojson
        map-sources/ne_10m_minor_islands.geojson
        map-sources/ne_10m_admin_0_disputed_areas_scale_rank_minor_islands.geojson
        map-sources/natural-earth-land-50m-20260929.geojson
        map-sources/natural-earth-disputed.original.geojson
        map-sources/natural-earth-disputed-lines-50m-20260929.geojson
        map-sources/natural-earth-countries.original.geojson
        map-sources/natural-earth-countries-10m.original.geojson
        map-sources/natural-earth-coastline-50m-20260929.geojson
        map-sources/natural-earth-boundary-lines-50m-20260929.geojson
        map-sources/diaoyu-official-locations-20260929.json
        map-sources/china-boundary-presentation-20260929.geojson
        map-sources/china-admin-datav-20260929.geojson)
    gnss_runtime_resource_files("${source_dir}" runtime_paths)
    # Resolve links before cleanup and reject paths outside the resource output.
    foreach(relative_path IN LISTS runtime_paths legacy_paths)
        gnss_runtime_resolve_output_path("${destination_real}/${relative_path}" candidate_real)
        string(TOLOWER "${candidate_real}" candidate_compare)
        cmake_path(IS_PREFIX destination_compare "${candidate_compare}" NORMALIZE inside_destination)
        if(NOT inside_destination)
            message(FATAL_ERROR "Resource path resolves outside the destination: ${relative_path}")
        endif()
    endforeach()
    foreach(legacy_path IN LISTS legacy_paths)
        set(legacy_file "${destination_real}/${legacy_path}")
        if(EXISTS "${legacy_file}" OR IS_SYMLINK "${legacy_file}")
            gnss_runtime_resolve_output_path("${legacy_file}" legacy_real)
            string(TOLOWER "${legacy_real}" legacy_compare)
            cmake_path(IS_PREFIX destination_compare "${legacy_compare}" NORMALIZE inside_destination)
            if(NOT inside_destination)
                message(FATAL_ERROR "Refusing to clean a resource path outside the destination: ${legacy_path}")
            endif()
            file(REMOVE_RECURSE "${legacy_file}")
        endif()
    endforeach()
endfunction()

function(gnss_install_runtime_resources)
    set(resource_source "${CMAKE_SOURCE_DIR}/resources")
    gnss_runtime_resource_files("${resource_source}" resource_files)
    # Remove the same named obsolete files when updating an existing install.
    install(CODE "
        include(\"${CMAKE_CURRENT_FUNCTION_LIST_FILE}\")
        gnss_clean_legacy_runtime_resources(
            \"${resource_source}\"
            \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/resources\")
    ")
    foreach(resource_file IN LISTS resource_files)
        get_filename_component(resource_subdir "${resource_file}" DIRECTORY)
        install(FILES "${resource_source}/${resource_file}"
            DESTINATION "resources/${resource_subdir}")
    endforeach()
endfunction()
