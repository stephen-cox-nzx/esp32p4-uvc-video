# ESP32-P4 UVC Camera - Development Environment Setup

This directory contains the development container (devcontainer) configuration for the ESP32-P4 UVC Camera project using **ESP-IDF v5.5**.

## Overview

The devcontainer provides a complete, reproducible development environment with:

- **ESP-IDF v5.5** (Espressif IoT Development Framework)
- **ESP32-P4** target support with full toolchain
- **Python 3.10+** with all required packages
- **VS Code extensions** for embedded development
- **Build tools**: CMake, Ninja, Make
- **Debugging support**: GDB integration
- **USB device support** for flashing and monitoring

## Quick Start

### Prerequisites

1. **Docker & Docker Compose** installed
2. **VS Code** with Dev Containers extension
3. **USB connection** to your Olimex ESP32-P4 DevKit (optional for building)

[](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)
```bash
winget install --interactive --exact dorssel.usbipd-win
```
[](https://stackoverflow.com/a/76093017)
### Launch the Development Environment

#### Option 1: Using VS Code (Recommended)

1. Open the project in VS Code
2. Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on macOS)
3. Search for "Dev Containers: Reopen in Container"
4. Wait for the container to build and initialize

#### Option 2: Using Command Line

```bash
# From the project root
docker-compose -f .devcontainer/docker-compose.yml up -d
docker exec -it esp32p4-uvc-dev bash
```

## Project Structure

```
.devcontainer/
├── devcontainer.json          # Main devcontainer configuration
├── Dockerfile                 # Custom container image definition
├── docker-compose.yml         # Optional Docker Compose setup
├── post-create.sh            # Setup script (runs after container creation)
├── post-start.sh             # Initialization script (runs on each start)
├── .devcontainerignore       # Files to exclude from sync
├── tasks.json                # VS Code build/flash/monitor tasks
├── launch.json               # GDB debugging configuration
├── settings.json             # VS Code workspace settings
└── cmake-kits.json          # CMake configuration presets
```

## Available Commands

Once in the devcontainer, use these tasks via **Tasks: Run Task** (`Ctrl+Shift+P`):

### Build Commands
- **`idf: build`** - Build the project (default build task)
- **`idf: set-target esp32p4`** - Configure ESP32-P4 as target
- **`idf: reconfigure`** - Reconfigure CMake without rebuilding
- **`idf: clean`** - Clean build artifacts
- **`idf: fullclean`** - Remove all build and config files

### Flash & Monitor
- **`idf: flash`** - Flash firmware to device (requires USB)
- **`idf: flash (monitor)`** - Flash and immediately start monitoring
- **`idf: monitor`** - Monitor serial output in real-time
- **`idf: erase_flash`** - Erase all flash memory

### Configuration & Analysis
- **`idf: menuconfig`** - Open interactive configuration menu
- **`size: app`** - Display firmware size breakdown
- **`size: components`** - Show component-wise size analysis

## Building and Flashing

### Basic Build

```bash
# Inside the container
idf.py build
```

### Build with Target Configuration

```bash
# Set target and build
idf.py set-target esp32p4
idf.py build
```

### Flash to Device

The default flash configuration uses `/dev/ttyACM0` at 921600 baud:

```bash
idf.py flash -p /dev/ttyACM0 -b 921600
```

To find your device port:
```bash
ls /dev/tty* | grep -E "(ACM|USB|USBSERIAL)"
```

### Monitor Serial Output

```bash
idf.py monitor -p /dev/ttyACM0
```

Press `Ctrl+]` to exit the monitor.

### Flash and Monitor Combined

```bash
idf.py flash monitor -p /dev/ttyACM0
```

## Debugging

### Remote GDB Debugging

1. Connect the device with a debug interface (JTAG/J-Link)
2. Start the GDB server on the device
3. In VS Code, open the Debug view (`Ctrl+Shift+D`)
4. Select **"GDB: Connect to Device"**
5. Press `F5` to start debugging

The configuration expects GDB server on `localhost:3333`.

### Breakpoints and Inspection

- Set breakpoints by clicking in the gutter
- Inspect variables in the Variables panel
- Use the Debug Console for live evaluation
- Step through code with F10 (step over) or F11 (step into)

## Hardware Connection

### Serial Connection (USB-C JTAG)

The Olimex ESP32-P4 DevKit USB-C port provides:
- **Serial Console** (for logs and monitoring)
- **JTAG Interface** (for debugging)
- **Bootloader Mode** (for flashing)

Auto-detection: Most modern Linux systems automatically create `/dev/ttyACM0` or `/dev/ttyUSB0`.

### USB High-Speed Interface

