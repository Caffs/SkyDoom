# x64 Windows static, clang-cl (MSVC ABI)

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
# Release-only deps — halves build time; plugin can still build in Debug
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Windows)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/clang-cl-cross.cmake")

set(VCPKG_ENV_PASSTHROUGH PATH HOME)
