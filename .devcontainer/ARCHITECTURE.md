```
PROJECT STRUCTURE: ESP32-P4 UVC Camera DevContainer Configuration
═════════════════════════════════════════════════════════════════

esp32p4-uvc-video/
│
├── .devcontainer/                          ← ALL DEVCONTAINER FILES HERE
│   │
│   ├── 📋 devcontainer.json               Main VS Code devcontainer config
│   │   └─ Features: extensions, settings, port forwarding, build/start hooks
│   │
│   ├── 🐳 Dockerfile                      Custom container image definition
│   │   └─ Contents: Ubuntu 22.04 + ESP-IDF v5.5 + ESP32-P4 toolchain
│   │
│   ├── 🐳 docker-compose.yml              Docker Compose orchestration
│   │   └─ Services: esp32p4 with volumes, devices, ports forwarded
│   │
│   ├── 📜 post-create.sh                  Setup script (runs after creation)
│   │   └─ Initializes ESP-IDF, sets target, validates configuration
│   │
│   ├── 📜 post-start.sh                   Startup script (runs on each start)
│   │   └─ Exports ESP-IDF environment variables
│   │
│   ├── ⚙️  tasks.json                     VS Code build tasks
│   │   └─ Tasks: build, flash, monitor, clean, menuconfig, size analysis
│   │
│   ├── 🐛 launch.json                     GDB debugging configuration
│   │   └─ Configs: remote GDB, flash+monitor debugging
│   │
│   ├── ⚙️  settings.json                  VS Code editor settings
│   │   └─ C/C++ analysis, Python formatting, CMake configuration
│   │
│   ├── 📦 cmake-kits.json                 CMake generator presets
│   │   └─ Presets: Ninja Multi-Config, Unix Makefiles
│   │
│   ├── 📄 README.md                       Comprehensive 80-section guide
│   │   └─ Full documentation, troubleshooting, advanced usage
│   │
│   ├── ⚡ QUICKSTART.md                   Quick reference (most tasks)
│   │   └─ 30-second setup, common tasks, troubleshooting table
│   │
│   ├── 📝 DEVCONTAINER_SUMMARY.md         This configuration summary
│   │   └─ Overview, files created, features, architecture
│   │
│   ├── 📋 GITIGNORE_ADDITIONS.md          Suggested .gitignore entries
│   │   └─ Build artifacts, IDE files, Python cache, OS-specific files
│   │
│   └── 🚫 .devcontainerignore             Files excluded from sync
│       └─ .git/, build/, __pycache__/, *.pyc, etc.
│
├── main/
├── components/
├── tools/
└── [other project files...]


CONTAINER ARCHITECTURE
═══════════════════════════════════════════════════════════════════

    ┌─────────────────────────────────────────────────────────┐
    │              HOST MACHINE (Linux/macOS/Windows)         │
    │                                                         │
    │  ┌─────────────────────────────────────────────────┐   │
    │  │  VS Code Editor                                 │   │
    │  │  ├─ Tasks: Ctrl+Shift+P → "Tasks: Run Task"   │   │
    │  │  ├─ Debug: Ctrl+Shift+D → Select config       │   │
    │  │  └─ Terminal: Ctrl+` → Dev Container Shell    │   │
    │  └─────────────────────────────────────────────────┘   │
    │                           │                             │
    │                           ▼                             │
    │  ┌─────────────────────────────────────────────────┐   │
    │  │ Docker / Docker Desktop / Colima               │   │
    │  └─────────────────────────────────────────────────┘   │
    │                           │                             │
    └───────────────────────────┼─────────────────────────────┘
                                │
                    ┌───────────┴───────────┐
                    │                       │
                    ▼                       ▼
        ┌──────────────────────┐  ┌──────────────────────┐
        │ Volume Mounts        │  │ Device Access        │
        │                      │  │                      │
        │ /workspace ◄────────────┤ Project files       │
        │ (live sync)          │  │ /dev/ttyACM0        │
        │                      │  │ /dev/bus/usb        │
        │ ~/.cache/idf_tools ◄────┤ USB serial/debug    │
        │ (persistent cache)   │  │                      │
        └──────────────────────┘  └──────────────────────┘
                    │
                    └────────────────┐
                                     ▼
        ┌────────────────────────────────────────────────────┐
        │   DOCKER CONTAINER (Ubuntu 22.04)                 │
        │                                                    │
        │  Environment:                                      │
        │  ├─ IDF_PATH=/opt/esp-idf                         │
        │  ├─ IDF_TOOLS_PATH=/home/vscode/.cache/idf_tools │
        │  └─ PYTHONPATH=/opt/esp-idf/tools:...             │
        │                                                    │
        │  ┌──────────────────────────────────────────────┐ │
        │  │ /opt/esp-idf (v5.5)                          │ │
        │  │ ├─ tools/cmake/                              │ │
        │  │ ├─ tools/idf_tools.py                        │ │
        │  │ ├─ components/                               │ │
        │  │ ├─ docs/                                     │ │
        │  │ └─ export.sh                                 │ │
        │  └──────────────────────────────────────────────┘ │
        │                                                    │
        │  ┌──────────────────────────────────────────────┐ │
        │  │ Build Tools & Toolchain                      │ │
        │  │ ├─ CMake 3.16+                               │ │
        │  │ ├─ Ninja + Make                              │ │
        │  │ ├─ xtensa-esp32p4-elf (compiler)            │ │
        │  │ ├─ GDB + OpenOCD                             │ │
        │  │ ├─ Python 3.10+ (pyserial, protobuf, etc)   │ │
        │  │ └─ TinyUSB, esp-idf-size, esp-idf-monitor   │ │
        │  └──────────────────────────────────────────────┘ │
        │                                                    │
        │  ┌──────────────────────────────────────────────┐ │
        │  │ /workspace (Project Root)                    │ │
        │  │ ├─ CMakeLists.txt                            │ │
        │  │ ├─ sdkconfig (generated)                     │ │
        │  │ ├─ build/ (generated artifacts)              │ │
        │  │ ├─ main/                                     │ │
        │  │ ├─ components/                               │ │
        │  │ └─ .devcontainer/ (this config)              │ │
        │  └──────────────────────────────────────────────┘ │
        │                                                    │
        │  Forwarded Ports:                                 │
        │  ├─ 554:554   → RTSP Server (ESP32)              │
        │  ├─ 8080:8080 → Web Interface (optional)         │
        │  └─ 3333:3333 → GDB Debug Server                │
        └────────────────────────────────────────────────────┘
                                │
                    ┌───────────┴───────────┐
                    │                       │
                    ▼                       ▼
        ┌──────────────────────┐  ┌──────────────────────┐
        │ Build Process        │  │ Serial Communication │
        │                      │  │                      │
        │ idf.py build         │  │ /dev/ttyACM0         │
        │ idf.py flash         │  │ 115200 baud (monitor)│
        │ idf.py monitor       │  │ 921600 baud (flash)  │
        │ idf.py menuconfig    │  │                      │
        │ idf.py size          │  │ Data flow:           │
        │                      │  │ ├─ Logs              │
        │ Build output:        │  │ ├─ Debug output      │
        │ ├─ .elf (binary)     │  │ ├─ Commands from host│
        │ ├─ .bin (firmware)   │  │ └─ Status/responses  │
        │ └─ .map (symbols)    │  │                      │
        └──────────────────────┘  └──────────────────────┘
                    │
                    └────────────────┐
                                     ▼
        ┌────────────────────────────────────────────────────┐
        │   OLIMEX ESP32-P4 DEVKIT (Target Hardware)         │
        │                                                    │
        │  ┌──────────────────────────────────────────────┐ │
        │  │ ESP32-P4 SoC                                 │ │
        │  │ ├─ Dual-core RISC-V @ 400MHz                │ │
        │  │ ├─ 32MB PSRAM (hex mode, 200MHz)            │ │
        │  │ └─ UHS (USB High-Speed) + Ethernet PHY      │ │
        │  └──────────────────────────────────────────────┘ │
        │                                                    │
        │  ┌──────────────────────────────────────────────┐ │
        │  │ Sensors & Peripherals                        │ │
        │  │ ├─ OV5647 Camera (5MP, 30fps@1080p RAW10)   │ │
        │  │ ├─ IP101GR Ethernet PHY (100Mbps)           │ │
        │  ├─ USB 2.0 HS (concurrent with debug port)    │ │
        │  └─ JTAG Debug Interface (USB-C)               │ │
        │  └──────────────────────────────────────────────┘ │
        │                                                    │
        │  Connections to Host:                             │
        │  ├─ USB-C: Serial/JTAG/Debug                     │
        │  ├─ USB HS: UVC Webcam stream                    │
        │  └─ Ethernet: RTSP/RTP stream (optional)         │
        └────────────────────────────────────────────────────┘


