# nrc_modular

Three-layer modular architecture for Newracom NRC WiFi chipsets with SPI interface.

## Quick Start

```bash
# Build
make

# Load modules
sudo insmod nrc_spi.ko
sudo insmod nrc_core.ko
sudo insmod nrc_wlan.ko    # Standard WiFi
# or
sudo insmod nrc_mcp.ko     # Management interface (optional)
```

## Architecture Overview

```
┌─────────────────────────────────────┐
│     Frontend Layer (Choose One)     │
│  ┌──────────────┬───────────────┐   │
│  │ nrc_wlan.ko  │  nrc_mcp.ko   │   │
│  │ MAC80211 WiFi│  Management   │   │
│  │  (Standard)  │  (Optional)   │   │
│  └──────────────┴───────────────┘   │
└─────────────────┬───────────────────┘
                  │
                  ▼
┌─────────────────────────────────────┐
│         HAL Layer (Required)        │
│          nrc_core.ko                │
│   Hardware Abstraction & Core       │
└─────────────────┬───────────────────┘
                  │
                  ▼
┌─────────────────────────────────────┐
│       Backend Layer (Required)      │
│          nrc_spi.ko                 │
│      SPI Hardware Interface         │
└─────────────────────────────────────┘
```

## Directory Structure

```
nrc_modular/
├── README.md                   # This file
├── Kconfig                     # Kernel configuration options
├── Makefile                    # Main build system
│
├── docs/                       # Documentation
│   ├── user-debug.md                      # User: Debug interface guide
│   ├── user-kernel-integration.md         # User: Kernel integration guide
│   ├── dev-overview-architecture.md       # Dev: System architecture overview
│   ├── dev-module-system.md               # Dev: Module system and interfaces
│   ├── dev-layer-wlan.md                  # Dev: WLAN frontend layer
│   ├── dev-layer-mcp.md                   # Dev: MCP frontend layer
│   ├── dev-layer-hal.md                   # Dev: HAL core layer
│   ├── dev-layer-spi.md                   # Dev: SPI backend layer
│   ├── dev-power-management.md            # Dev: Power management system
│   └── dev-fw-lifecycle.md                # Dev: Firmware lifecycle management
│
├── docker/                     # Docker build environment
│   ├── Dockerfile              # Unified image (multi-kernel)
│   ├── README.md               # Docker setup guide
│   ├── build-docker.sh         # Docker cross-compile script
│   ├── build-builtin.sh        # Built-in kernel build script
│   ├── enter-container.sh      # Interactive container shell
│   └── kernel-configs/         # Target device kernel configs
│
├── common/                     # Shared headers
│   ├── nrc.h                   # Core structures
│   ├── nrc-hif.h               # Hardware interface
│   ├── nrc-hif-defs.h          # HIF constants and basic types
│   ├── nrc-debug-common.h      # Debug interface
│   ├── nrc-build-config.h      # Build configuration
│   ├── nrc-trace.h             # Trace system
│   ├── nrc-params.h            # Module parameters structure
│   ├── nrc-ps-common.h         # Power save common definitions
│   ├── nrc-bd-common.h         # Board data common definitions
│   ├── nrc-backend-hif-interface.h    # Backend HIF interface
│   ├── nrc-backend-hif-callback.h     # Backend callbacks
│   ├── nrc-hal-core-interface.h       # HAL core interface
│   ├── nrc-hal-core-callback.h        # HAL callbacks
│   ├── nrc-wim-types.h         # WIM type definitions
│   ├── nrc-vendor.h            # Vendor commands
│   └── nrc-twt.h               # TWT definitions
│
├── backend/                    # Backend Layer
│   ├── dts/
│   │   └── nrc-rpi.dts         # Device tree for Raspberry Pi
│   └── nrc_spi/                # SPI Backend Module
│       ├── nrc-hif-cspi.c      # CSPI protocol implementation
│       ├── nrc-spi-init.c      # Module initialization
│       ├── nrc-spi-device.c    # Device registration
│       ├── nrc-spi-hif-ops.c   # HIF operations
│       ├── nrc-spi-callback.c  # Callback handlers
│       ├── nrc-spi-gpio.c      # GPIO control
│       ├── nrc-spi-params.c    # Module parameters
│       └── nrc-debug.c         # Debug interface
│
├── hal/                        # HAL Layer
│   └── nrc_core/               # Core HAL Module
│       ├── hif.c               # Hardware interface functions
│       ├── nrc-init.c          # Core initialization
│       ├── nrc-hal-init.c      # HAL initialization
│       ├── nrc-hal-export.c    # Symbol exports
│       ├── nrc-fw.c            # Firmware interface
│       ├── nrc-bd.c            # Board data handling
│       ├── nrc-ps.c            # Power save
│       ├── nrc-tx.c            # TX processing
│       ├── nrc-dump.c          # Debug dump functions
│       ├── nrc-hal-ops-impl.c  # HAL operations implementation
│       ├── nrc-hal-callback.c  # Callback system
│       ├── wim.c               # WIM operations
│       └── nrc-debug.c         # Debug interface
│
├── frontend/                   # Frontend Layer
│   ├── nrc_wlan/               # WLAN Frontend (Standard WiFi)
│   │   ├── nrc-mac80211.c      # MAC80211 interface
│   │   ├── mac80211-ext.c      # MAC80211 extensions
│   │   ├── nrc-wlan-init.c     # WLAN initialization
│   │   ├── nrc-wlan-hal-init.c # HAL initialization
│   │   ├── nrc-wlan-post-init.c # Post initialization
│   │   ├── nrc-wlan-callback.c # Callback handlers
│   │   ├── nrc-wlan-params.c   # Module parameters
│   │   ├── nrc-trx.c           # TX/RX data path
│   │   ├── nrc-netlink.c       # Netlink interface
│   │   ├── nrc-pm.c            # Power management
│   │   ├── nrc-ps.c            # Power save
│   │   ├── nrc-s1g.c           # 802.11ah (S1G) support
│   │   ├── nrc-mac80211-twt.c  # TWT (Target Wake Time)
│   │   ├── nrc-twt-sched.c     # TWT scheduling
│   │   ├── nrc-apf.c           # Android Packet Filter
│   │   ├── nrc-stats.c         # Statistics
│   │   ├── nrc-wim-wlan.c      # WLAN-specific WIM operations
│   │   ├── nrc-debug.c         # Debug interface
│   │   ├── nrc-radiotap.h      # Radiotap header definitions
│   │   ├── nrc-trx-handler.h   # TX/RX handler macros
│   │   └── nrc-sta-handler.h   # STA handler macros
│   │
│   └── nrc_mcp/                # MCP Frontend (Optional)
│       ├── mcp.h               # MCP definitions
│       ├── nrc-mcp-init.c      # MCP initialization
│       ├── nrc-mcp-callback.c  # Callback handlers
│       ├── nrc-mcp-params.c    # Module parameters
│       ├── nrc-mcp-transmit.c  # MCP transmit functions
│       ├── nrc-netlink-driver.c # Netlink interface
│       ├── nrc-log.c           # Logging functions
│       ├── nrc-debug.c         # Debug interface
│       └── nrc-rpi.dts         # Device tree for Raspberry Pi
│
└── scripts/                    # Utility scripts
    ├── deploy-modules.sh       # Module deployment script (SSH/ADB)
    ├── remote-build.sh         # Remote target build script (rsync+SSH)
    ├── debug.sh                # Debug control script
    ├── start_modular.py        # Module loader
    ├── stop_modular.py         # Module unloader
    ├── start.py                # Legacy start script
    ├── start_wlan.py           # WLAN-only start script
    ├── start_mcp.py            # MCP-only start script
    ├── iperf.sh                # iperf test helper
    ├── monitor_ps.sh           # Power save monitoring
    ├── android/                # Android-specific scripts
    └── memleak/                # Memory leak monitoring
```

