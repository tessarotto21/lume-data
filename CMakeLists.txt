# SPDX-License-Identifier: AGPL-3.0-only
cmake_minimum_required(VERSION 3.20)
project(lume VERSION 0.1.7 LANGUAGES C)   # the single source of the version (ABOUT-007)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(SQLite3 REQUIRED)

file(GLOB LUME_SOURCES CONFIGURE_DEPENDS src/*.c)
if(APPLE)
  enable_language(OBJC)
  list(APPEND LUME_SOURCES src/macos_app.m)   # native app menu: About routed to Lume's About
endif()
add_executable(lume ${LUME_SOURCES})

target_include_directories(lume PRIVATE src third_party)
target_link_libraries(lume PRIVATE SDL3::SDL3 SQLite::SQLite3 ${CMAKE_DL_LIBS})
target_compile_definitions(lume PRIVATE LUME_SOURCE_ASSETS="${CMAKE_SOURCE_DIR}/assets")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(lume PRIVATE _GNU_SOURCE)   # nftw, strcasestr, memmem... (glibc hides them otherwise)
  target_link_options(lume PRIVATE -rdynamic)   # function names in crash stacks (backtrace_symbols)
endif()

# Product and build metadata (About, Copy Info, log). Nothing here is invented: what is not
# configured stays empty and the About says so (ABOUT-007..009, 014..021, 024).
set(LUME_CHANNEL "Development" CACHE STRING "Release channel: Development, Early Preview, Beta or Stable")
set(LUME_BUILD_ID "" CACHE STRING "Build identifier (commit or build number); empty = taken from git when the tree is a repository")
set(LUME_WEBSITE_URL "https://lume-data.org" CACHE STRING "Official website (https)")
set(LUME_DOCS_URL "https://lume-data.org/docs" CACHE STRING "Documentation (https)")
set(LUME_ISSUES_URL "https://github.com/tessarotto21/lume-data/issues/new/choose" CACHE STRING "Issue tracker / support (https)")
set(LUME_RELEASE_NOTES_URL "https://github.com/tessarotto21/lume-data/releases" CACHE STRING "Release notes (https)")
set(LUME_LICENSE_NAME "GNU AGPL-3.0-only" CACHE STRING "Lume's own license (LICENSE, LICENSING.md)")
set(LUME_COPYRIGHT "Copyright (C) 2026 Isabella Tessarotto" CACHE STRING "Copyright notice")
set(_lume_build_id "${LUME_BUILD_ID}")
if(_lume_build_id STREQUAL "")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE _lume_build_id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif()
endif()
string(TIMESTAMP _lume_configured "%Y-%m-%d %H:%M UTC" UTC)
target_compile_definitions(lume PRIVATE
  LUME_VERSION="${PROJECT_VERSION}" LUME_CHANNEL="${LUME_CHANNEL}" LUME_BUILD_ID="${_lume_build_id}"
  LUME_CONFIGURED_UTC="${_lume_configured}"
  LUME_WEBSITE_URL="${LUME_WEBSITE_URL}" LUME_DOCS_URL="${LUME_DOCS_URL}" LUME_ISSUES_URL="${LUME_ISSUES_URL}"
  LUME_RELEASE_NOTES_URL="${LUME_RELEASE_NOTES_URL}" LUME_LICENSE_NAME="${LUME_LICENSE_NAME}" LUME_COPYRIGHT="${LUME_COPYRIGHT}")

if(WIN32)
  if(NOT MSVC)
    target_compile_options(lume PRIVATE -include ${CMAKE_SOURCE_DIR}/src/compat_win.h)   # POSIX pieces Windows lacks
  endif()
  target_link_libraries(lume PRIVATE psapi advapi32 ws2_32 winhttp bcrypt shell32)   # Credential Manager, sockets, HTTPS (http_win.c), random, opening URLs
endif()
if(MSVC)
  target_compile_options(lume PRIVATE /W4)
else()
  target_compile_options(lume PRIVATE -Wall -Wextra -Wno-unused-parameter)
  set_source_files_properties(src/font.c PROPERTIES COMPILE_OPTIONS -Wno-unused-function)
  target_link_libraries(lume PRIVATE m)
endif()

if(APPLE)
  target_link_libraries(lume PRIVATE "-framework Cocoa" "-framework Security")   # Security: the Keychain (secstore.c)
  # Code signing with a fixed identity, so the Keychain recognises Lume across rebuilds ("Always Allow"
  # sticks). Automatic choice: an Apple "Developer ID Application" identity when the Keychain has one
  # (a Team ID: the Keychain trusts every build of the same team, BL-053), else the local self-signed
  # "Lume Local Dev" certificate, else an ad-hoc signature. LUME_CODESIGN_IDENTITY overrides the choice.
  set(LUME_CODESIGN_IDENTITY "" CACHE STRING "codesign identity for the lume binary; empty = Developer ID, then Lume Local Dev, when present")
  set(_lume_sign "${LUME_CODESIGN_IDENTITY}")
  if(CMAKE_C_FLAGS MATCHES "-fsanitize")
    set(_lume_sign "-")   # sanitizer builds: dyld refuses the sanitizer runtime in a team-signed process
  elseif(_lume_sign STREQUAL "")
    execute_process(COMMAND security find-identity -v -p codesigning OUTPUT_VARIABLE _ids ERROR_QUIET)
    if(_ids MATCHES "\"(Developer ID Application: [^\"]+)\"")
      set(_lume_sign "${CMAKE_MATCH_1}")
    elseif(_ids MATCHES "\"Lume Local Dev\"")
      set(_lume_sign "Lume Local Dev")
    endif()
  endif()
  if(NOT _lume_sign STREQUAL "")
    add_custom_command(TARGET lume POST_BUILD
      COMMAND xattr -c $<TARGET_FILE:lume>   # extended attributes ("detritus") make codesign refuse
      COMMAND codesign --force --sign "${_lume_sign}" --identifier dev.lume $<TARGET_FILE:lume>
      COMMENT "Signing lume with ${_lume_sign}" VERBATIM)
  endif()
  # "Lume Data.app" (BL-022): the same binary, Info.plist with the project version, the brand
  # .icns and the assets in Resources (SDL_GetBasePath() points there inside a bundle).
  set(LUME_APP "${CMAKE_BINARY_DIR}/Lume Data.app")
  configure_file(${CMAKE_SOURCE_DIR}/cmake/Info.plist.in ${CMAKE_BINARY_DIR}/bundle/Info.plist @ONLY)   # not next to the binary: codesign would take build/ for a bundle
  add_custom_target(lume_app ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LUME_APP}/Contents/MacOS" "${LUME_APP}/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:lume> "${LUME_APP}/Contents/MacOS/lume"
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/bundle/Info.plist "${LUME_APP}/Contents/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/assets/brand/lume-data.icns "${LUME_APP}/Contents/Resources/lume-data.icns"
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets "${LUME_APP}/Contents/Resources/assets"
    DEPENDS lume
    COMMENT "Assembling Lume Data.app")
endif()

# Ship the assets next to the binary so the app also works outside the source tree.
add_custom_command(TARGET lume POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:lume>/assets)
