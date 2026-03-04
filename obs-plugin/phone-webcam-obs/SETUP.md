# Quick Setup Guide for OBS Plugin Development

## What You Just Got

A complete OBS plugin scaffold that:
- Receives MJPEG frames over UDP
- Decodes JPEG frames with libjpeg-turbo  
- Outputs video to OBS as a source
- Handles packet reassembly for frames larger than MTU

## Development Environment Setup

### Option 1: Visual Studio (Recommended for Windows)

**Installation:**
1. Download Visual Studio 2022 Community: https://visualstudio.microsoft.com/
2. During install, select **"Desktop development with C++"** workload
3. Install OBS Studio: https://obsproject.com/download
4. Install libjpeg-turbo: https://libjpeg-turbo.org/Downloads/Latest

**Building:**
```bash
cd phone-webcam-obs
mkdir build
cd build
cmake .. -DOBS_DIR="C:/Program Files/obs-studio"
cmake --build . --config Release
```

### Option 2: Visual Studio Code (Cross-platform)

**Installation:**
1. Download VS Code: https://code.visualstudio.com/
2. Install extensions:
   - C/C++ (Microsoft)
   - CMake Tools (Microsoft)
   - CMake (twxs)
3. Install OBS Studio
4. Install libjpeg-turbo

**Building:**
1. Open `phone-webcam-obs` folder in VS Code
2. Press `Ctrl+Shift+P` → "CMake: Configure"
3. Press `F7` to build
4. Plugin will be in `build/Release/` or `build/`

## File Structure Explained

```
phone-webcam-obs/
├── CMakeLists.txt           # Build configuration
├── src/
│   ├── plugin-main.cpp      # OBS plugin entry point (registers source)
│   ├── phone-source.cpp     # Main source implementation
│   ├── phone-source.h       # Source structure and callbacks
│   ├── udp-receiver.cpp     # UDP packet handling
│   ├── udp-receiver.h
│   ├── jpeg-decoder.cpp     # JPEG decoding with libjpeg-turbo
│   └── jpeg-decoder.h
├── data/
│   └── locale/
│       └── en-US.ini        # UI text strings
└── README.md                # Full documentation
```

## Key Components

### 1. Plugin Main (`plugin-main.cpp`)
- Entry point for OBS
- Registers the "Phone Webcam" source type
- Minimal, just tells OBS about our source

### 2. Phone Source (`phone-source.cpp`)
- Implements OBS source callbacks
- Creates network thread to receive frames
- Outputs decoded frames to OBS

### 3. UDP Receiver (`udp-receiver.cpp`)
- Listens on UDP port (default 9000)
- Reassembles fragmented frames
- Non-blocking socket for smooth operation

### 4. JPEG Decoder (`jpeg-decoder.cpp`)
- Uses libjpeg-turbo for fast decoding
- Converts JPEG → RGB
- Handles errors gracefully

## Next Steps

### 1. Build the Plugin
Follow the build instructions for your platform in README.md

### 2. Test with Dummy Data
Before building the Android app, you can test the plugin with a simple Python script:

```python
# test_sender.py - Send a test JPEG over UDP
import socket
import struct

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

with open('test.jpg', 'rb') as f:
    jpeg_data = f.read()

# Simple single-packet send (for small JPEGs)
header = struct.pack('IIII', 1, 1, 0, len(jpeg_data))
sock.sendto(header + jpeg_data, ('127.0.0.1', 9000))
```

### 3. Verify in OBS
1. Open OBS
2. Add source → "Phone Webcam"
3. Run the Python test script
4. You should see the test image appear

### 4. Build Android App
Once the plugin works with test data, build the Android app that:
- Captures frames with Camera2 API
- Encodes with libjpeg-turbo
- Sends via UDP with the same packet format

## Debugging Tips

**Windows:**
- Attach Visual Studio debugger to OBS.exe
- Set breakpoints in `phone_source_update()` or `receive_frame()`

**VS Code:**
- Use "Attach to Process" and select OBS
- Works on Windows, macOS, Linux

**Logging:**
- Check OBS log files: Help → Log Files → Show Log Files
- Look for "Phone Webcam" entries
- All `blog()` calls appear here

## Common Issues

**Plugin doesn't load:**
- Check OBS log for error messages
- Verify libjpeg-turbo is installed
- Ensure OBS version is 29.0+

**Can't find OBS headers:**
- Set `OBS_DIR` CMake variable to OBS install location
- Windows: Usually `C:/Program Files/obs-studio`
- macOS: `/Applications/OBS.app/Contents/Resources`

**Linker errors:**
- Install libjpeg-turbo development files
- Check that paths in CMakeLists.txt are correct

## Performance Notes

The current implementation:
- Uses a single network thread
- Non-blocking sockets to prevent hangs
- Minimal buffering for low latency
- RGB → RGBA conversion (could optimize to direct RGBA decode)

**Optimization opportunities:**
1. Use libjpeg-turbo's direct RGBA output (skip conversion)
2. Use OBS's texture API instead of frame copy
3. Add frame dropping when behind (already drops packets)
4. Hardware JPEG decode (platform-specific)

## What's Missing (Android Side)

The Android app needs to:
1. Capture YUV frames with Camera2 API
2. Convert YUV → JPEG with libjpeg-turbo
3. Fragment JPEG into packets (if > 1400 bytes)
4. Send with same header structure:
   ```c
   struct PacketHeader {
       uint32_t sequence_number;  // Frame number
       uint32_t total_packets;    // How many packets in this frame
       uint32_t packet_index;     // Which packet is this (0-based)
       uint32_t data_size;        // Bytes in each packet
   };
   ```

Ready to start building the Android side, or want to test the plugin first?
