# The Rust crate in src/p2p (Iroh), built by cargo through Corrosion
# (ADR 0010). It needs cargo on PATH; E3MAIL_P2P=OFF builds without sync.
include(FetchContent)
FetchContent_Declare(Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.6.1)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(MANIFEST_PATH ${PROJECT_SOURCE_DIR}/src/p2p/Cargo.toml
    CRATES e3p2p
    CRATE_TYPES staticlib
    LOCKED)
target_include_directories(e3p2p INTERFACE ${PROJECT_SOURCE_DIR}/src/p2p)

# macOS: what iroh's dependencies call in the system (network interfaces and
# proxies, the keychain's certificate checks). Corrosion adds only what the
# Rust standard library needs.
if(APPLE)
    target_link_libraries(e3p2p INTERFACE "-framework CoreFoundation" "-framework Security"
        "-framework SystemConfiguration")
endif()

# MSVC: name its linker. Under Git Bash, Git's own `link` comes first on PATH,
# and rustc would run it for the crate and its build scripts (same triple as
# the host, so one variable covers both). corrosion_set_linker is ignored
# with MSVC.
if(MSVC AND CMAKE_LINKER)
    string(TOUPPER "${Rust_CARGO_TARGET_CACHED}" e3_rust_target)
    string(REPLACE "-" "_" e3_rust_target "${e3_rust_target}")
    set(e3_msvc_linker "CARGO_TARGET_${e3_rust_target}_LINKER=${CMAKE_LINKER}")
    corrosion_set_env_vars(e3p2p "${e3_msvc_linker}")
endif()

# Android: link through the NDK's clang wrapper for this target and API
# level. Corrosion's plain clang lacks both, and a dependency that links a
# shared library of its own (iroh-relay) then cannot find liblog.
if(ANDROID)
    get_filename_component(e3_ndk_bin ${CMAKE_CXX_COMPILER} DIRECTORY)
    string(REGEX REPLACE "^android-" "" e3_android_api "${ANDROID_PLATFORM}")
    set(e3_android_linker ${e3_ndk_bin}/${Rust_CARGO_TARGET}${e3_android_api}-clang)
    if(NOT EXISTS ${e3_android_linker})
        message(FATAL_ERROR "No NDK linker at ${e3_android_linker}; set Rust_CARGO_TARGET")
    endif()
    corrosion_set_linker(e3p2p ${e3_android_linker})
endif()

# The crate's own tests: framing, the accept list, two endpoints on this
# machine without a relay.
if(E3MAIL_BUILD_TESTS AND NOT ANDROID AND NOT CMAKE_CROSSCOMPILING)
    find_program(E3MAIL_CARGO cargo HINTS $ENV{HOME}/.cargo/bin)
    if(E3MAIL_CARGO)
        add_test(NAME p2p_cargo
            COMMAND ${E3MAIL_CARGO} test --locked --manifest-path ${PROJECT_SOURCE_DIR}/src/p2p/Cargo.toml
                --target-dir ${CMAKE_BINARY_DIR}/p2p-test)
        if(e3_msvc_linker)
            set_tests_properties(p2p_cargo PROPERTIES ENVIRONMENT "${e3_msvc_linker}")
        endif()
    endif()
endif()
