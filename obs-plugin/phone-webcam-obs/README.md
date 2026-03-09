# Phone Webcam OBS Plugin

Ultra-low latency phone-as-webcam plugin for OBS Studio. Works with the **Phone Webcam** Android app.

## Features

- **Ultra-low latency**: 20-40ms glass-to-glass on local network
- **H.264 streaming**: Hardware-accelerated video encoding on the phone
- **SRTP encryption**: AES-128 encrypted video and audio streams with per-session random salt and PBKDF2 key derivation
- **Mutual authentication**: HMAC-SHA256 challenge-response — neither side starts streaming without verifying the other knows the password
- **Auto-discovery**: mDNS automatically finds your phone on the network — no IP address needed
- **Audio support**: AAC audio streamed alongside video
- **Simple setup**: Install plugin, install app, connect to same WiFi

## How It Works

The Phone Webcam Android app streams your phone's camera to OBS over your local WiFi network. When both are on the same network, OBS auto-discovers the phone via mDNS and initiates a handshake — no manual IP entry required. If you set a password in OBS, the app must use the same password to connect; the stream will be fully encrypted over SRTP. Once connected, video arrives as H.264 over RTP and audio as AAC on the next port up.

## Building

### Prerequisites

- CMake 3.20 or higher
- C++17 compiler
- OBS Studio 29.0 or higher
- Visual Studio 2022 (Windows)

**Windows dependencies:**

First, install [vcpkg](https://github.com/microsoft/vcpkg) if you don't have it, then run:

```bash
vcpkg install libsrtp
vcpkg install nlohmann-json
vcpkg integrate install
```

`vcpkg integrate install` makes Visual Studio and CMake automatically find vcpkg-installed packages without any manual path configuration.

**FFmpeg** — Download a shared build from [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) or [BtbN](https://github.com/BtbN/FFmpeg-Builds/releases) and extract to `C:\ffmpeg`. The CMakeLists expects FFmpeg 7.x (`avcodec-62.dll` etc).

**OpenSSL** — Download the Win64 installer from [slproweb](https://slproweb.com/products/Win32OpenSSL.html) and install to the default location (`C:\Program Files\OpenSSL-Win64`).

**libjpeg-turbo** *(optional, legacy)* — Download the installer from [libjpeg-turbo.org](https://libjpeg-turbo.org/) and install to `C:\libjpeg-turbo64`.

### Build Instructions

#### Windows (Visual Studio)

```bash
cd phone-webcam-obs
mkdir build
cd build
cmake .. -DOBS_ROOT="C:/dev/obs-studio"
cmake --build . --config Release
cmake --install . --config Release
```

Override any dependency paths if your installs differ from the defaults:

```bash
cmake .. -DOBS_ROOT="C:/dev/obs-studio" -DFFMPEG_ROOT="D:/libs/ffmpeg" -DSRTP_ROOT="D:/libs/libsrtp2"
```

#### Windows (VS Code)

1. Install VS Code extensions:
   - C/C++ (Microsoft)
   - CMake Tools (Microsoft)
2. Open folder in VS Code
3. Press `Ctrl+Shift+P` → "CMake: Configure"
4. Set `OBS_ROOT` and any other paths in CMake settings
5. Press `F7` to build

#### Linux

Linux support is not officially tested yet. Contributions welcome.

## Installing

Extract the zip so that the contents of `obs-plugins/64bit/` land in:

```
C:\Program Files\obs-studio\obs-plugins\64bit\
```

Restart OBS after installing.

## Usage

1. Install the plugin and restart OBS
2. Install the **Phone Webcam** app on your Android phone
3. Connect phone and PC to the same WiFi network
4. In OBS, add a new source → **Phone Webcam**
5. Optionally set a password in the source properties (recommended)
6. Open the app, set the same password, and press **Start Stream**
7. OBS will auto-discover the phone and begin receiving video

### Ports Used

| Purpose | Port |
|---|---|
| RTP video | 9000 (configurable) |
| RTP audio | video port + 1 |
| Handshake (TCP) | 65432 (fixed) |

Make sure your firewall allows UDP on your chosen video port and the port above it, and TCP on 65432.

## Security

When a password is set:
- Both sides perform HMAC-SHA256 mutual authentication before streaming begins
- A random 32-byte salt is generated fresh each session and exchanged during the handshake
- PBKDF2 (SHA-256, 7000 iterations) derives the SRTP master key from the password and salt
- Video and audio are independently encrypted with AES-128 SRTP

Without a password the stream is unencrypted. On a trusted home network this is low risk, but a password is strongly recommended.

## Bitrate

The app supports 8–40 Mbps. For 1080p30 on a typical home WiFi network, 20 Mbps is a good starting point.

## Troubleshooting

**Plugin doesn't show up in OBS:**
- Verify the `.dll` files are in `C:\Program Files\obs-studio\obs-plugins\64bit\`
- Restart OBS after installing

**No video appears:**
- Check firewall — allow UDP on your video port and port+1, and TCP on 65432
- Verify phone and PC are on the same WiFi network (not guest vs main)
- Check that the password in OBS and the app match exactly

**Video is choppy:**
- Check WiFi signal strength
- Try reducing bitrate in the app
- Ensure other devices aren't saturating the network

## License

This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this software, either in source code form or as a compiled binary, for any purpose, commercial or non-commercial, and by any means.

In jurisdictions that recognize copyright laws, the author or authors of this software dedicate any and all copyright interest in the software to the public domain. We make this dedication for the benefit of the public at large and to the detriment of our heirs and successors. We intend this dedication to be an overt act of relinquishment in perpetuity of all present and future rights to this software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <https://unlicense.org>