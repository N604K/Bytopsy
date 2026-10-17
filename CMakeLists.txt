cmake_minimum_required(VERSION 3.20)
project(WinNCDU VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: self-contained .exe files with no VC++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Shared engine: scanning, tree model, file operations, analysis, import/export.
add_library(winncdu_core STATIC
  src/scanner.cpp
  src/tree.cpp
  src/fsops.cpp
  src/analysis.cpp
  src/export.cpp
  src/util.cpp
  src/protect.cpp
  src/journal.cpp
)
target_include_directories(winncdu_core PUBLIC src)
target_compile_definitions(winncdu_core PUBLIC UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(winncdu_core PUBLIC shell32 ole32 shlwapi bcrypt user32 advapi32 rstrtmgr ntdll)

# Windowed app (Direct2D): build\winncdu.exe
add_executable(winncdu WIN32
  src/gui/main_gui.cpp
  src/gui/app.cpp
  src/gui/actions.cpp
  src/gui/draw.cpp
  src/gui/gfx.cpp
  src/gui/icons.cpp
  src/gui/treemap.cpp
  src/gui/history.cpp
  src/gui/cleanup.cpp
  src/gui/platform.cpp
  src/gui/winncdu-gui.rc
)
target_link_libraries(winncdu PRIVATE winncdu_core d2d1 dwrite windowscodecs dwmapi comctl32 uxtheme gdi32 advapi32)

# Terminal app (ncdu-style TUI): build\winncdu-cli.exe
add_executable(winncdu-cli
  src/main.cpp
  src/ui.cpp
  src/console.cpp
  src/winncdu.rc
)
target_link_libraries(winncdu-cli PRIVATE winncdu_core)

foreach(t winncdu_core winncdu winncdu-cli)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W4 /utf-8 /permissive- /EHsc /MP)
  endif()
endforeach()
if(MSVC)
  # Big main-thread stack for deep folder trees; manifests are embedded via the .rc files.
  target_link_options(winncdu PRIVATE /STACK:16777216 /MANIFEST:NO)
  target_link_options(winncdu-cli PRIVATE /STACK:16777216 /MANIFEST:NO)
endif()

# Self-check of the protection rules: build\winncdu-protect-test.exe (also run by `ctest`).
add_executable(winncdu-protect-test tests/protect_test.cpp)
target_link_libraries(winncdu-protect-test PRIVATE winncdu_core)
if(MSVC)
  target_compile_options(winncdu-protect-test PRIVATE /W4 /utf-8 /permissive- /EHsc)
endif()
enable_testing()
add_test(NAME protect COMMAND winncdu-protect-test)

# Deletion journal and Recycle Bin restore: build\winncdu-journal-test.exe (also run by `ctest`).
add_executable(winncdu-journal-test tests/journal_test.cpp)
target_link_libraries(winncdu-journal-test PRIVATE winncdu_core)
if(MSVC)
  target_compile_options(winncdu-journal-test PRIVATE /W4 /utf-8 /permissive- /EHsc)
endif()
add_test(NAME journal COMMAND winncdu-journal-test)

# Cleanup preview semantics (kept entries survive, changed entries are skipped): build\winncdu-cleanup-test.exe
add_executable(winncdu-cleanup-test tests/cleanup_test.cpp src/gui/cleanup.cpp)
target_link_libraries(winncdu-cleanup-test PRIVATE winncdu_core)
target_compile_definitions(winncdu-cleanup-test PRIVATE WINNCDU_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}")
if(MSVC)
  target_compile_options(winncdu-cleanup-test PRIVATE /W4 /utf-8 /permissive- /EHsc)
endif()
add_test(NAME cleanup COMMAND winncdu-cleanup-test)

# Permanent deletion (links, read-only, locked files, long paths): build\winncdu-delete-test.exe
add_executable(winncdu-delete-test tests/delete_test.cpp)
target_link_libraries(winncdu-delete-test PRIVATE winncdu_core)
if(MSVC)
  target_compile_options(winncdu-delete-test PRIVATE /W4 /utf-8 /permissive- /EHsc)
endif()
add_test(NAME delete COMMAND winncdu-delete-test)

install(TARGETS winncdu winncdu-cli RUNTIME DESTINATION bin)
