# Quick Reference - ESP32-P4 UVC Camera DevContainer

## Getting Started (30 seconds)

1. **Open in VS Code**
   ```
   Ctrl+Shift+P → "Dev Containers: Reopen in Container"
   ```
   Wait for container to build (~3-5 minutes first time)

2. **Build the Project**
   ```
   Ctrl+Shift+P → "Tasks: Run Task" → "idf: build"
   ```

3. **Flash to Device** (USB connected)
   ```
   Ctrl+Shift+P → "Tasks: Run Task" → "idf: flash"
   ```

4. **Monitor Output**
   ```
   Ctrl+Shift+P → "Tasks: Run Task" → "idf: monitor"
   ```

## Most Common Tasks

| Task | VS Code | Terminal |
|------|---------|----------|
| **Build** | `Tasks → idf: build` | `idf.py build` |
| **Flash** | `Tasks → idf: flash` | `idf.py flash -p /dev/ttyACM0` |
| **Monitor** | `Tasks → idf: monitor` | `idf.py monitor -p /dev/ttyACM0` |
| **Both** | `Tasks → idf: flash (monitor)` | `idf.py flash monitor` |
| **Clean** | `Tasks → idf: clean` | `idf.py clean` |
| **Full Clean** | `Tasks → idf: fullclean` | `idf.py fullclean` |
| **Config** | `Tasks → idf: menuconfig` | `idf.py menuconfig` |
| **Size Info** | `Tasks → size: app` | `idf.py size` |

## Environment

- **ESP-IDF:** v5.5 (installed in `/opt/esp-idf`)
- **Target:** ESP32-P4
- **Toolchain:** xtensa-esp32p4-elf
- **Python:** 3.10+ (with all IDF dependencies)
- **Build System:** CMake + Ninja/Make

## Device Connection

- **Serial Port:** `/dev/ttyACM0` (USB-C on Olimex board)
- **Baud Rate:** 921600 (default for flash), 115200 (monitor)
- **Finding Port:** `ls /dev/tty* | grep -E "(ACM|USB)"`

## File Locations Inside Container

| Path | Purpose |
|------|---------|
| `/workspace` | Your project root |
| `/opt/esp-idf` | ESP-IDF v5.5 installation |
| `/home/vscode/.cache/idf_tools` | Tool cache (persisted) |
| `/workspace/build` | Build output |
| `/workspace/sdkconfig` | Build configuration |

## Debugging (GDB)

1. Start debug server on device (OpenOCD/J-Link)
2. `Ctrl+Shift+D` → Select "GDB: Connect to Device"
3. `F5` to start debugging
4. Set breakpoints, step through code, inspect variables

## USB Flash Protocol

Device automatically enters bootloader when:
- USB is connected and data lines are properly configured
- `idf.py flash` is executed with proper port and baud

To manually force bootloader:
```bash
# Hold GPIO0 low while asserting reset, release reset, then release GPIO0
```

## Ports & Services

| Port | Service | Protocol |
|------|---------|----------|
| 554 | RTSP Server | RTSP/RTP |
| 8080 | Web Interface | HTTP |
| 3333 | GDB Debug | TCP |

Access from host: `localhost:PORT`

## Build Optimization

```bash
# Parallel build (uses all CPU cores)
idf.py build -- -j $(nproc)

# Incremental build (default, much faster)
idf.py build

# Full rebuild from scratch
idf.py fullclean && idf.py build
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| **Permission denied /dev/ttyACM0** | `sudo usermod -aG dialout $USER` (then logout/login) |
| **CMake error** | `idf.py reconfigure` |
| **Build fails mysteriously** | `idf.py fullclean && idf.py build` |
| **"IDF_PATH not set"** | `source /opt/esp-idf/export.sh` |
| **Device not detected** | Check USB cable, try different port (`-p /dev/ttyUSB0`) |
| **Monitor shows garbage** | Check baud rate: default 115200 |

## Configuration (Kconfig)

Access menuconfig:
```bash
idf.py menuconfig
```

Or edit directly:
- Main config: `/workspace/sdkconfig`
- Defaults: `/workspace/sdkconfig.defaults*`

Camera-specific settings in `sdkconfig.defaults.esp32p4`

## Performance Monitoring

```bash
# Firmware size breakdown
idf.py size-components

# RAM usage analysis
idf.py size

# Performance statistics (if enabled in config)
# Check build/esp_video_uvc.map for detailed symbols
```

## Useful ESP-IDF Commands

```bash
# List all targets
idf.py --list-targets

# Show version
idf.py --version

# Get help
idf.py --help

# Build specific component
idf.py build -c <component_name>

# Verbose output
idf.py -v build

# Dry run (show commands without executing)
idf.py -n build
```

## Container Lifecycle

```bash
# First time: builds image + creates container
# Just opening in VS Code handles this automatically

# Restart container (if needed)
# Dev Containers → Rebuild Container

# Stop container
# Dev Containers → Stop Container

# Clean up everything
# Dev Containers → Remove Dev Container
```

## Quick Tips

- ✅ Always `idf.py set-target esp32p4` first on fresh clones
- ✅ Use mounted volume cache: changes persist across container restarts
- ✅ Build errors? Try `idf.py reconfigure` before `fullclean`
- ✅ Monitor baud rate is usually 115200, not the flash baud rate
- ✅ Press `Ctrl+]` in monitor to exit (not Ctrl+C)
- ✅ `.devcontainer` files are version-controlled, edits persist

## Next Steps

1. **Read** [.devcontainer/README.md](./README.md) for detailed info
2. **Configure** hardware via `idf.py menuconfig`
3. **Build** with `idf: build` task
4. **Flash** with `idf: flash` task
5. **Monitor** with `idf: monitor` task

---

**Container:** `esp32p4-uvc:v5.5` | **ESP-IDF:** v5.5 | **Target:** ESP32-P4
