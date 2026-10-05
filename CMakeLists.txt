cmake_minimum_required(VERSION 3.16)
project(k7zx VERSION 5.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(K7ZX_BUILD_GUI   "Build the GTK 3 graphical interface" ON)
option(K7ZX_BUILD_TESTS "Build the self tests"            ON)
option(K7ZX_WERROR      "Treat warnings as errors"        OFF)

find_package(Python3 COMPONENTS Interpreter)

# ---------------------------------------------------------------------------
# The conversion engine: platform independent, no GUI dependencies.
# ---------------------------------------------------------------------------
add_library(k7zx_core STATIC
  src/core/byteorder.h
  src/core/convert.cpp
  src/core/convert.h
  src/core/defs.h
  src/core/loader.h
  src/core/loader_data.cpp
  src/core/lz.cpp
  src/core/lz.h
  src/core/mp3.cpp
  src/core/mp3.h
  src/core/rayo.cpp
  src/core/rayo.h
  src/core/rayo_loader.inc
  src/core/settings.cpp
  src/core/settings.h
  src/core/texts.cpp
  src/core/texts.h
  src/core/zxcode.cpp
  src/core/zxcode.h
  src/core/zxfiles.cpp
  src/core/zxfiles.h
  src/core/zxwav.cpp
  src/core/zxwav.h
)
target_include_directories(k7zx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
  target_compile_options(k7zx_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(k7zx_core PRIVATE -Wall -Wextra)
  if(K7ZX_WERROR)
    target_compile_options(k7zx_core PRIVATE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Command line front end
# ---------------------------------------------------------------------------
add_executable(k7zx-cli src/cli/main.cpp)
target_link_libraries(k7zx-cli PRIVATE k7zx_core)
if(NOT MSVC)
  target_compile_options(k7zx-cli PRIVATE -Wall -Wextra)
  if(K7ZX_WERROR)
    target_compile_options(k7zx-cli PRIVATE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Graphical front end
# ---------------------------------------------------------------------------
if(K7ZX_BUILD_GUI)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-3.0)

  add_executable(k7zx
    src/gui/main.cpp
    src/gui/mainwindow.cpp
    src/gui/mainwindow.h
    src/gui/batchdialog.cpp
    src/gui/batchdialog.h
    src/gui/player.cpp
    src/gui/player.h
  )
  target_link_libraries(k7zx PRIVATE k7zx_core PkgConfig::GTKMM)
  if(NOT MSVC)
    target_compile_options(k7zx PRIVATE -Wall -Wextra)
  if(K7ZX_WERROR)
    target_compile_options(k7zx PRIVATE -Werror)
  endif()
  endif()

  install(TARGETS k7zx RUNTIME DESTINATION bin)
endif()

install(TARGETS k7zx-cli RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(K7ZX_BUILD_TESTS)
  enable_testing()

  add_executable(k7zx-tests
    tests/test_main.cpp
    tests/test_routines.cpp
    tests/test_zxfiles.cpp
    tests/test_fuzz.cpp
    tests/test_zxwav.cpp
    tests/test_zxcode.cpp
    tests/test_rayo.cpp
  )
  target_link_libraries(k7zx-tests PRIVATE k7zx_core)
  target_include_directories(k7zx-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  if(NOT MSVC)
    target_compile_options(k7zx-tests PRIVATE -Wall -Wextra)
  if(K7ZX_WERROR)
    target_compile_options(k7zx-tests PRIVATE -Werror)
  endif()
  endif()

  set(K7ZX_TEST_DATA ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)

  # Regenerate the synthetic tape / snapshot fixtures.  Use the interpreter
  # find_package() located rather than a bare "python3", which is not on PATH by
  # that name in every environment CMake builds in.
  if(Python3_Interpreter_FOUND OR PYTHON3_FOUND)
    add_test(NAME k7zx-testdata
             COMMAND ${CMAKE_COMMAND} -E env ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_testdata.py ${K7ZX_TEST_DATA})
  endif()

  add_test(NAME k7zx-tests
           COMMAND k7zx-tests --data-dir ${K7ZX_TEST_DATA})
  set_tests_properties(k7zx-tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(k7zx-tests PROPERTIES DEPENDS k7zx-testdata)

  if(Python3_Interpreter_FOUND OR PYTHON3_FOUND)
    # Every finding the guide and the README publish -- the per-speed ratings,
    # the bit rates, the counts -- is re-derived from the source here and the
    # test fails if any of it has drifted.  A conclusion nobody re-checks stops
    # being true without anyone noticing.
    add_test(NAME k7zx-findings
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_findings.py
                     ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(k7zx-findings PROPERTIES
                         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    add_test(NAME k7zx-roundtrip
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/roundtrip_tap.py
                     $<TARGET_FILE:k7zx-cli> ${K7ZX_TEST_DATA}/test.tap)
    # It reads tests/data/test.tap, so it needs the generator to have run.  With
    # ctest -j on a clean tree it started in parallel and failed.
    set_tests_properties(k7zx-roundtrip PROPERTIES DEPENDS k7zx-testdata)

    # What the command line says it does, as opposed to what the tape does.
    add_test(NAME k7zx-cli
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_behaviour.py
                     $<TARGET_FILE:k7zx-cli> ${K7ZX_TEST_DATA})
    set_tests_properties(k7zx-cli PROPERTIES DEPENDS k7zx-testdata)
  endif()
endif()
