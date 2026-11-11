cmake_minimum_required(VERSION 3.20)
project(libclip VERSION 1.0.0 LANGUAGES C)

# Builds a static library by default; configure with -DBUILD_SHARED_LIBS=ON
# for a DLL / shared object.  Tests, examples and install rules default to on
# only when libclip is the top-level project, so add_subdirectory() from a
# host application builds just the library.
option(LIBCLIP_BUILD_TESTS "Build the libclip unit tests" ${PROJECT_IS_TOP_LEVEL})
option(LIBCLIP_BUILD_EXAMPLES "Build the libclip example programs" ${PROJECT_IS_TOP_LEVEL})
option(LIBCLIP_INSTALL "Generate libclip install rules" ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- SQLite (public domain amalgamation) -----------------------------------
if(NOT TARGET libclip_sqlite3)
  add_library(libclip_sqlite3 STATIC third_party/sqlite/sqlite3.c)
  target_include_directories(libclip_sqlite3 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite>)
  target_compile_definitions(libclip_sqlite3 PRIVATE
    SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED
    SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0)
  set_target_properties(libclip_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
  if(MSVC)
    target_compile_options(libclip_sqlite3 PRIVATE /w)
  endif()
endif()

# ---- libclip ------------------------------------------------------------------
add_library(clip
  src/clip_util.c
  src/clip_zlib.c
  src/clip_png.c
  src/clip_container.c
  src/clip_offscreen.c
  src/clip_document.c
  src/clip_writer.c)
add_library(libclip::clip ALIAS clip)
target_include_directories(clip PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include> PRIVATE src)
target_link_libraries(clip PRIVATE libclip_sqlite3)
set_target_properties(clip PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF C_VISIBILITY_PRESET hidden VERSION ${PROJECT_VERSION})
target_compile_definitions(clip PRIVATE CLIP_BUILDING)
if(BUILD_SHARED_LIBS)
  target_compile_definitions(clip PUBLIC CLIP_SHARED)
endif()
if(MSVC)
  target_compile_options(clip PRIVATE /W4 /utf-8)
  target_compile_definitions(clip PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(clip PRIVATE -Wall -Wextra)
endif()

# ---- tests --------------------------------------------------------------------
if(LIBCLIP_BUILD_TESTS)
  enable_testing()
  add_executable(libclip_tests tests/test_libclip.c)
  target_link_libraries(libclip_tests PRIVATE libclip::clip)
  set_target_properties(libclip_tests PROPERTIES C_STANDARD 99)
  if(MSVC)
    target_compile_definitions(libclip_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  # Optional: a .clip saved by Clip Studio Paint.  When set, the tests also
  # check that libclip reads it and that a round trip of it reads back.
  set(LIBCLIP_REFERENCE_FILE "" CACHE FILEPATH "CSP-written .clip used by the tests (optional)")
  if(LIBCLIP_REFERENCE_FILE)
    add_test(NAME libclip COMMAND libclip_tests "${LIBCLIP_REFERENCE_FILE}")
  else()
    add_test(NAME libclip COMMAND libclip_tests)
  endif()
endif()

# ---- examples -----------------------------------------------------------------
if(LIBCLIP_BUILD_EXAMPLES)
  foreach(example clipinfo clipnew)
    add_executable(${example} examples/${example}.c)
    target_link_libraries(${example} PRIVATE libclip::clip)
    set_target_properties(${example} PROPERTIES C_STANDARD 99)
    if(MSVC)
      target_compile_definitions(${example} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
  endforeach()
  if(WIN32)
    target_link_libraries(clipinfo PRIVATE shell32)  # CommandLineToArgvW
  endif()
endif()

# ---- install / find_package(libclip) ------------------------------------------
if(LIBCLIP_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set_target_properties(clip PROPERTIES EXPORT_NAME clip)
  # A static libclip needs the bundled SQLite at link time, so it is
  # installed alongside; a shared libclip contains it.
  if(BUILD_SHARED_LIBS)
    set(_libclip_targets clip)
  else()
    set(_libclip_targets clip libclip_sqlite3)
  endif()
  install(TARGETS ${_libclip_targets} EXPORT libclipTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/libclip DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/libclip)
  install(EXPORT libclipTargets NAMESPACE libclip:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libclip)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/libclipConfig.cmake
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/libclipTargets.cmake\")
")
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/libclipConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libclipConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/libclipConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libclip)
endif()
