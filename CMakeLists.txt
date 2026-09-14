cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

# Optional local component cache for offline builds. Normal builds use the
# versions pinned in main/idf_component.yml via the ESP Component Registry.
if(DEFINED ENV{FOLO_LOCAL_COMPONENTS})
    file(TO_CMAKE_PATH "$ENV{FOLO_LOCAL_COMPONENTS}" FOLO_LOCAL_COMPONENTS_PATH)
    set(EXTRA_COMPONENT_DIRS "${FOLO_LOCAL_COMPONENTS_PATH}")
    list(PREPEND CMAKE_MODULE_PATH
         "${FOLO_LOCAL_COMPONENTS_PATH}/espressif__cmake_utilities")
endif()

set(PROJECT_VER "0.7.0")
project(folo_recorder_c3)
