cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Dependency paths — override these on the cmake command line if your
# installs are in different locations:
#
#   cmake -DFFMPEG_ROOT="D:/libs/ffmpeg" -DOBS_ROOT="D:/obs-studio" ..
#
# Defaults point to the original developer's machine paths so the project
# builds out of the box without any changes for them.
# ---------------------------------------------------------------------------
if(WIN32)
    set(TURBOJPEG_ROOT "C:/libjpeg-turbo64"          CACHE PATH "Path to libjpeg-turbo install")
    set(SRTP_ROOT      "C:/libsrtp2"                 CACHE PATH "Path to libsrtp2 install")
    set(OPENSSL_ROOT   "C:/Program Files/OpenSSL-Win64" CACHE PATH "Path to OpenSSL install")
    set(FFMPEG_ROOT    "C:/ffmpeg"                   CACHE PATH "Path to FFmpeg install")
    set(OBS_ROOT       "C:/dev/obs-studio"           CACHE PATH "Path to OBS Studio source")

    # Derived paths — do not edit these, edit the roots above
    set(TURBOJPEG_INCLUDE_DIR  "${TURBOJPEG_ROOT}/include")
    set(TURBOJPEG_LIB          "${TURBOJPEG_ROOT}/lib/turbojpeg.lib")
    set(TURBOJPEG_DLL_JPEG     "${TURBOJPEG_ROOT}/bin/jpeg62.dll")
    set(TURBOJPEG_DLL_TURBO    "${TURBOJPEG_ROOT}/bin/turbojpeg.dll")

    set(SRTP_INCLUDE_DIR       "${SRTP_ROOT}/include")
    set(SRTP_LIB               "${SRTP_ROOT}/lib/srtp3.lib")
    set(SRTP_DLL               "${SRTP_ROOT}/bin/srtp3.dll")

    set(OPENSSL_ROOT_DIR       "${OPENSSL_ROOT}")
    set(OPENSSL_INCLUDE_DIR    "${OPENSSL_ROOT}/include")
    set(OPENSSL_CRYPTO_LIB     "${OPENSSL_ROOT}/lib/VC/x64/MD/libcrypto.lib")
    set(OPENSSL_CRYPTO_DLL     "${OPENSSL_ROOT}/bin/libcrypto-3-x64.dll")

    set(FFMPEG_INCLUDE_DIR     "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIB_AVCODEC     "${FFMPEG_ROOT}/lib/avcodec.lib")
    set(FFMPEG_LIB_AVUTIL      "${FFMPEG_ROOT}/lib/avutil.lib")
    set(FFMPEG_LIB_SWSCALE     "${FFMPEG_ROOT}/lib/swscale.lib")
    set(FFMPEG_LIB_SWRESAMPLE  "${FFMPEG_ROOT}/lib/swresample.lib")
    set(FFMPEG_DLL_AVCODEC     "${FFMPEG_ROOT}/bin/avcodec-62.dll")
    set(FFMPEG_DLL_AVUTIL      "${FFMPEG_ROOT}/bin/avutil-60.dll")
    set(FFMPEG_DLL_SWSCALE     "${FFMPEG_ROOT}/bin/swscale-9.dll")
    set(FFMPEG_DLL_SWRESAMPLE  "${FFMPEG_ROOT}/bin/swresample-6.dll")

    set(OBS_INCLUDE_DIR        "${OBS_ROOT}/libobs")
    set(OBS_BUILD_DIR          "${OBS_ROOT}/build_x64")
    set(OBS_LIB_DIR            "${OBS_ROOT}/build_x64/libobs/Release")
    set(OBS_PLUGINS_DIR        "${OBS_ROOT}/build_x64/rundir/Release/obs-plugins/64bit"
        CACHE PATH "Path to OBS plugins output directory (for development)")
endif()

project(phone-webcam VERSION 1.0.0)

# Force Release runtime — must come immediately after project()
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Non-Windows OBS paths (unchanged from original)
# ---------------------------------------------------------------------------
if(APPLE)
    set(OBS_INCLUDE_DIR "/Applications/OBS.app/Contents/Resources/include")
    set(OBS_LIB_DIR     "/Applications/OBS.app/Contents/Frameworks")
elseif(NOT WIN32) # Linux
    set(OBS_INCLUDE_DIR "/usr/include/obs")
    set(OBS_LIB_DIR     "/usr/lib")
endif()

# ---------------------------------------------------------------------------
# Sources and headers
# ---------------------------------------------------------------------------
set(PLUGIN_SOURCES
    src/plugin-main.cpp
    src/phone-source.cpp
    src/rtp-receiver.cpp
    src/srtp-session.cpp
    src/jpeg-decoder.cpp
    src/mdns-discovery.cpp
    src/handshake-client.cpp
    src/h264-decoder.cpp
    src/aac-decoder.cpp
    src/handshake-server.cpp
)

set(PLUGIN_HEADERS
    src/phone-source.h
    src/udp-receiver.h
    src/handshake-client.h
    src/handshake-server.h
    src/h264-decoder.h
    src/aac-decoder.h
    src/rtp-receiver.h
    src/srtp-session.h
    src/jpeg-decoder.h
    src/mdns-discovery.h
    src/auth.h
)

add_library(phone-webcam MODULE
    ${PLUGIN_SOURCES}
    ${PLUGIN_HEADERS}
)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
target_include_directories(phone-webcam PRIVATE
    ${OBS_INCLUDE_DIR}
    ${OBS_BUILD_DIR}/config
    ${TURBOJPEG_INCLUDE_DIR}
    ${FFMPEG_INCLUDE_DIR}
    ${SRTP_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
)

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
target_link_libraries(phone-webcam PRIVATE
    ${TURBOJPEG_LIB}
    ${FFMPEG_LIB_AVCODEC}
    ${FFMPEG_LIB_AVUTIL}
    ${FFMPEG_LIB_SWSCALE}
    ${FFMPEG_LIB_SWRESAMPLE}
    ${SRTP_LIB}
    ${OPENSSL_CRYPTO_LIB}
)

if(WIN32)
    target_link_libraries(phone-webcam PRIVATE
        "${OBS_LIB_DIR}/obs.lib"
        ws2_32
        dnsapi
        iphlpapi
        bcrypt
    )
elseif(APPLE)
    target_link_libraries(phone-webcam PRIVATE
        "${OBS_LIB_DIR}/libobs.dylib"
    )
else()
    target_link_libraries(phone-webcam PRIVATE
        obs
    )
endif()

# ---------------------------------------------------------------------------
# Post-build: copy DLLs next to the plugin .dll (for running from build dir)
# ---------------------------------------------------------------------------
if(WIN32)
    add_custom_command(TARGET phone-webcam POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SRTP_DLL}
            ${OPENSSL_CRYPTO_DLL}
            ${FFMPEG_DLL_AVCODEC}
            ${FFMPEG_DLL_AVUTIL}
            ${FFMPEG_DLL_SWSCALE}
            ${FFMPEG_DLL_SWRESAMPLE}
            ${TURBOJPEG_DLL_JPEG}
            ${TURBOJPEG_DLL_TURBO}
            $<TARGET_FILE_DIR:phone-webcam>
        COMMENT "Copying dependency DLLs to build output directory"
    )

    # Copy plugin + DLLs to OBS dev rundir for testing
    add_custom_command(TARGET phone-webcam POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:phone-webcam>
            ${SRTP_DLL}
            ${OPENSSL_CRYPTO_DLL}
            ${FFMPEG_DLL_AVCODEC}
            ${FFMPEG_DLL_AVUTIL}
            ${FFMPEG_DLL_SWSCALE}
            ${FFMPEG_DLL_SWRESAMPLE}
            ${TURBOJPEG_DLL_JPEG}
            ${TURBOJPEG_DLL_TURBO}
            ${OBS_PLUGINS_DIR}
        COMMENT "Deploying plugin to OBS plugins directory"
    )
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
set_target_properties(phone-webcam PROPERTIES PREFIX "")

if(WIN32)
    install(TARGETS phone-webcam
        LIBRARY DESTINATION "obs-plugins/64bit"
    )
elseif(APPLE)
    install(TARGETS phone-webcam
        LIBRARY DESTINATION "$ENV{HOME}/Library/Application Support/obs-studio/plugins/phone-webcam/bin"
    )
else()
    install(TARGETS phone-webcam
        LIBRARY DESTINATION "$ENV{HOME}/.config/obs-studio/plugins/phone-webcam/bin/64bit"
    )
endif()

install(DIRECTORY data/
    DESTINATION "data/obs-plugins/phone-webcam"
)