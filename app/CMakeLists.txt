cmake_minimum_required(VERSION 3.12)
project(CommanderDesktopDemo LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "Use the Windows MSVC x86 toolchain for StarCraft 1.16.1")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "StarCraft/BWAPI require an x86 build; use the x86 Native Tools prompt")
endif()
set(STARCRAFT_LAB "E:/StarCraftAI" CACHE PATH "Local game experiment directory")
set(BWAPI_ROOT "${STARCRAFT_LAB}/dependencies/bwapi" CACHE PATH "BWAPI include and lib directory")
set(BWEM_ROOT "${STARCRAFT_LAB}/dependencies/strategy-libs/bwem/BWEM" CACHE PATH "BWEM include and src directory")
file(GLOB BWEM_SOURCES "${BWEM_ROOT}/src/*.cpp")

add_library(CommanderLayer SHARED CommanderLayer.cpp Candidate.def ${BWEM_SOURCES})
target_compile_definitions(CommanderLayer PRIVATE NOMINMAX WIN32 NDEBUG _CRT_SECURE_NO_WARNINGS)
target_compile_options(CommanderLayer PRIVATE /bigobj)
target_include_directories(CommanderLayer PRIVATE "${BWAPI_ROOT}/include" "${BWEM_ROOT}/include")
target_link_libraries(CommanderLayer "${BWAPI_ROOT}/lib/BWAPI.lib" user32)

add_library(ReplayControl SHARED ReplayControl.cpp Candidate.def)
target_compile_definitions(ReplayControl PRIVATE NOMINMAX WIN32 NDEBUG)
target_include_directories(ReplayControl PRIVATE "${BWAPI_ROOT}/include")
target_link_libraries(ReplayControl "${BWAPI_ROOT}/lib/BWAPI.lib")

add_library(LegacyStorm SHARED LegacyStorm.cpp LegacyStorm.def)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/StormBase.lib"
  COMMAND "${CMAKE_AR}" /nologo /machine:x86
    "/def:${CMAKE_CURRENT_SOURCE_DIR}/LegacyStormImport.def"
    "/out:${CMAKE_CURRENT_BINARY_DIR}/StormBase.lib"
  DEPENDS LegacyStormImport.def VERBATIM)
add_custom_target(StormBaseImport DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/StormBase.lib")
add_dependencies(LegacyStorm StormBaseImport)
target_link_libraries(LegacyStorm "${CMAKE_CURRENT_BINARY_DIR}/StormBase.lib")

add_library(ResultsWatcher SHARED ResultsWatcher.cpp ResultsWatcher.def)
target_compile_definitions(ResultsWatcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
