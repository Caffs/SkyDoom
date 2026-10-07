# Skip full link tests (lld-link fails the vs_link_exe check on Linux)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# xwin splat output directory (run: xwin splat --output ~/.xwin)
set(XWIN_DIR
    "$ENV{HOME}/.xwin"
    CACHE PATH "xwin splat output directory")

find_program(CMAKE_LINKER NAMES lld-link REQUIRED)
find_program(CMAKE_C_COMPILER NAMES clang-cl REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang-cl REQUIRED)
find_program(CMAKE_AR NAMES llvm-lib REQUIRED)
find_program(CMAKE_MT NAMES llvm-mt REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES llvm-rc REQUIRED)

set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# Use release CRT (/MD) to match Skyrim's runtime (xwin doesn't ship msvcrtd.lib)
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreadedDLL"
    CACHE STRING "")

# Windows SDK and MSVC CRT include paths (/imsvc suppresses warnings from system headers)
set(_xwin_includes "/imsvc\"${XWIN_DIR}/crt/include\"" "/imsvc\"${XWIN_DIR}/sdk/include/ucrt\""
                   "/imsvc\"${XWIN_DIR}/sdk/include/um\"" "/imsvc\"${XWIN_DIR}/sdk/include/shared\"")
string(JOIN " " _xwin_include_flags ${_xwin_includes})

set(CMAKE_C_FLAGS_INIT "${_xwin_include_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_xwin_include_flags}")

# Windows SDK and MSVC CRT library paths
set(_xwin_libflags "/libpath:\"${XWIN_DIR}/crt/lib/x86_64\"" "/libpath:\"${XWIN_DIR}/sdk/lib/um/x86_64\""
                   "/libpath:\"${XWIN_DIR}/sdk/lib/ucrt/x86_64\"" "/ignore:4099")
string(JOIN " " _xwin_lib_flags ${_xwin_libflags})

set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_lib_flags} /MANIFEST:NO")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_lib_flags} /MANIFEST:NO")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_lib_flags} /MANIFEST:NO")

# Search xwin for headers/libs, not programs
set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# lld-link is case-sensitive; xwin installs lowercase libs but CommonLibSSE-NG references
# mixed-case names (e.g. Advapi32.lib). Create TitleCase symlinks pointing to lowercase files.
foreach(_xwin_lib_dir "${XWIN_DIR}/crt/lib/x86_64" "${XWIN_DIR}/sdk/lib/um/x86_64" "${XWIN_DIR}/sdk/lib/ucrt/x86_64")
    file(GLOB _libs "${_xwin_lib_dir}/*.lib")
    foreach(_lib ${_libs})
        get_filename_component(_stem "${_lib}" NAME_WE)
        string(TOLOWER "${_stem}" _stem_lower)
        if(_stem STREQUAL _stem_lower)
            string(SUBSTRING "${_stem_lower}" 0 1 _first)
            string(TOUPPER "${_first}" _first_upper)
            string(SUBSTRING "${_stem_lower}" 1 -1 _rest)
            set(_title "${_first_upper}${_rest}")
            set(_dst "${_xwin_lib_dir}/${_title}.lib")
            if(NOT EXISTS "${_dst}")
                execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "${_lib}" "${_dst}"
                                RESULT_VARIABLE _symlink_result)
                if(NOT _symlink_result EQUAL 0)
                    message(FATAL_ERROR "Failed to create symlink: ${_dst} -> ${_lib}")
                endif()
            endif()
        endif()
    endforeach()
endforeach()
