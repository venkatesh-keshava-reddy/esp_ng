# ESP-NG: ESP32 Next Generation Framework

A componentized ESP-IDF framework with reusable modules for building ESP32 applications.

## Architecture

This project follows ESP-IDF's component architecture where each module is a self-contained component with proper dependencies.

### Component Structure

```
esp_ng/
├── components/          # Reusable components
│   ├── version/        # Version info
│   ├── event_bus/      # Custom event system
│   ├── config_mgr/     # NVS configuration manager
│   ├── diag/           # Diagnostics
│   ├── wdt_mgr/        # Watchdog manager
│   ├── net_mgr/        # Network/WiFi manager
│   ├── provisioning_mgr/  # WiFi provisioning
│   ├── sntp_client/    # SNTP time sync
│   ├── udp_broadcast/  # UDP broadcast
│   ├── ota_mgr/        # OTA update manager
│   ├── http_ui/        # HTTP server UI
│   └── app_startup/    # Orchestrated startup (Phases 1-3)
├── main/               # Application-specific code
│   └── main.c          # Application entry point
└── partitions.csv      # 8MB flash partition table
```

### Dependency Hierarchy

**Foundation Layer:**
- `version` - No dependencies
- `event_bus` - Minimal ESP-IDF dependencies

**Core Services:**
- `config_mgr` - NVS storage
- `diag` - Diagnostics (depends on version)
- `wdt_mgr` - Watchdog timer

**Network Layer:**
- `net_mgr` - WiFi/network (depends on config_mgr, event_bus)
- `provisioning_mgr` - WiFi provisioning (depends on net_mgr)

**Application Services:**
- `sntp_client` - Time sync (depends on net_mgr, config_mgr, event_bus)
- `udp_broadcast` - UDP broadcast (depends on net_mgr, config_mgr, event_bus, diag, version)
- `ota_mgr` - OTA updates (depends on net_mgr, config_mgr, event_bus, version)
- `http_ui` - Web interface (depends on most other components)

**Startup Orchestration:**
- `app_startup` - Manages generic startup phases 1-3 (depends on all core and network components)

## Hardware Requirements

- **ESP32** (or compatible module)
- **8MB Flash** (ESP32-WROVER recommended)
- Minimum 520KB RAM for WiFi/BLE coexistence

## Flash Partition Layout (8MB)

The partition table is optimized for dual OTA with factory recovery:

| Partition | Size | Offset | Description |
|-----------|------|--------|-------------|
| Bootloader + PT | 128 KB | 0x000000 | ESP-IDF bootloader and partition table |
| NVS | 24 KB | 0x009000 | Non-volatile storage for WiFi, config |
| OTA Data | 8 KB | 0x00F000 | Tracks active OTA partition |
| PHY Init | 4 KB | 0x011000 | RF calibration data |
| Factory | 2 MB | 0x020000 | Factory/recovery firmware |
| OTA_0 | 2.5 MB | 0x220000 | First OTA partition |
| OTA_1 | 2.5 MB | 0x4A0000 | Second OTA partition |
| Storage (SPIFFS) | 832 KB | 0x720000 | File storage |

**Total Used:** 7.97 MB / 8.00 MB (28 KB free)

## Usage

### Building

```bash
cd esp_ng
idf.py build
```

### Flashing

```bash
idf.py -p COMx flash monitor
```

### Using Components in Your Project

You can use these components in your own ESP-IDF projects:

1. Copy desired components from `components/` to your project's `components/` directory
2. Add component dependencies in your `main/CMakeLists.txt`
3. Include component headers in your code

Example:
```c
#include "net_mgr.h"
#include "sntp_client.h"

void app_main(void) {
    net_mgr_start();
    sntp_client_start();
}
```

## Component Features

- **app_startup**: Orchestrated 3-phase generic startup sequence
- **event_bus**: Custom event system for inter-module communication
- **config_mgr**: Persistent configuration storage in NVS
- **net_mgr**: WiFi connection with auto-reconnect
- **provisioning_mgr**: BLE provisioning for WiFi credentials
- **sntp_client**: Network-aware time synchronization
- **ota_mgr**: HTTPS OTA updates with dual-partition support
- **http_ui**: Web-based configuration interface
- **wdt_mgr**: Task watchdog management
- **diag**: System diagnostics and health monitoring
- **version**: Firmware version information

## Migrating from Monolithic main/

Components were extracted from a monolithic `main/` directory to create reusable modules that can be:
- Tested independently
- Reused across projects
- Maintained separately
- Versioned independently (future enhancement)

## License

(Add your license here)
