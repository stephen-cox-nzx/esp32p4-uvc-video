#!/bin/bash
set -e

echo "=== ESP32-P4 UVC Camera Dev Environment Setup ==="

# ESP-IDF is already installed in the espressif/idf:v5.5 image
# Just set the target and validate configuration

# Initialize the project
cd /workspaces/esp32p4-uvc-video

source /opt/esp/idf/export.sh
# idf.py add-dependency "espressif/esp_cam_sensor^2.0.1"
# idf.py add-dependency "espressif/esp_video^2.0.1"
# idf.py add-dependency "espressif/esp_ipa^1.3.1"
# idf.py add-dependency "espressif/usb_device_uvc^1.2.0"

# Set ESP32-P4 as target
echo "Setting ESP32-P4 as build target..."
idf.py set-target esp32p4 --quiet 2>/dev/null || true

# Validate build configuration
echo "Validating build configuration..."
idf.py reconfigure --quiet 2>/dev/null || true

echo ""
echo "✓ Dev environment setup complete!"
echo ""
echo "Available commands:"
echo "  idf.py build          - Build the project"
echo "  idf.py flash          - Flash to device (requires USB connection)"
echo "  idf.py monitor        - Monitor serial output"
echo "  idf.py clean          - Clean build artifacts"
echo "  idf.py fullclean      - Clean all build artifacts including config"
echo "  idf.py set-target esp32p4 - Set build target"
echo ""
