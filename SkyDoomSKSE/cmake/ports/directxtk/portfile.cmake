vcpkg_from_github(
    OUT_SOURCE_PATH
    SOURCE_PATH
    REPO
    Microsoft/DirectXTK
    REF
    feb2024
    SHA512
    272303b8de9484159312c06087caf93016f96795facd9090ff4699347e96bc5ff80a177424b044099c19a93c4cbaf44684cc79abba43f1d8b4f530c6a47998a9
    HEAD_REF
    main)

# Headers only (shader compilation requires fxc.exe, Windows-only)
file(INSTALL "${SOURCE_PATH}/Inc/" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

# Create header-only CMake target for find_package(directxtk CONFIG REQUIRED)
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/share/directxtk")

file(
    WRITE "${CURRENT_PACKAGES_DIR}/share/directxtk/directxtkTargets.cmake"
    [[
if(NOT TARGET Microsoft::DirectXTK)
    add_library(Microsoft::DirectXTK INTERFACE IMPORTED GLOBAL)
    set_target_properties(Microsoft::DirectXTK PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
    )
endif()
]])

file(WRITE "${CURRENT_PACKAGES_DIR}/share/directxtk/directxtkConfig.cmake"
     "include(\"\${CMAKE_CURRENT_LIST_DIR}/directxtkTargets.cmake\")\n")

file(
    INSTALL "${SOURCE_PATH}/LICENSE"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
    RENAME copyright)