The project implements USB 2.0 High-Speed device mode for UVC streaming (separate from the USB-C debug port):
- Uses dedicated GPIO pins on ESP32-P4
- No configuration needed in the devcontainer
- Accessible from host machine when flashed and running

## Environment Variables

The container sets these environment variables:

```bash
IDF_PATH=/opt/esp-idf              # ESP-IDF installation directory
IDF_TOOLS_PATH=/home/vscode/.cache/idf_tools  # Tools cache directory
PYTHONPATH=/opt/esp-idf/tools:...  # Python module paths
```

To verify they're set:
```bash
echo $IDF_PATH
source $IDF_PATH/export.sh  # Re-export if needed
```

## Customization

### Modifying Build Configuration

Edit the project's Kconfig or use the interactive menu:

```bash
idf.py menuconfig
```

Changes are saved to `sdkconfig` (ignored by git). Defaults are in:
- `sdkconfig.defaults`
- `sdkconfig.defaults.esp32p4`

### Adding Python Dependencies

Edit the `post-create.sh` script to add `pip install` commands:

```bash
pip install --no-cache-dir package-name
```

### Adding VS Code Extensions

Add extension IDs to `devcontainer.json`:

```json
"extensions": [
    "publisher.extension-id",
    ...
]
```

### Changing Build Generator

In `devcontainer.json`, modify:

```json
"cmake.tools.preferredGenerators": ["Ninja Multi-Config"]
```

Options: `Ninja`, `Ninja Multi-Config`, `Unix Makefiles`

## Port Forwarding

The devcontainer automatically forwards these ports:

| Port | Service | Purpose |
|------|---------|---------|
| 554  | RTSP Server | Stream video from ESP32 |
| 8080 | Web Interface | Optional web dashboard |
| 3333 | GDB Debug | Remote debugging |

When running in a container, access them via:
- `localhost:554` (from host machine)
- `host.docker.internal:554` (from other containers)

## Troubleshooting

### "IDF_PATH not set" Error

```bash
source /opt/esp-idf/export.sh
```

### Device Not Found During Flash

1. Check USB connection
2. List available ports: `ls /dev/tty*`
3. Verify permissions: `sudo usermod -aG dialout $USER`
4. Try different port: `idf.py flash -p /dev/ttyUSB0`

### "Permission Denied" on Serial Port

```bash
sudo usermod -aG dialout $USER
sudo usermod -aG plugdev $USER
# Log out and back in for changes to take effect
```

### Build Cache Issues

```bash
idf.py fullclean
idf.py set-target esp32p4
idf.py build
```

### Out of Memory During Build

Increase Docker memory allocation in VS Code settings or `docker-compose.yml`.

### Slow CMake Configuration

First build in a container can take 1-2 minutes. Subsequent builds are faster due to caching.

## Performance Optimization

### Parallel Builds

Set Ninja parallelism in tasks.json or use:

```bash
idf.py build -- -j $(nproc)
```

### IDF Tools Caching

Tools are cached in `~/.cache/idf_tools` (mounted as a volume). This persists across container restarts.

### Container Image Size

The Dockerfile uses a slim Ubuntu 22.04 base (~77 MB + IDF ~500 MB).

## File Synchronization

By default, the entire project directory is mounted as a volume, providing:
- **Live file synchronization** between host and container
- **Instant reflection** of editor changes
- **Ability to edit** from either side

Excluded files (in `.devcontainerignore`):
- `.git/`, `build/`, `__pycache__/`
- `*.pyc`, `.pytest_cache/`, `.DS_Store`

## Advanced: Running Without VS Code

### Docker Compose Method

```bash
cd .devcontainer
docker-compose up -d
docker-compose exec esp32p4 idf.py build
docker-compose exec esp32p4 idf.py flash -p /dev/ttyACM0
```

### Direct Docker Command

```bash
docker build -f .devcontainer/Dockerfile -t esp32p4-uvc:v5.5 .
docker run -it --rm \
    -v $(pwd):/workspace \
    -v ~/.cache/idf_tools:/root/.cache/idf_tools \
    --device=/dev/ttyACM0 \
    esp32p4-uvc:v5.5 bash
```

## Documentation & Resources

- [ESP-IDF v5.5 Documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5/)
- [ESP32-P4 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf)
- [VS Code Dev Containers](https://code.visualstudio.com/docs/remote/containers)
- [CMake in VS Code](https://code.visualstudio.com/docs/cpp/cmake-tools)

## Contributing

When modifying the devcontainer:

1. Test changes locally first
2. Update documentation
3. Test on a fresh container to ensure reproducibility
4. Commit all `.devcontainer/` files to git

## License

This devcontainer configuration is provided as-is for development purposes.

---

**Last Updated:** March 2026
**ESP-IDF Version:** v5.5
**Target:** ESP32-P4
**Base Image:** Ubuntu 22.04
