cmake_minimum_required(VERSION 3.22)
project(phonewebcam_native)

# libsrtp configuration
# Force internal crypto to avoid OpenSSL dependency on Android
set(CRYPTO_LIBRARY "internal" CACHE STRING "" FORCE)
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(external/libsrtp)

add_library(turbojpeg STATIC IMPORTED)
set_target_properties(turbojpeg PROPERTIES
    IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/libjpeg/${ANDROID_ABI}/libturbojpeg.a
    INTERFACE_INCLUDE_DIRECTORIES
        ${CMAKE_SOURCE_DIR}/libjpeg/include
)

add_library(phonewebcam_jni SHARED
    jpeg_encoder_jni.cpp
    srtp_jni.cpp
)

# Link against srtp3 (the target name from the submodule's CMakeLists.txt)
target_link_libraries(phonewebcam_jni
    srtp3
    turbojpeg
    android
    log
)