## Documentation

### User Documentation

| Document | Description |
|----------|-------------|
| **[user-debug.md](docs/user-debug.md)** | Debug interface usage, debug.sh script, debugfs |
| **[user-kernel-integration.md](docs/user-kernel-integration.md)** | Integrating driver into kernel tree, Kconfig |

### Developer Documentation

| Document | Description |
|----------|-------------|
| [dev-overview-architecture.md](docs/dev-overview-architecture.md) | System architecture, module structure, data paths |
| [dev-module-system.md](docs/dev-module-system.md) | Module loading, initialization, interfaces, callbacks |
| [dev-layer-wlan.md](docs/dev-layer-wlan.md) | WLAN frontend - IEEE 802.11, TX/RX, credit management |
| [dev-layer-mcp.md](docs/dev-layer-mcp.md) | MCP frontend - Custom protocol, netlink channels |
| [dev-layer-hal.md](docs/dev-layer-hal.md) | HAL core - HIF protocol, packet routing, WIM |
| [dev-layer-spi.md](docs/dev-layer-spi.md) | SPI backend - Physical interface, slot management |
| [dev-power-management.md](docs/dev-power-management.md) | Power management - States, sleep/wake flows |
| [dev-fw-lifecycle.md](docs/dev-fw-lifecycle.md) | Firmware lifecycle - Load, start, restart flows |

