cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED GNSS_RESOURCE_SOURCE_DIR OR NOT DEFINED GNSS_RESOURCE_DESTINATION)
    message(FATAL_ERROR "GNSS_RESOURCE_SOURCE_DIR and GNSS_RESOURCE_DESTINATION are required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/RuntimeResources.cmake")
gnss_runtime_resource_files("${GNSS_RESOURCE_SOURCE_DIR}" resource_files)
gnss_runtime_validate_destination(
    "${GNSS_RESOURCE_SOURCE_DIR}" "${GNSS_RESOURCE_DESTINATION}")

foreach(resource_file IN LISTS resource_files)
    get_filename_component(resource_subdir "${resource_file}" DIRECTORY)
    file(MAKE_DIRECTORY "${GNSS_RESOURCE_DESTINATION}/${resource_subdir}")
    file(COPY_FILE
        "${GNSS_RESOURCE_SOURCE_DIR}/${resource_file}"
        "${GNSS_RESOURCE_DESTINATION}/${resource_file}"
        ONLY_IF_DIFFERENT)
endforeach()