WORKFLOW DIAGRAM
═════════════════════════════════════════════════════════════════

┌──────────────────────┐
│ 1. Open in VS Code   │
│ Ctrl+Shift+P         │
│ "Reopen in Container"│
└──────────┬───────────┘
           ▼
┌──────────────────────────────────────┐
│ 2. Container Initialization (3-5 min)│
│ ├─ Build Docker image                │
│ ├─ Download ESP-IDF v5.5             │
│ ├─ Install toolchain                 │
│ ├─ Run post-create.sh                │
│ └─ Mount volumes & device access     │
└──────────┬───────────────────────────┘
           ▼
┌──────────────────────────────────────┐
│ 3. Ready for Development             │
│ ├─ IDF environment active            │
│ ├─ Tasks available                   │
│ └─ Code completion working           │
└──────────┬───────────────────────────┘
           ▼
        ┌─────────────────────────────────────────┐
        │ 4. Edit & Build Cycle                   │
        │                                         │
        │ Edit source files                      │
        │ ↓                                       │
        │ Run: idf: build task                   │
        │ ↓                                       │
        │ Incremental compile in container       │
        │ ↓                                       │
        │ View errors in VS Code Problems panel  │
        │ ↓                                       │
        │ Fix & rebuild (< 1 min)               │
        └──────────────────┬──────────────────────┘
                           ▼
        ┌──────────────────────────────────────┐
        │ 5. Flash & Monitor                   │
        │                                      │
        │ Connect ESP32-P4 via USB-C          │
        │ ↓                                    │
        │ Run: idf: flash task                │
        │ ↓                                    │
        │ Firmware transferred (5-10 sec)    │
        │ ↓                                    │
        │ Device reboots & runs firmware      │
        │ ↓                                    │
        │ Run: idf: monitor task              │
        │ ↓                                    │
        │ View serial output in real-time     │
        │ ↓                                    │
        │ Analyze logs, set breakpoints       │
        └──────────────────┬───────────────────┘
                           ▼
        ┌──────────────────────────────────────┐
        │ 6. Debug (Optional)                  │
        │                                      │
        │ Connect GDB server on device        │
        │ ↓                                    │
        │ Ctrl+Shift+D → Select GDB config   │
        │ ↓                                    │
        │ F5 to start debugging               │
        │ ↓                                    │
        │ Step, inspect, set breakpoints      │
        └──────────────────────────────────────┘


