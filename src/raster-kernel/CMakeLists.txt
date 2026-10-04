set(RASTER_SOURCES
  "main.cpp"
  "stbi.cpp"
)

set(RASTER_HEADERS
  "stdafx.hpp"
  "include.hpp"
)

add_executable(raster ${RASTER_SOURCES} ${RASTER_HEADERS})

set_target_properties(raster PROPERTIES
  OUTPUT_NAME "raster"
  FOLDER "Applications"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:raster>"
  VS_DEBUGGER_COMMAND_ARGUMENTS "--scene-name=sponza --framebuffer-width=512 --framebuffer-height=512"
)

arches_set_output_dirs(raster)

target_compile_features(raster PRIVATE cxx_std_20)

file(TO_CMAKE_PATH "${ARCHES_DATASET_DIR}" ARCHES_DEFAULT_DATASET_DIR_CMAKE)
target_compile_definitions(raster
  PRIVATE
    ARCHES_DEFAULT_DATASET_DIR="${ARCHES_DEFAULT_DATASET_DIR_CMAKE}"
)

target_compile_options(raster
  PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
    $<$<AND:$<PLATFORM_ID:Linux>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>>:-march=native>
)

target_include_directories(raster
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
)

# stdc++fs: std::filesystem lives in a separate library on GCC < 9.
target_link_libraries(raster
  PRIVATE
    Threads::Threads
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:stdc++fs>
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${RASTER_SOURCES} ${RASTER_HEADERS})