## Building

### Native Build

```bash
# Auto-detect architecture
make

# Cross-compile for ARM64
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# Build individual modules
make -C backend/nrc_spi
make -C hal/nrc_core
make -C frontend/nrc_wlan
make -C frontend/nrc_mcp

# Clean
make clean
```

## Installation

### Automated Deployment

```bash
# Deploy to target device via SSH
./scripts/deploy-modules.sh pi@192.168.0.4

# Deploy to Android device via ADB
./scripts/deploy-modules.sh adb

# Show deployment help
./scripts/deploy-modules.sh help
```

### Remote Build

Build directly on target device (useful when Docker cross-compile is unavailable):

```bash
# Build on remote target via SSH (syncs source, builds, fetches .ko files)
./scripts/remote-build.sh 192.168.0.4 pi raspberry
```

### Manual Installation

**Load Order (Important):**
```bash
# Load in dependency order
sudo insmod nrc_spi.ko          # 1. Backend
sudo insmod nrc_core.ko         # 2. HAL
sudo insmod nrc_wlan.ko         # 3. WLAN Frontend

# Or load MCP instead/additionally
sudo insmod nrc_mcp.ko          # 3. MCP Frontend (optional)
```

**Unload Order:**
```bash
# Unload in reverse order
sudo rmmod nrc_wlan             # or nrc_mcp
sudo rmmod nrc_core
sudo rmmod nrc_spi
```

### Using Start Script

```bash
# Load all modules automatically (AP mode)
sudo ./scripts/start_modular.py 1 0 US

# Stop all modules
sudo ./scripts/stop_modular.py
```

## Module Configuration

### Module Parameters

**nrc_spi.ko:**
```bash
modinfo nrc_spi.ko
# View available parameters
```

**nrc_core.ko:**
```bash
modinfo nrc_core.ko
```

**nrc_wlan.ko:**
```bash
modinfo nrc_wlan.ko
# Example: power_save, fw_name, etc.
```

### Runtime Configuration

```bash
# Debug control
sudo ./scripts/debug.sh mask status
sudo ./scripts/debug.sh mask preset verbose

# Device control
sudo ./scripts/debug.sh device status
sudo ./scripts/debug.sh signal

# See user-debug.md for detailed usage
```

## Debugging

### Debug Interface

```bash
# Check debug masks
cat /sys/kernel/debug/nrc_core/debug_mask

# Check SKB statistics
cat /sys/kernel/debug/nrc_core/skb_stats

# Enable verbose debugging
sudo ./scripts/debug.sh mask preset verbose

# Monitor kernel log
dmesg -wH
```

### Common Issues

**Module load fails:**
```bash
# Check dependencies
modprobe --show-depends nrc_wlan

# Check kernel log
dmesg | tail -50
```

**No wireless interface:**
```bash
# Check module loaded
lsmod | grep nrc

# Check interfaces
iw dev
ip link show
```

See [user-debug.md](docs/user-debug.md) for comprehensive debugging guide.

## Frontend Selection

### WLAN Frontend (nrc_wlan.ko)

**Use for:**
- Standard WiFi station mode
- Access Point (AP) mode
- WiFi Direct / P2P
- Integration with NetworkManager/wpa_supplicant

**Features:**
- Full MAC80211 support
- 802.11ah (S1G) support
- TWT (Target Wake Time)
- Power save modes
- Android Packet Filter (APF)

### MCP Frontend (nrc_mcp.ko)

**Use for:**
- Custom device management
- Low-level hardware testing
- Factory testing tools
- Firmware development

**Features:**
- Direct netlink interface
- Low-level control
- Specialized management operations

**Note:** Both frontends can coexist and share HAL/Backend layers.

## Kernel Integration

To integrate into mainline kernel tree, see [user-kernel-integration.md](docs/user-kernel-integration.md).

**Summary:**
1. Copy driver to `drivers/net/wireless/newracom/`
2. Add Kconfig source and Makefile entry
3. Configure with `make menuconfig`
4. Build with `make modules`

## Supported Platforms

**Tested:**
- Raspberry Pi 4 (ARM64)
- Linux kernel 4.19+
- Tested kernels: 5.10.17, 6.12.47

**Requirements:**
- `CONFIG_CFG80211=m` (for WLAN)
- `CONFIG_MAC80211=m` (for WLAN)
- `CONFIG_SPI=y`

## License

See individual source files for license information.