FILE RELATIONSHIPS
═══════════════════════════════════════════════════════════════════

devcontainer.json (MASTER CONFIG)
├─ References: Dockerfile
├─ References: .devcontainer features
├─ Loads: settings.json
├─ Loads: extensions list
├─ Executes: post-create.sh → post-start.sh
├─ Mounts: volumes from docker-compose.yml (compatible)
└─ Provides: environment variables for tasks.json

Dockerfile (BUILD IMAGE)
├─ Base: mcr.microsoft.com/devcontainers/base:ubuntu-22.04
├─ Installs: ~30 system dependencies
├─ Downloads: esp-idf from GitHub
├─ Compiles: Toolchain via idf_tools.py
├─ Creates: vscode user with USB access
└─ Used by: devcontainer.json & docker-compose.yml

post-create.sh (INITIALIZATION)
├─ Sources: /opt/esp-idf/export.sh
├─ Sets: IDF_PATH, IDF_TOOLS_PATH
├─ Runs: idf.py set-target esp32p4
├─ Validates: Build configuration
└─ Output: Environment ready message

tasks.json (BUILD AUTOMATION)
├─ idf: build → Executes: idf.py build
├─ idf: flash → Executes: idf.py flash -p /dev/ttyACM0
├─ idf: monitor → Executes: idf.py monitor -p /dev/ttyACM0
├─ Problem matchers: Parse output for errors
└─ Requires: devcontainer.json environment

launch.json (DEBUGGING)
├─ GDB Config → Connects to localhost:3333
├─ Pre-launch task: flash device first
├─ Post-launch: Stop at first breakpoint
└─ Tools: xtensa-esp32p4-elf-gdb in container

settings.json (EDITOR PREFERENCES)
├─ CMake configuration
├─ C/C++ IntelliSense settings
├─ Editor formatting rules
├─ File exclusions for search/display
└─ Code analysis (clang-tidy) configuration

cmake-kits.json (BUILD PRESETS)
├─ Defines: Ninja Multi-Config generator
├─ Fallback: Unix Makefiles
├─ Toolchain: xtensa-esp32p4-elf-gcc
└─ Cache variables: CMAKE_BUILD_TYPE, IDF_TARGET

