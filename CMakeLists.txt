cmake_minimum_required(VERSION 3.24)
project(sd_app LANGUAGES CXX)

if(NOT EXISTS "${CMAKE_BINARY_DIR}/conan_provider.cmake")
    file(DOWNLOAD "https://raw.githubusercontent.com/conan-io/cmake-conan/c22bbf0af0b73d5f0def24a9cdf4ce503ae79e5d/conan_provider.cmake"
         "${CMAKE_BINARY_DIR}/conan_provider.cmake"
         EXPECTED_HASH SHA256=796E17C1240D7B2167388B9F68411208084A641ED25DF172D64D80EF92F8DA7A
         TLS_VERIFY ON)
endif()
list(APPEND CMAKE_PROJECT_TOP_LEVEL_INCLUDES ${CMAKE_BINARY_DIR}/conan_provider.cmake)

set(CMAKE_CXX_STANDARD 20)
set(SOURCE_DIR src)
set(INCLUDE_DIR include)
set(APP_DIR app)
set(TEST_DIR tests)

find_package(imgui CONFIG REQUIRED)
find_package(glfw3 REQUIRED)
find_package(stb REQUIRED)
find_package(spdlog REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

get_target_property(RAW_IMGUI_INCLUDES imgui::imgui INTERFACE_INCLUDE_DIRECTORIES)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    string(REGEX MATCH "\\$<\\$<CONFIG:[Dd]ebug>:[^>]+" IMGUI_PATH_MATCH "${RAW_IMGUI_INCLUDES}")
    string(REPLACE "$<$<CONFIG:Debug>:" "" IMGUI_PATH_MATCH "${IMGUI_PATH_MATCH}")
    string(REPLACE "$<$<CONFIG:debug>:" "" IMGUI_PATH_MATCH "${IMGUI_PATH_MATCH}")
    
    set(IMGUI_INCLUDE_DIR "${IMGUI_PATH_MATCH}")
else()
    string(REGEX MATCH "\\$<\\$<CONFIG:[Rr]elease>:[^>]+" IMGUI_PATH_MATCH "${RAW_IMGUI_INCLUDES}")
    string(REPLACE "$<$<CONFIG:Release>:" "" IMGUI_PATH_MATCH "${IMGUI_PATH_MATCH}")
    string(REPLACE "$<$<CONFIG:release>:" "" IMGUI_PATH_MATCH "${IMGUI_PATH_MATCH}")
    
    set(IMGUI_INCLUDE_DIR "${IMGUI_PATH_MATCH}")
endif()

add_executable(sd_app
    #    ${SOURCE_DIR}/img_load.cpp
    ${IMGUI_INCLUDE_DIR}/../res/bindings/imgui_impl_glfw.cpp
    ${IMGUI_INCLUDE_DIR}/../res/bindings/imgui_impl_opengl3.cpp
    ${APP_DIR}/sd_demo.cpp)
    
target_include_directories(sd_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${INCLUDE_DIR} ${IMGUI_INCLUDE_DIR}/../res)

target_link_libraries(sd_app PUBLIC stb::stb spdlog::spdlog)
target_link_libraries(sd_app PRIVATE CLI11::CLI11 imgui::imgui glfw)

find_package(GTest 1.16.0 CONFIG REQUIRED)
add_executable(sd_app_test
    ${TEST_DIR}/sd_app_test.cpp)
target_include_directories(sd_app_test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${INCLUDE_DIR})
target_link_libraries(sd_app_test PUBLIC stb::stb spdlog::spdlog)
target_link_libraries(sd_app_test PRIVATE GTest::gtest GTest::gtest_main)
target_link_libraries(sd_app_test PRIVATE CLI11::CLI11)

enable_testing()
include(GoogleTest)
gtest_discover_tests(sd_app_test)
