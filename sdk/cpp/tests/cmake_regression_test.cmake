cmake_minimum_required(VERSION 3.28)
if(NOT DEFINED CPP_ROOT OR NOT DEFINED WORK_ROOT OR NOT DEFINED GENERATOR)
    message(FATAL_ERROR "CPP_ROOT, WORK_ROOT and GENERATOR are required")
endif()

# 每次使用独立目录，避免删除或改写源码树和已有构建。
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
set(fixture "${WORK_ROOT}/cmake-${suffix}")
set(source "${fixture}/source with spaces")
set(destination "${fixture}/destination with spaces")
file(MAKE_DIRECTORY "${source}" "${destination}")
file(WRITE "${source}/one.dll" "first fixture")
file(WRITE "${source}/two.dll" "second fixture")
foreach(dlls IN ITEMS "" "${source}/one.dll" "${source}/one.dll;${source}/two.dll")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DVERDANDI_RUNTIME_DLLS=${dlls}"
        "-DVERDANDI_RUNTIME_DIRECTORY=${destination}" -P "${CPP_ROOT}/cmake/CopyRuntimeDlls.cmake"
        COMMAND_ERROR_IS_FATAL ANY)
    foreach(dll IN LISTS dlls)
        get_filename_component(name "${dll}" NAME)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${dll}" "${destination}/${name}" COMMAND_ERROR_IS_FATAL ANY)
    endforeach()
endforeach()

file(MAKE_DIRECTORY "${fixture}/project/src")
file(COPY "${CPP_ROOT}/src/protocol" "${CPP_ROOT}/src/protocol.cpp.in" DESTINATION "${fixture}/project/src")
file(WRITE "${fixture}/project/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.28)\nproject(embed_regression LANGUAGES NONE)\n"
    "include(\"${CPP_ROOT}/cmake/EmbedProtocol.cmake\")\nverdandi_embed_protocol(generated)\nadd_custom_target(probe ALL)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${fixture}/project" -B "${fixture}/build" -G "${GENERATOR}" COMMAND_ERROR_IS_FATAL ANY)
file(SHA256 "${fixture}/build/generated/protocol.cpp" before)
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1 COMMAND_ERROR_IS_FATAL ANY)
file(APPEND "${fixture}/project/src/protocol/catalog/patch.lua" "\n-- incremental-build-regression\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${fixture}/build" --config Release COMMAND_ERROR_IS_FATAL ANY)
file(SHA256 "${fixture}/build/generated/protocol.cpp" after)
if(before STREQUAL after)
    message(FATAL_ERROR "Changing Lua did not update the embedded protocol during build")
endif()