docker-compose.yml (ORCHESTRATION)
├─ Builds: Using Dockerfile
├─ Mounts: Project root + tool cache + USB
├─ Services: esp32p4 (main development service)
├─ Ports: 554, 8080, 3333 (forwarded)
└─ Environment: IDF_PATH, PYTHONPATH, etc.

README.md (DOCUMENTATION)
├─ Comprehensive guide (80+ sections)
├─ References: All other config files
├─ Examples: Common commands & workflows
└─ Troubleshooting: Problem → Solution pairs

QUICKSTART.md (QUICK REFERENCE)
├─ 30-second setup instructions
├─ Most common task table
├─ Device connection info
├─ Troubleshooting quick table
└─ Next steps for full documentation


CONFIGURATION PRIORITY & LOADING ORDER
═══════════════════════════════════════════════════════════════════

1st Priority: devcontainer.json
    └─ Loaded by VS Code Remote Container extension
    └─ All subsequent configs referenced/loaded from here

2nd Priority: Dockerfile
    └─ Executed during container build
    └─ Sets up base environment
    └─ Installs all dependencies

3rd Priority: post-create.sh
    └─ Executed AFTER container is ready
    └─ Initializes project-specific settings
    └─ Sets IDF target, validates config

4th Priority: post-start.sh
    └─ Executed EVERY TIME container starts
    └─ Re-exports environment variables
    └─ Quick startup hook

5th Priority: settings.json + cmake-kits.json
    └─ Loaded by VS Code when opening workspace
    └─ Defines editor behavior & build presets

6th Priority: tasks.json
    └─ Parsed by VS Code task runner
    └─ Executes idf.py commands

7th Priority: launch.json
    └─ Loaded when Debug view is opened
    └─ Configures GDB integration

Docker-compose.yml
    └─ Alternative to VS Code devcontainer
    └─ Used for non-VS Code development
    └─ Compatible with devcontainer.json structure


DEPENDENCY GRAPH
═══════════════════════════════════════════════════════════════════

Host System
    ├─ Docker/Docker Desktop
    │   └─ ESP32-P4-UVC Image
    │       ├─ Ubuntu 22.04 (base)
    │       ├─ Build tools (CMake, Ninja, Make)
    │       ├─ ESP-IDF v5.5
    │       │   ├─ xtensa-esp32p4-elf (toolchain)
    │       │   ├─ python3 modules
    │       │   └─ OpenOCD/GDB
    │       └─ TinyUSB, other libraries
    │
    ├─ VS Code
    │   ├─ Dev Containers Extension
    │   ├─ C/C++ Extension
    │   ├─ Python Extension
    │   ├─ CMake Tools Extension
    │   └─ ESP-IDF Extension
    │
    └─ Hardware (ESP32-P4)
        ├─ Bootloader (pre-flashed)
        └─ IDF runtime environment


FILE SIZES (Approximate)
═════════════════════════════════════════════════════════════════════

Configuration Files:
├─ devcontainer.json ............ ~2 KB
├─ Dockerfile ................... ~4 KB
├─ post-create.sh ............... ~1 KB
├─ post-start.sh ................ <1 KB
├─ tasks.json ................... ~6 KB
├─ launch.json .................. ~2 KB
├─ settings.json ................ ~3 KB
├─ cmake-kits.json .............. ~1 KB
└─ docker-compose.yml ........... ~2 KB

Documentation:
├─ README.md .................... ~25 KB
├─ QUICKSTART.md ................ ~8 KB
└─ DEVCONTAINER_SUMMARY.md ...... ~15 KB

Total Config: ~27 KB
Total Documentation: ~48 KB
Total: ~75 KB (completely negligible)

Container Image Build:
├─ Base image (Ubuntu) .......... ~77 MB
├─ System packages .............. ~200 MB
├─ ESP-IDF v5.5 ................. ~500 MB
└─ Total final image ............ ~800 MB (compressed)


VERSION COMPATIBILITY
═════════════════════════════════════════════════════════════════════

✓ ESP-IDF: v5.5 (specified)
✓ ESP32-P4: Full support
✓ Base Image: Ubuntu 22.04 LTS
✓ Python: 3.10, 3.11 (compatible)
✓ CMake: 3.16+ (required by IDF)
✓ Ninja: 1.10+ (recommended)
✓ Docker: 20.10+ (buildx support)
✓ VS Code: 1.60+ (remote container support)

Tested On:
├─ Linux (native Docker)
├─ macOS (Docker Desktop / Colima)
└─ Windows (WSL2 + Docker Desktop)


═════════════════════════════════════════════════════════════════════
                    END OF ARCHITECTURE DIAGRAM
═════════════════════════════════════════════════════════════════════
```
