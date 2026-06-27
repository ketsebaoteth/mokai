cmake_minimum_required(VERSION 3.20)
project(mokai_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "src/*.hpp")
list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/entery.cpp")

add_library(mokai_core STATIC ${SOURCES} ${HEADERS})

target_include_directories(mokai_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(mokai src/entery.cpp)
target_link_libraries(mokai PRIVATE mokai_core)

if(NOT MSVC)
  target_link_options(mokai PRIVATE $<$<CONFIG:Release>:-s>)
else()
  target_link_options(mokai PRIVATE $<$<CONFIG:Release>:/RELEASE;/OPT:REF;/OPT:ICF>)
endif()
