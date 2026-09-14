cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

# Optional local component cache for offline builds. Normal builds use the
# versions pinned in main/idf_component.yml via the ESP Component Registry.
if(DEFINED ENV{AI_PASSPORT_LOCAL_COMPONENTS})
    file(TO_CMAKE_PATH "$ENV{AI_PASSPORT_LOCAL_COMPONENTS}" AI_PASSPORT_LOCAL_COMPONENTS_PATH)
    set(EXTRA_COMPONENT_DIRS "${AI_PASSPORT_LOCAL_COMPONENTS_PATH}")
    list(PREPEND CMAKE_MODULE_PATH
         "${AI_PASSPORT_LOCAL_COMPONENTS_PATH}/espressif__cmake_utilities")
endif()

set(PROJECT_VER "0.7.0")
project(ai_passport_recorder)
