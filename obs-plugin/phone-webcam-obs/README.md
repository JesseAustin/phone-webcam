# Phone Webcam OBS Plugin

Ultra-low latency phone-as-webcam plugin for OBS Studio.

## Features

- **Ultra-low latency**: 20-40ms glass-to-glass on local network
- **MJPEG streaming**: Individual JPEG frames over UDP
- **Simple setup**: Just install plugin and connect phone to same WiFi
- **Cross-platform**: Windows, macOS, Linux

## Building

### Prerequisites

**All platforms:**
- CMake 3.20 or higher
- C++17 compiler
- OBS Studio 29.0 or higher installed
- libjpeg-turbo development files

**Windows:**
- Visual Studio 2022 (Community Edition is fine)
- Windows 10 SDK

**macOS:**
- Xcode 14 or higher

**Linux:**
- GCC 11+ or Clang 14+
- `libobs-dev` package
- `libjpeg-turbo8-dev` package

### Build Instructions

#### Windows (Visual Studio)

```bash
# Clone or download this repository
cd phone-webcam-obs

# Create build directory
mkdir build
cd build

# Configure CMake (adjust OBS path as needed)
cmake .. -DOBS_DIR="C:/Program Files/obs-studio"

# Build
cmake --build . --config Release

# Install (copies to OBS plugins folder)
cmake --install . --config Release
```

#### Windows (VS Code)

1. Install VS Code extensions:
   - C/C++ (Microsoft)
   - CMake Tools (Microsoft)

2. Open folder in VS Code
3. Press `Ctrl+Shift+P` → "CMake: Configure"
4. Set `OBS_DIR` in CMake settings
5. Press `F7` to build

#### macOS

```bash
cd phone-webcam-obs
mkdir build && cd build

cmake ..
make -j4

# Install
make install
```

#### Linux

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install libobs-dev libjpeg-turbo8-dev cmake build-essential

cd phone-webcam-obs
mkdir build && cd build

cmake ..
make -j4

# Install to user plugins folder
make install
```

### Installing libjpeg-turbo

**Windows:**
Download from https://libjpeg-turbo.org/ and install to `C:\libjpeg-turbo64`

**macOS:**
```bash
brew install jpeg-turbo
```

**Linux:**
```bash
sudo apt install libjpeg-turbo8-dev  # Ubuntu/Debian
sudo dnf install libjpeg-turbo-devel  # Fedora
```

## Usage

1. Build and install the plugin
2. Restart OBS Studio
3. Add a new source → "Phone Webcam"
4. Note the UDP port number (default: 9000)
5. Install the Android app on your phone
6. Connect phone to same WiFi network
7. Enter the port number in the app
8. Start streaming!

## Architecture

```
Phone (Android)           Network (UDP)           OBS Plugin
─────────────────         ─────────────           ───────────

Camera2 API
    ↓
YUV frames
    ↓
libjpeg-turbo     ────────────────────→        UDP Receiver
JPEG encoding                                        ↓
    ↓                                          Packet reassembly
Packet                                               ↓
fragmentation                                   JPEG decoder
    ↓                                                ↓
UDP socket        ←────────────────────        RGB → RGBA
                                                     ↓
                                              OBS source output
```

## Performance

**Target latency**: 20-40ms glass-to-glass

**Breakdown**:
- Camera capture: 8ms
- JPEG encode: 5-10ms (phone)
- Network: 2-5ms (local WiFi)
- JPEG decode: 3-5ms (desktop)
- OBS processing: 2-5ms

**Bandwidth** (1080p @ 30fps):
- MJPEG: ~24 Mbps
- Easily handled by WiFi 4 (802.11n) and higher

## Troubleshooting

**Plugin doesn't show up in OBS:**
- Check that the .dll/.so/.dylib is in the correct plugins folder
- On Windows: `%APPDATA%\obs-studio\plugins\phone-webcam\bin\64bit`
- On macOS: `~/Library/Application Support/obs-studio/plugins/phone-webcam/bin`
- On Linux: `~/.config/obs-studio/plugins/phone-webcam/bin/64bit`

**No video appears:**
- Check firewall settings (allow UDP on port 9000)
- Verify phone and PC are on same WiFi network
- Try increasing port number if 9000 is in use

**Video is choppy:**
- Check WiFi signal strength
- Ensure other devices aren't saturating bandwidth
- Try moving closer to router

## License

MIT License (customize as needed)

## Credits

Built with:
- [OBS Studio](https://obsproject.com/)
- [libjpeg-turbo](https://libjpeg-turbo.org/)

## TODO

- [ ] Android app implementation
- [ ] USB support via ADB port forwarding
- [ ] H.264 codec option
- [ ] Device discovery (UDP broadcast)
- [ ] Quality/resolution settings
- [ ] Frame rate display
- [ ] Connection status indicator
