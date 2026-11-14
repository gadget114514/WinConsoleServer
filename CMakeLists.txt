cmake_minimum_required(VERSION 3.20)
project(WinConsoleServer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/utf-8 /W3)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

add_compile_definitions(
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  ECODE_ENABLE_SIXEL
)

set(OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin")

# ---------------------------------------------------------------------------
# Protocol
# ---------------------------------------------------------------------------
add_library(wcs_protocol STATIC
  src/common/protocol.cpp
  src/common/frame_reader.cpp
)
target_include_directories(wcs_protocol PUBLIC src/common)

# ---------------------------------------------------------------------------
# ConPTY wrapper (server + local terminal)
# ---------------------------------------------------------------------------
add_library(wcs_conpty STATIC
  src/common/conpty_session.cpp
)
target_include_directories(wcs_conpty PUBLIC src/common)

# ---------------------------------------------------------------------------
# libsixel (vendored source, built with the same runtime as our binaries)
# ---------------------------------------------------------------------------
set(LIBSIXEL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsixel/src")
add_library(libsixel STATIC
  "${LIBSIXEL_SRC}/output.c"
  "${LIBSIXEL_SRC}/fromsixel.c"
  "${LIBSIXEL_SRC}/tosixel.c"
  "${LIBSIXEL_SRC}/quant.c"
  "${LIBSIXEL_SRC}/dither.c"
  "${LIBSIXEL_SRC}/frame.c"
  "${LIBSIXEL_SRC}/pixelformat.c"
  "${LIBSIXEL_SRC}/scale.c"
  "${LIBSIXEL_SRC}/chunk.c"
  "${LIBSIXEL_SRC}/loader.c"
  "${LIBSIXEL_SRC}/frompnm.c"
  "${LIBSIXEL_SRC}/fromgif.c"
  "${LIBSIXEL_SRC}/encoder.c"
  "${LIBSIXEL_SRC}/decoder.c"
  "${LIBSIXEL_SRC}/stb_image_write.c"
  "${LIBSIXEL_SRC}/writer.c"
  "${LIBSIXEL_SRC}/status.c"
  "${LIBSIXEL_SRC}/malloc_stub.c"
  "${LIBSIXEL_SRC}/allocator.c"
  "${LIBSIXEL_SRC}/tty.c"
)
target_include_directories(libsixel
  PRIVATE "${LIBSIXEL_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsixel"
  PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsixel"
)
target_compile_definitions(libsixel PRIVATE $<$<CONFIG:Debug>:HAVE_DEBUG>)

# ---------------------------------------------------------------------------
# Ecode terminal components (copied from D:/wsgit/Ecode/Application/Terminal)
# ---------------------------------------------------------------------------
add_library(ecode_terminal STATIC
  src/client/terminal/src/TerminalBuffer.cpp
  src/client/terminal/src/TerminalEmulator.cpp
  src/client/terminal/src/TerminalView.cpp
  src/client/terminal/src/ImageManager.cpp
)
target_include_directories(ecode_terminal PUBLIC
  src/client/terminal/include
  src/client
)
target_link_libraries(ecode_terminal PUBLIC
  d2d1
  dwrite
  windowscodecs
  imm32
  user32
  gdi32
  shell32
  shlwapi
  comctl32
  ole32
  libsixel
)

# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------
add_executable(WinConsoleServer
  src/server/main.cpp
  src/server/server.cpp
  src/server/session.cpp
  src/server/session_hub.cpp
  src/server/server_config.cpp
  src/server/terminal_job.cpp
  src/server/windows_service.cpp
  src/server/app.manifest
)
target_include_directories(WinConsoleServer PRIVATE src/server src/common)
target_link_libraries(WinConsoleServer PRIVATE wcs_protocol wcs_conpty ws2_32)
set_target_properties(WinConsoleServer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}/$<CONFIG>"
)

# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------
add_executable(WinConsoleTerminal WIN32
  src/client/main.cpp
  src/client/local_session.cpp
  src/client/remote_session.cpp
  src/client/client.rc
  src/client/app.manifest
)
target_include_directories(WinConsoleTerminal PRIVATE
  src/client
  src/client/terminal/include
  src/common
)
target_link_libraries(WinConsoleTerminal PRIVATE
  wcs_protocol
  wcs_conpty
  ecode_terminal
  ws2_32
  comctl32
  dwmapi
  shell32
  shlwapi
  ole32
)
set_target_properties(WinConsoleTerminal PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}/$<CONFIG>"
)

# ---------------------------------------------------------------------------
# wct: console (CUI) WCSP terminal client
# ---------------------------------------------------------------------------
add_executable(wct
  src/client/wct_main.cpp
  src/client/wct_client.cpp
  src/client/remote_session.cpp
)
target_include_directories(wct PRIVATE src/client src/common)
target_link_libraries(wct PRIVATE wcs_protocol ws2_32)
set_target_properties(wct PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}/$<CONFIG>"
)

# ---------------------------------------------------------------------------
# wcs-cli: session administration (list / delete)
# ---------------------------------------------------------------------------
add_executable(wcs-cli
  src/client/wcs_cli_main.cpp
  src/client/remote_session.cpp
)
target_include_directories(wcs-cli PRIVATE src/client src/common)
target_link_libraries(wcs-cli PRIVATE wcs_protocol ws2_32)
set_target_properties(wcs-cli PROPERTIES
  OUTPUT_NAME "wcs-cli"
  RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}/$<CONFIG>"
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE wcs_protocol)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_frame_reader tests/test_frame_reader.cpp)
target_link_libraries(test_frame_reader PRIVATE wcs_protocol)
add_test(NAME frame_reader COMMAND test_frame_reader)

set_target_properties(test_protocol test_frame_reader PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}/$<CONFIG>/tests"
)

# ---------------------------------------------------------------------------
# Runtime files
# ---------------------------------------------------------------------------
add_custom_command(TARGET WinConsoleServer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/conpty/conpty.dll"
    "$<TARGET_FILE_DIR:WinConsoleServer>/conpty.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/conpty/OpenConsole.exe"
    "$<TARGET_FILE_DIR:WinConsoleServer>/OpenConsole.exe"
)

add_custom_command(TARGET WinConsoleTerminal POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/conpty/conpty.dll"
    "$<TARGET_FILE_DIR:WinConsoleTerminal>/conpty.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/conpty/OpenConsole.exe"
    "$<TARGET_FILE_DIR:WinConsoleTerminal>/OpenConsole.exe"
)
