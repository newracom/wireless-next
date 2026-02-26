#!/bin/bash
################################################################################
# NRC Driver Debug Mask Control Script
#
# Copyright (c) 2016-2025 Newracom, Inc.
#
# This script provides comprehensive control over debug masks for all NRC
# driver modules (SPI, Core, WLAN, MCP).
#
# Usage: nrc-debug.sh [command] [options]
#        See 'nrc-debug.sh help' for detailed usage information
################################################################################

# Color definitions for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Debugfs paths - Debug Masks
DEBUGFS_SPI="/sys/kernel/debug/nrc_spi/debug_mask"
DEBUGFS_CORE="/sys/kernel/debug/nrc_core/debug_mask"
DEBUGFS_WLAN="/sys/kernel/debug/ieee80211/nrc80211/debug_mask"
DEBUGFS_MCP="/sys/kernel/debug/nrc_mcp/debug_mask"

# Debugfs paths - Debug Levels
DEBUGFS_SPI_LEVEL="/sys/kernel/debug/nrc_spi/debug_level"
DEBUGFS_CORE_LEVEL="/sys/kernel/debug/nrc_core/debug_level"
DEBUGFS_WLAN_LEVEL="/sys/kernel/debug/ieee80211/nrc80211/debug_level"
DEBUGFS_MCP_LEVEL="/sys/kernel/debug/nrc_mcp/debug_level"

# Debugfs paths - Core Module
DEBUGFS_SKB_DEBUG="/sys/kernel/debug/nrc_core/skb_debug"
DEBUGFS_SKB_STATS="/sys/kernel/debug/nrc_core/skb_stats"
DEBUGFS_CREDIT="/sys/kernel/debug/nrc_core/credit"
DEBUGFS_SLOT="/sys/kernel/debug/nrc_core/slot"
DEBUGFS_DEBUG_LEVEL="/sys/kernel/debug/nrc_core/debug_level"
DEBUGFS_RESTART="/sys/kernel/debug/nrc_core/restart"
DEBUGFS_PS_CONTROL="/sys/kernel/debug/nrc_core/ps_control"
DEBUGFS_FW_CONTROL="/sys/kernel/debug/nrc_core/fw_control"

# Debugfs paths - SPI Backend Module
DEBUGFS_CSPI="/sys/kernel/debug/nrc_spi/cspi"
DEBUGFS_RESET="/sys/kernel/debug/nrc_spi/reset"

# Debugfs paths - Loopback Test (Core Module)
DEBUGFS_LB_TEST="/sys/kernel/debug/nrc_core/loopback/test"
DEBUGFS_LB_COUNT="/sys/kernel/debug/nrc_core/loopback/count"
DEBUGFS_LB_SAMPLE="/sys/kernel/debug/nrc_core/loopback/sample"
DEBUGFS_LB_REPORT="/sys/kernel/debug/nrc_core/loopback/report"
DEBUGFS_LB_HEXDUMP="/sys/kernel/debug/nrc_core/loopback/hexdump"

# Debugfs paths - WLAN Module
DEBUGFS_WLAN_INFO="/sys/kernel/debug/ieee80211/nrc80211/info"
DEBUGFS_WAKEUP="/sys/kernel/debug/ieee80211/nrc80211/wakeup"
DEBUGFS_SLEEP="/sys/kernel/debug/ieee80211/nrc80211/sleep"
DEBUGFS_SNR="/sys/kernel/debug/ieee80211/nrc80211/snr"
DEBUGFS_RSSI="/sys/kernel/debug/ieee80211/nrc80211/rssi"
DEBUGFS_BEACON="/sys/kernel/debug/ieee80211/nrc80211/beacon_updated"
DEBUGFS_TPUT="/sys/kernel/debug/ieee80211/nrc80211/expected_tput"

# Debug level definitions (from enum NRC_DEBUG_LEVEL in nrc-debug-common.h)
declare -A DEBUG_LEVELS=(
    ["ERR"]=0       # NRC_DBG_LEVEL_ERR = 0 (Errors only)
    ["WARN"]=1      # NRC_DBG_LEVEL_WARN = 1 (Warnings and above)
    ["INFO"]=2      # NRC_DBG_LEVEL_INFO = 2 (Info and above - default)
    ["DBG"]=3       # NRC_DBG_LEVEL_DBG = 3 (All debug messages)
)

# Debug bit definitions (from enum NRC_DEBUG_MASK in nrc-debug-common.h)
# Each enum value represents a bit position, converted to hex mask
declare -A DEBUG_BITS=(
    ["BASIC"]=0x00000001    # NRC_DBG_BASIC = 0 (bit 0)
    ["HIF"]=0x00000002      # NRC_DBG_HIF = 1 (bit 1)
    ["WIM"]=0x00000004      # NRC_DBG_WIM = 2 (bit 2)
    ["TX"]=0x00000008       # NRC_DBG_TX = 3 (bit 3)
    ["RX"]=0x00000010       # NRC_DBG_RX = 4 (bit 4)
    ["MAC"]=0x00000020      # NRC_DBG_MAC = 5 (bit 5)
    ["CAPI"]=0x00000040     # NRC_DBG_CAPI = 6 (bit 6)
    ["PS"]=0x00000080       # NRC_DBG_PS = 7 (bit 7)
    ["STATS"]=0x00000100    # NRC_DBG_STATS = 8 (bit 8)
    ["STATE"]=0x00000200    # NRC_DBG_STATE = 9 (bit 9)
    ["BD"]=0x00000400       # NRC_DBG_BD = 10 (bit 10)
    ["FW"]=0x00000800       # NRC_DBG_FW = 11 (bit 11)
    ["AMPDU"]=0x00001000    # NRC_DBG_AMPDU = 12 (bit 12)
    ["CREDIT"]=0x00002000   # NRC_DBG_CREDIT = 13 (bit 13)
    ["SLOT"]=0x00004000     # NRC_DBG_SLOT = 14 (bit 14)
    ["BUS"]=0x00008000      # NRC_DBG_BUS = 15 (bit 15)
)

# All debug bits enabled mask
DEBUG_ALL=0xFFFFFFFF

################################################################################
# Helper Functions
################################################################################

# Print colored message
print_msg() {
    local color=$1
    shift
    echo -e "${color}$@${NC}"
}

# Print error and exit
error_exit() {
    print_msg "$RED" "ERROR: $@" >&2
    exit 1
}

# Check if debugfs entry exists
check_debugfs() {
    local path=$1
    local module=$2

    if [ ! -f "$path" ]; then
        print_msg "$YELLOW" "Warning: $module module debugfs not found at $path"
        print_msg "$YELLOW" "         (Module may not be loaded)"
        return 1
    fi
    return 0
}

# Read debug mask from debugfs
read_debug_mask() {
    local path=$1
    if [ -f "$path" ]; then
        cat "$path" 2>/dev/null
    else
        echo "N/A"
    fi
}

# Write debug mask to debugfs
write_debug_mask() {
    local path=$1
    local value=$2

    if [ -f "$path" ]; then
        echo "$value" > "$path" 2>/dev/null
        if [ $? -eq 0 ]; then
            return 0
        else
            print_msg "$RED" "Failed to write to $path"
            return 1
        fi
    else
        return 1
    fi
}

# Convert decimal to hex string
dec_to_hex() {
    printf "0x%08X" "$1"
}

# Convert mask value to human-readable text (bit names)
mask_to_text() {
    local mask=$1
    local bits=()
    
    # Find all active bits
    for bit_name in "${!DEBUG_BITS[@]}"; do
        local bit_value=${DEBUG_BITS[$bit_name]}
        if [ $((mask & bit_value)) -ne 0 ]; then
            bits+=("$bit_name")
        fi
    done
    
    # Return comma-separated list or "None"
    if [ ${#bits[@]} -eq 0 ]; then
        echo "None"
    else
        # Sort and join with commas
        echo "${bits[*]}" | tr ' ' ','
    fi
}

# Parse debug bit names and return combined mask
parse_debug_bits() {
    local bits="$@"
    local mask=0

    for bit in $bits; do
        bit=$(echo "$bit" | tr '[:lower:]' '[:upper:]')
        if [ -n "${DEBUG_BITS[$bit]}" ]; then
            mask=$((mask | ${DEBUG_BITS[$bit]}))
        else
            print_msg "$YELLOW" "Warning: Unknown debug bit '$bit' (ignoring)"
        fi
    done

    echo "$mask"
}

################################################################################
# Command Functions
################################################################################

# Show current debug mask status for all modules
cmd_status() {
    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "NRC Driver Debug Mask Status"
    print_msg "$CYAN" "=========================================="
    echo

    # SPI Module
    print_msg "$BLUE" "SPI Module ($DEBUGFS_SPI):"
    if check_debugfs "$DEBUGFS_SPI" "SPI"; then
        local mask=$(read_debug_mask "$DEBUGFS_SPI")
        printf "  Current mask: %s (%s) [%s]\n" "$(dec_to_hex $mask)" "$mask" "$(mask_to_text $mask)"
        show_active_bits "$mask"
    fi
    echo

    # Core Module
    print_msg "$BLUE" "Core Module ($DEBUGFS_CORE):"
    if check_debugfs "$DEBUGFS_CORE" "Core"; then
        local mask=$(read_debug_mask "$DEBUGFS_CORE")
        printf "  Current mask: %s (%s) [%s]\n" "$(dec_to_hex $mask)" "$mask" "$(mask_to_text $mask)"
        show_active_bits "$mask"
    fi
    echo

    # WLAN Module
    print_msg "$BLUE" "WLAN Module ($DEBUGFS_WLAN):"
    if check_debugfs "$DEBUGFS_WLAN" "WLAN"; then
        local mask=$(read_debug_mask "$DEBUGFS_WLAN")
        printf "  Current mask: %s (%s) [%s]\n" "$(dec_to_hex $mask)" "$mask" "$(mask_to_text $mask)"
        show_active_bits "$mask"
    fi
    echo

    # MCP Module
    print_msg "$BLUE" "MCP Module ($DEBUGFS_MCP):"
    if check_debugfs "$DEBUGFS_MCP" "MCP"; then
        local mask=$(read_debug_mask "$DEBUGFS_MCP")
        printf "  Current mask: %s (%s) [%s]\n" "$(dec_to_hex $mask)" "$mask" "$(mask_to_text $mask)"
        show_active_bits "$mask"
    fi
}

# Show active debug bits for a given mask
show_active_bits() {
    local mask=$1
    local active_bits=()

    for bit_name in "${!DEBUG_BITS[@]}"; do
        local bit_value=${DEBUG_BITS[$bit_name]}
        if [ $((mask & bit_value)) -ne 0 ]; then
            active_bits+=("$bit_name")
        fi
    done

    if [ ${#active_bits[@]} -eq 0 ]; then
        print_msg "$YELLOW" "  Active bits: None (all debug disabled)"
    else
        print_msg "$GREEN" "  Active bits: ${active_bits[*]}"
    fi
}

################################################################################
# Debug Level Commands
################################################################################

# Show current debug level
cmd_level_status() {
    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "Debug Level Status"
    print_msg "$CYAN" "=========================================="
    echo

    # SPI Module Level
    print_msg "$BLUE" "SPI Module ($DEBUGFS_SPI_LEVEL):"
    if [ -f "$DEBUGFS_SPI_LEVEL" ]; then
        local level=$(cat "$DEBUGFS_SPI_LEVEL" 2>/dev/null)
        local level_name=""
        case "$level" in
            0) level_name="ERR  - Errors only" ;;
            1) level_name="WARN - Warnings and errors" ;;
            2) level_name="INFO - Info, warnings, and errors" ;;
            3) level_name="DBG  - All debug messages" ;;
            *) level_name="Unknown" ;;
        esac
        printf "  Level: %s (%s)\n" "$level" "$level_name"
    else
        print_msg "$YELLOW" "  Not available (SPI module not loaded?)"
    fi
    echo

    # Core Module Level
    print_msg "$BLUE" "Core Module ($DEBUGFS_CORE_LEVEL):"
    if [ -f "$DEBUGFS_CORE_LEVEL" ]; then
        local level=$(cat "$DEBUGFS_CORE_LEVEL" 2>/dev/null)
        local level_name=""
        case "$level" in
            0) level_name="ERR  - Errors only" ;;
            1) level_name="WARN - Warnings and errors" ;;
            2) level_name="INFO - Info, warnings, and errors" ;;
            3) level_name="DBG  - All debug messages" ;;
            *) level_name="Unknown" ;;
        esac
        printf "  Level: %s (%s)\n" "$level" "$level_name"
    else
        print_msg "$YELLOW" "  Not available (Core module not loaded?)"
    fi
    echo

    # WLAN Module Level
    print_msg "$BLUE" "WLAN Module ($DEBUGFS_WLAN_LEVEL):"
    if [ -f "$DEBUGFS_WLAN_LEVEL" ]; then
        local level=$(cat "$DEBUGFS_WLAN_LEVEL" 2>/dev/null)
        local level_name=""
        case "$level" in
            0) level_name="ERR  - Errors only" ;;
            1) level_name="WARN - Warnings and errors" ;;
            2) level_name="INFO - Info, warnings, and errors" ;;
            3) level_name="DBG  - All debug messages" ;;
            *) level_name="Unknown" ;;
        esac
        printf "  Level: %s (%s)\n" "$level" "$level_name"
    else
        print_msg "$YELLOW" "  Not available (WLAN module not loaded?)"
    fi
    echo

    # MCP Module Level
    print_msg "$BLUE" "MCP Module ($DEBUGFS_MCP_LEVEL):"
    if [ -f "$DEBUGFS_MCP_LEVEL" ]; then
        local level=$(cat "$DEBUGFS_MCP_LEVEL" 2>/dev/null)
        local level_name=""
        case "$level" in
            0) level_name="ERR  - Errors only" ;;
            1) level_name="WARN - Warnings and errors" ;;
            2) level_name="INFO - Info, warnings, and errors" ;;
            3) level_name="DBG  - All debug messages" ;;
            *) level_name="Unknown" ;;
        esac
        printf "  Level: %s (%s)\n" "$level" "$level_name"
    else
        print_msg "$YELLOW" "  Not available (MCP module not loaded?)"
    fi
    echo

    print_msg "$CYAN" "Available Levels:"
    printf "  %s - Errors only (ERR_* messages)\n" "0 (ERR)"
    printf "  %s - Warnings and above (WARn + ERR_*)\n" "1 (WARN)"
    printf "  %s - Info and above (INFO + WARn + ERR_*) [default]\n" "2 (INFO)"
    printf "  %s - All debug messages (DBG_* + INFO + WARn + ERR_*)\n" "3 (DBG)"
}

# Set debug level
cmd_level_set() {
    local level="$1"

    if [ -z "$level" ]; then
        error_exit "Usage: $0 level set <level>\n  Levels: ERR(0), WARN(1), INFO(2), DBG(3)"
    fi

    # Parse level (name or number)
    local level_value=""
    local level_name=""

    if [[ "$level" =~ ^[0-9]+$ ]]; then
        # Numeric value
        if [ "$level" -ge 0 ] && [ "$level" -le 3 ]; then
            level_value=$level
        else
            error_exit "Invalid level number. Must be 0-3 (ERR=0, WARN=1, INFO=2, DBG=3)"
        fi
    else
        # Level name
        level=$(echo "$level" | tr '[:lower:]' '[:upper:]')
        if [ -n "${DEBUG_LEVELS[$level]}" ]; then
            level_value=${DEBUG_LEVELS[$level]}
            level_name=$level
        else
            error_exit "Invalid level name '$level'. Use: ERR, WARN, INFO, or DBG"
        fi
    fi

    # Determine name for display
    case "$level_value" in
        0) level_name="ERR" ;;
        1) level_name="WARN" ;;
        2) level_name="INFO" ;;
        3) level_name="DBG" ;;
    esac

    # Set SPI module debug level
    if [ -f "$DEBUGFS_SPI_LEVEL" ]; then
        echo "$level_value" > "$DEBUGFS_SPI_LEVEL" 2>/dev/null
        if [ $? -eq 0 ]; then
            print_msg "$GREEN" "SPI: Debug level set to $level_name ($level_value)"
        else
            print_msg "$RED" "SPI: Failed to set debug level"
        fi
    else
        print_msg "$YELLOW" "SPI: Debug level not available"
    fi

    # Set Core module debug level
    if [ -f "$DEBUGFS_CORE_LEVEL" ]; then
        echo "$level_value" > "$DEBUGFS_CORE_LEVEL" 2>/dev/null
        if [ $? -eq 0 ]; then
            print_msg "$GREEN" "Core: Debug level set to $level_name ($level_value)"
        else
            print_msg "$RED" "Core: Failed to set debug level"
        fi
    else
        print_msg "$YELLOW" "Core: Debug level not available"
    fi

    # Set WLAN module debug level
    if [ -f "$DEBUGFS_WLAN_LEVEL" ]; then
        echo "$level_value" > "$DEBUGFS_WLAN_LEVEL" 2>/dev/null
        if [ $? -eq 0 ]; then
            print_msg "$GREEN" "WLAN: Debug level set to $level_name ($level_value)"
        else
            print_msg "$RED" "WLAN: Failed to set debug level"
        fi
    else
        print_msg "$YELLOW" "WLAN: Debug level not available"
    fi

    # Set MCP module debug level
    if [ -f "$DEBUGFS_MCP_LEVEL" ]; then
        echo "$level_value" > "$DEBUGFS_MCP_LEVEL" 2>/dev/null
        if [ $? -eq 0 ]; then
            print_msg "$GREEN" "MCP: Debug level set to $level_name ($level_value)"
        else
            print_msg "$RED" "MCP: Failed to set debug level"
        fi
    else
        print_msg "$YELLOW" "MCP: Debug level not available"
    fi

    echo
    case "$level_value" in
        0) echo "  Only ERR_* messages will be shown" ;;
        1) echo "  WARn and ERR_* messages will be shown" ;;
        2) echo "  INFO, WARn, and ERR_* messages will be shown" ;;
        3) echo "  All debug messages (DBG_*, INFO, WARn, ERR_*) will be shown" ;;
    esac
}

# Show available debug bit definitions
cmd_list_bits() {
    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "Available Debug Bits"
    print_msg "$CYAN" "=========================================="
    echo

    printf "%-20s %-12s %s\n" "Bit Name" "Hex Value" "Description"
    printf "%-20s %-12s %s\n" "--------" "---------" "-----------"

    # Sort bits by value
    for bit_name in $(for k in "${!DEBUG_BITS[@]}"; do echo "$k ${DEBUG_BITS[$k]}"; done | sort -k2 -n | cut -d' ' -f1); do
        local bit_value=${DEBUG_BITS[$bit_name]}
        local description=""

        case "$bit_name" in
            BASIC) description="Basic general debug messages" ;;
            HIF) description="Host Interface operations" ;;
            WIM) description="Wireless Interface Message" ;;
            TX) description="TX operations" ;;
            RX) description="RX operations" ;;
            MAC) description="MAC layer operations" ;;
            CAPI) description="C API operations" ;;
            PS) description="Power save" ;;
            STATS) description="Statistics" ;;
            STATE) description="State machine" ;;
            BD) description="Board data operations" ;;
            FW) description="Firmware state and operations" ;;
            AMPDU) description="A-MPDU aggregation monitoring" ;;
            CREDIT) description="Credit management and updates" ;;
            SLOT) description="TX/RX slot management" ;;
            BUS) description="Backend bus operations (SPI/SDIO/UART)" ;;
        esac

        printf "%-20s %-12s %s\n" "$bit_name" "$(dec_to_hex $bit_value)" "$description"
    done

    echo
    print_msg "$CYAN" "Special Values:"
    printf "%-20s %-12s %s\n" "ALL" "$(dec_to_hex $DEBUG_ALL)" "All debug bits enabled"
    printf "%-20s %-12s %s\n" "NONE" "0x00000000" "All debug bits disabled"
}

# Set debug mask for specified module(s)
cmd_set() {
    local module="$1"
    local value="$2"

    if [ -z "$module" ] || [ -z "$value" ]; then
        error_exit "Usage: $0 mask set <module|all> <hex_value|bit_names...>"
    fi

    # Parse value (hex or bit names)
    local mask=0
    if [[ "$value" =~ ^0x[0-9a-fA-F]+$ ]]; then
        # Hex value
        mask=$((value))
    elif [[ "$value" =~ ^[0-9]+$ ]]; then
        # Decimal value
        mask=$value
    else
        # Parse as bit names
        shift 2
        mask=$(parse_debug_bits "$value" "$@")
    fi


    module=$(echo "$module" | tr '[:upper:]' '[:lower:]')

    case "$module" in
        spi)
            set_module_mask "$DEBUGFS_SPI" "SPI" "$mask"
            ;;
        core)
            set_module_mask "$DEBUGFS_CORE" "Core" "$mask"
            ;;
        wlan)
            set_module_mask "$DEBUGFS_WLAN" "WLAN" "$mask"
            ;;
        mcp)
            set_module_mask "$DEBUGFS_MCP" "MCP" "$mask"
            ;;
        all)
            set_module_mask "$DEBUGFS_SPI" "SPI" "$mask"
            set_module_mask "$DEBUGFS_CORE" "Core" "$mask"
            set_module_mask "$DEBUGFS_WLAN" "WLAN" "$mask"
            set_module_mask "$DEBUGFS_MCP" "MCP" "$mask"
            ;;
        *)
            error_exit "Invalid module '$module'. Use: spi, core, wlan, mcp, or all"
            ;;
    esac
}

# Set debug mask for a specific module
set_module_mask() {
    local path=$1
    local module=$2
    local mask=$3

    if check_debugfs "$path" "$module"; then
        if write_debug_mask "$path" "$mask"; then
            print_msg "$GREEN" "$module: Set debug mask to $(dec_to_hex $mask) ($mask) [$(mask_to_text $mask)]"
        else
            print_msg "$RED" "$module: Failed to set debug mask"
        fi
    fi
}

# Enable specific debug bits (OR operation)
cmd_enable() {
    local module="$1"
    shift
    local bits="$@"

    if [ -z "$module" ] || [ -z "$bits" ]; then
        error_exit "Usage: $0 mask enable <module|all> <bit_names...>"
    fi


    local new_bits=$(parse_debug_bits "$bits")
    module=$(echo "$module" | tr '[:upper:]' '[:lower:]')

    case "$module" in
        spi)
            enable_module_bits "$DEBUGFS_SPI" "SPI" "$new_bits"
            ;;
        core)
            enable_module_bits "$DEBUGFS_CORE" "Core" "$new_bits"
            ;;
        wlan)
            enable_module_bits "$DEBUGFS_WLAN" "WLAN" "$new_bits"
            ;;
        mcp)
            enable_module_bits "$DEBUGFS_MCP" "MCP" "$new_bits"
            ;;
        all)
            enable_module_bits "$DEBUGFS_SPI" "SPI" "$new_bits"
            enable_module_bits "$DEBUGFS_CORE" "Core" "$new_bits"
            enable_module_bits "$DEBUGFS_WLAN" "WLAN" "$new_bits"
            enable_module_bits "$DEBUGFS_MCP" "MCP" "$new_bits"
            ;;
        *)
            error_exit "Invalid module '$module'. Use: spi, core, wlan, mcp, or all"
            ;;
    esac
}

# Enable bits for a specific module
enable_module_bits() {
    local path=$1
    local module=$2
    local new_bits=$3

    if check_debugfs "$path" "$module"; then
        local current=$(read_debug_mask "$path")
        local new_mask=$((current | new_bits))

        if write_debug_mask "$path" "$new_mask"; then
            print_msg "$GREEN" "$module: Enabled bits, new mask = $(dec_to_hex $new_mask) ($new_mask) [$(mask_to_text $new_mask)]"
        else
            print_msg "$RED" "$module: Failed to enable bits"
        fi
    fi
}

# Disable specific debug bits (AND NOT operation)
cmd_disable() {
    local module="$1"
    shift
    local bits="$@"

    if [ -z "$module" ] || [ -z "$bits" ]; then
        error_exit "Usage: $0 mask disable <module|all> <bit_names...>"
    fi


    local disable_bits=$(parse_debug_bits "$bits")
    module=$(echo "$module" | tr '[:upper:]' '[:lower:]')

    case "$module" in
        spi)
            disable_module_bits "$DEBUGFS_SPI" "SPI" "$disable_bits"
            ;;
        core)
            disable_module_bits "$DEBUGFS_CORE" "Core" "$disable_bits"
            ;;
        wlan)
            disable_module_bits "$DEBUGFS_WLAN" "WLAN" "$disable_bits"
            ;;
        mcp)
            disable_module_bits "$DEBUGFS_MCP" "MCP" "$disable_bits"
            ;;
        all)
            disable_module_bits "$DEBUGFS_SPI" "SPI" "$disable_bits"
            disable_module_bits "$DEBUGFS_CORE" "Core" "$disable_bits"
            disable_module_bits "$DEBUGFS_WLAN" "WLAN" "$disable_bits"
            disable_module_bits "$DEBUGFS_MCP" "MCP" "$disable_bits"
            ;;
        *)
            error_exit "Invalid module '$module'. Use: spi, core, wlan, mcp, or all"
            ;;
    esac
}

# Disable bits for a specific module
disable_module_bits() {
    local path=$1
    local module=$2
    local disable_bits=$3

    if check_debugfs "$path" "$module"; then
        local current=$(read_debug_mask "$path")
        local new_mask=$((current & ~disable_bits))

        if write_debug_mask "$path" "$new_mask"; then
            print_msg "$GREEN" "$module: Disabled bits, new mask = $(dec_to_hex $new_mask) ($new_mask) [$(mask_to_text $new_mask)]"
        else
            print_msg "$RED" "$module: Failed to disable bits"
        fi
    fi
}

# Quick preset configurations
cmd_preset() {
    local preset="$1"

    if [ -z "$preset" ]; then
        error_exit "Usage: $0 mask preset <all-on|all-off|errors-only|verbose>"
    fi


    case "$preset" in
        all-on)
            print_msg "$CYAN" "Enabling all debug bits for all modules..."
            cmd_set "all" "$DEBUG_ALL"
            ;;
        all-off)
            print_msg "$CYAN" "Disabling all debug bits for all modules..."
            cmd_set "all" "0"
            ;;
        errors-only)
            print_msg "$CYAN" "Setting errors-only mode (disable all masks, set level to ERR)..."
            cmd_set "all" "0"
            cmd_level_set "ERR"
            ;;
        verbose)
            print_msg "$CYAN" "Setting verbose mode (HIF + WIM + MAC + TX + RX)..."
            local mask=$(parse_debug_bits "HIF WIM MAC TX RX")
            cmd_set "all" "$mask"
            ;;
        *)
            error_exit "Invalid preset '$preset'. Use: all-on, all-off, errors-only, or verbose"
            ;;
    esac

    echo
    cmd_status
}

################################################################################
# SKB Debug Control Functions
################################################################################

# Show SKB debug status
cmd_skb_status() {
    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "SKB Debug Status"
    print_msg "$CYAN" "=========================================="
    echo

    if [ ! -f "$DEBUGFS_SKB_DEBUG" ]; then
        print_msg "$YELLOW" "Warning: Core module not loaded or debugfs not available"
        print_msg "$YELLOW" "         Expected path: $DEBUGFS_SKB_DEBUG"
        return 1
    fi

    local status=$(cat "$DEBUGFS_SKB_DEBUG" 2>/dev/null)

    if [ "$status" = "Y" ]; then
        print_msg "$GREEN" "SKB Debug: ENABLED"
        echo "  Path: $DEBUGFS_SKB_DEBUG"
        echo ""
        print_msg "$BLUE" "SKB statistics tracking is active."
        echo "  View statistics: cat /sys/kernel/debug/nrc_core/skb_stats"
    else
        print_msg "$YELLOW" "SKB Debug: DISABLED"
        echo "  Path: $DEBUGFS_SKB_DEBUG"
        echo ""
        print_msg "$BLUE" "SKB statistics tracking is inactive."
        echo "  Enable with: $0 skb enable"
    fi
}

# Enable SKB debug
cmd_skb_enable() {

    if [ ! -f "$DEBUGFS_SKB_DEBUG" ]; then
        error_exit "Core module not loaded or debugfs not available at $DEBUGFS_SKB_DEBUG"
    fi

    echo "Y" > "$DEBUGFS_SKB_DEBUG" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "SKB Debug ENABLED"
        echo "  SKB allocation/free tracking is now active"
        echo "  View statistics: cat /sys/kernel/debug/nrc_core/skb_stats"
    else
        error_exit "Failed to enable SKB debug"
    fi
}

# Disable SKB debug
cmd_skb_disable() {

    if [ ! -f "$DEBUGFS_SKB_DEBUG" ]; then
        error_exit "Core module not loaded or debugfs not available at $DEBUGFS_SKB_DEBUG"
    fi

    echo "N" > "$DEBUGFS_SKB_DEBUG" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "SKB Debug DISABLED"
        echo "  SKB allocation/free tracking is now inactive"
    else
        error_exit "Failed to disable SKB debug"
    fi
}

################################################################################
# Device Control Commands
################################################################################

# Show device status
cmd_device_status() {
    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "NRC Device Status"
    print_msg "$CYAN" "=========================================="
    echo

    # Credit queue status
    print_msg "$BLUE" "Credit Queue Status:"
    if [ -f "$DEBUGFS_CREDIT" ]; then
        cat "$DEBUGFS_CREDIT" 2>/dev/null
    else
        print_msg "$YELLOW" "  Credit info not available"
    fi
    echo

    # Slot queue status
    print_msg "$BLUE" "Slot Queue Status:"
    if [ -f "$DEBUGFS_SLOT" ]; then
        cat "$DEBUGFS_SLOT" 2>/dev/null
    else
        print_msg "$YELLOW" "  Slot info not available"
    fi
    echo

    # SPI status
    print_msg "$BLUE" "SPI Status:"
    if [ -f "$DEBUGFS_CSPI" ]; then
        local status=$(cat "$DEBUGFS_CSPI" 2>/dev/null)
        if [ -n "$status" ]; then
            print_msg "$GREEN" "  Status: $status"
        else
            print_msg "$YELLOW" "  Unable to read SPI status"
        fi
    else
        print_msg "$YELLOW" "  SPI status not available"
    fi
    echo

    # WLAN Interface Information
    print_msg "$BLUE" "WLAN Interface Information:"
    if [ -f "$DEBUGFS_WLAN_INFO" ]; then
        cat "$DEBUGFS_WLAN_INFO" 2>/dev/null
    else
        print_msg "$YELLOW" "  WLAN info not available"
    fi
}

# Reset device
cmd_device_reset() {

    if [ ! -f "$DEBUGFS_RESET" ]; then
        error_exit "SPI module not loaded or debugfs not available at $DEBUGFS_RESET"
    fi

    print_msg "$YELLOW" "WARNING: This will reset the device hardware!"
    read -p "Continue? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_msg "$CYAN" "Reset cancelled"
        return 0
    fi

    echo 1 > "$DEBUGFS_RESET" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "Device reset triggered successfully"
        echo "  The device has been reset at hardware level"
    else
        error_exit "Failed to reset device"
    fi
}

# Restart device
cmd_device_restart() {

    if [ ! -f "$DEBUGFS_RESTART" ]; then
        error_exit "Core module not loaded or debugfs not available at $DEBUGFS_RESTART"
    fi

    print_msg "$YELLOW" "WARNING: This will restart the network device!"
    read -p "Continue? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_msg "$CYAN" "Restart cancelled"
        return 0
    fi

    echo 1 > "$DEBUGFS_RESTART" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "Network device restart triggered successfully"
        echo "  The network device has been restarted"
    else
        error_exit "Failed to restart device"
    fi
}

################################################################################
# Signal Quality Monitoring Commands
################################################################################

# Show signal quality
cmd_signal_status() {
    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "Signal Quality Status"
    print_msg "$CYAN" "=========================================="
    echo

    # RSSI
    print_msg "$BLUE" "RSSI (Received Signal Strength Indicator):"
    if [ -f "$DEBUGFS_RSSI" ]; then
        local rssi=$(cat "$DEBUGFS_RSSI" 2>/dev/null)
        if [ -n "$rssi" ]; then
            print_msg "$GREEN" "  $rssi dBm"
        else
            print_msg "$YELLOW" "  Not available (not connected?)"
        fi
    else
        print_msg "$YELLOW" "  WLAN module not loaded"
    fi
    echo

    # SNR
    print_msg "$BLUE" "SNR (Signal-to-Noise Ratio):"
    if [ -f "$DEBUGFS_SNR" ]; then
        local snr=$(cat "$DEBUGFS_SNR" 2>/dev/null)
        if [ -n "$snr" ]; then
            print_msg "$GREEN" "  $snr dB"
        else
            print_msg "$YELLOW" "  Not available (not connected?)"
        fi
    else
        print_msg "$YELLOW" "  WLAN module not loaded"
    fi
    echo

    # Expected throughput
    print_msg "$BLUE" "Expected Throughput:"
    if [ -f "$DEBUGFS_TPUT" ]; then
        local tput=$(cat "$DEBUGFS_TPUT" 2>/dev/null)
        if [ -n "$tput" ]; then
            print_msg "$GREEN" "  $tput"
        else
            print_msg "$YELLOW" "  Not available (not connected?)"
        fi
    else
        print_msg "$YELLOW" "  WLAN module not loaded"
    fi
    echo

    # Beacon status
    print_msg "$BLUE" "Beacon Update Status:"
    if [ -f "$DEBUGFS_BEACON" ]; then
        local beacon=$(cat "$DEBUGFS_BEACON" 2>/dev/null)
        if [ -n "$beacon" ]; then
            print_msg "$GREEN" "  $beacon"
        else
            print_msg "$YELLOW" "  Not available"
        fi
    else
        print_msg "$YELLOW" "  WLAN module not loaded"
    fi
}

################################################################################
# Power Management Commands
################################################################################

# Wakeup device
cmd_pm_wakeup() {

    if [ ! -f "$DEBUGFS_WAKEUP" ]; then
        error_exit "WLAN module not loaded or debugfs not available at $DEBUGFS_WAKEUP"
    fi

    echo 1 > "$DEBUGFS_WAKEUP" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "Device wakeup triggered successfully"
        echo "  The device should now be awake"
    else
        error_exit "Failed to trigger device wakeup"
    fi
}

# Sleep device
cmd_pm_sleep() {

    if [ ! -f "$DEBUGFS_SLEEP" ]; then
        error_exit "WLAN module not loaded or debugfs not available at $DEBUGFS_SLEEP"
    fi

    echo 1 > "$DEBUGFS_SLEEP" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "Device sleep triggered successfully"
        echo "  The device should now enter sleep mode"
    else
        error_exit "Failed to trigger device sleep"
    fi
}

################################################################################
# Power Save Control Commands (Core Module)
################################################################################

# Show PS status
cmd_ps_status() {

    if [ ! -f "$DEBUGFS_PS_CONTROL" ]; then
        error_exit "Core module not loaded or debugfs not available at $DEBUGFS_PS_CONTROL"
    fi

    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "NRC Power Save Status (Core Module)"
    print_msg "$CYAN" "=========================================="
    echo ""

    cat "$DEBUGFS_PS_CONTROL" 2>/dev/null
}

################################################################################
# Firmware Control Commands (Core Module)
################################################################################

# Show FW status
cmd_fw_status() {

    if [ ! -f "$DEBUGFS_FW_CONTROL" ]; then
        error_exit "Core module not loaded or debugfs not available at $DEBUGFS_FW_CONTROL"
    fi

    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "NRC Firmware Status"
    print_msg "$CYAN" "=========================================="
    echo ""

    cat "$DEBUGFS_FW_CONTROL" 2>/dev/null
}

# FW unload command (future implementation)
cmd_fw_unload() {

    if [ ! -f "$DEBUGFS_FW_CONTROL" ]; then
        error_exit "Core module not loaded or debugfs not available at $DEBUGFS_FW_CONTROL"
    fi

    print_msg "$YELLOW" "WARNING: This will unload the firmware!"
    print_msg "$YELLOW" "         This feature is not yet implemented."
    read -p "Continue? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_msg "$CYAN" "Unload cancelled"
        return 0
    fi

    echo "unload" > "$DEBUGFS_FW_CONTROL" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "Firmware unload command sent"
        echo "  Check kernel log (dmesg) for results"
    else
        error_exit "Failed to send firmware unload command"
    fi
}

# PS Wake command
cmd_ps_wake() {
    local timeout="${1:-2000}"

    if [ ! -f "$DEBUGFS_PS_CONTROL" ]; then
        error_exit "Core module not loaded or debugfs not available at $DEBUGFS_PS_CONTROL"
    fi

    if ! [[ "$timeout" =~ ^[0-9]+$ ]]; then
        error_exit "Invalid timeout. Must be a positive integer (milliseconds)"
    fi

    print_msg "$CYAN" "Requesting PS wake..."
    echo "  Timeout: $timeout ms"

    echo "wake $timeout" > "$DEBUGFS_PS_CONTROL" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "PS wake request sent successfully"
        echo "  Check kernel log (dmesg) for results"
    else
        error_exit "Failed to send PS wake request"
    fi
}

# PS Reset command - force reset stuck PS state
cmd_ps_reset() {

    if [ ! -f "$DEBUGFS_PS_CONTROL" ]; then
        error_exit "Core module not loaded or debugfs not available at $DEBUGFS_PS_CONTROL"
    fi

    print_msg "$YELLOW" "WARNING: This will forcibly reset PS state to WAKE!"
    print_msg "$YELLOW" "         Use this only when PS is stuck (e.g., WAKING state)"
    echo ""
    read -p "Continue? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        print_msg "$CYAN" "Reset cancelled"
        return 0
    fi

    print_msg "$CYAN" "Forcing PS state reset..."

    echo "reset" > "$DEBUGFS_PS_CONTROL" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "PS state forcibly reset to WAKE"
        echo "  Check kernel log (dmesg) for results"
        echo "  Check current status: $0 ps status"
    else
        error_exit "Failed to reset PS state"
    fi
}

# PS Sleep command
cmd_ps_sleep() {
    local mode="$1"
    local timeout="$2"

    if [ ! -f "$DEBUGFS_PS_CONTROL" ]; then
        error_exit "Core module not loaded or debugfs not available at $DEBUGFS_PS_CONTROL"
    fi

    if [ -z "$mode" ] || [ -z "$timeout" ]; then
        error_exit "Usage: $0 ps sleep <mode> <timeout_ms>\n  Modes: 1=MODEMSLEEP, 2=DEEPSLEEP_TIM, 3=DEEPSLEEP_NONTIM"
    fi

    if ! [[ "$mode" =~ ^[0-9]+$ ]] || [ "$mode" -lt 1 ] || [ "$mode" -gt 3 ]; then
        error_exit "Invalid mode. Must be 1 (MODEMSLEEP), 2 (DEEPSLEEP_TIM), or 3 (DEEPSLEEP_NONTIM)"
    fi

    if ! [[ "$timeout" =~ ^[0-9]+$ ]]; then
        error_exit "Invalid timeout. Must be a positive integer (milliseconds)"
    fi

    local mode_name=""
    case "$mode" in
        1) mode_name="MODEMSLEEP" ;;
        2) mode_name="DEEPSLEEP_TIM" ;;
        3) mode_name="DEEPSLEEP_NONTIM" ;;
    esac

    print_msg "$CYAN" "Requesting PS sleep..."
    echo "  Mode: $mode_name ($mode)"
    echo "  Timeout: $timeout ms"

    echo "sleep $mode $timeout" > "$DEBUGFS_PS_CONTROL" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "PS sleep request sent successfully"
        echo "  Check kernel log (dmesg) for results"
    else
        error_exit "Failed to send PS sleep request"
    fi
}

################################################################################
# Loopback Test Commands (HIF TX/RX throughput measurement)
################################################################################

# Check if loopback debugfs is available
check_loopback_debugfs() {
    if [ ! -d "/sys/kernel/debug/nrc_core/loopback" ]; then
        error_exit "Loopback debugfs not available. Core module may not be loaded."
    fi
}

# Show loopback test status
cmd_loopback_status() {
    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "HIF Loopback Test Status"
    print_msg "$CYAN" "=========================================="
    echo

    check_loopback_debugfs

    # Current settings - read from sample file which now shows config
    print_msg "$BLUE" "Current Configuration:"
    if [ -f "$DEBUGFS_LB_SAMPLE" ]; then
        cat "$DEBUGFS_LB_SAMPLE" 2>/dev/null
    else
        echo "  Not available"
    fi
    echo

    print_msg "$CYAN" "Available Test Modes:"
    echo "  0 - Round-trip: Host TX → FW echo → Host RX (full RTT measurement)"
    echo "  1 - TX only: Host TX → FW (TX throughput + FW arrival time)"
    echo "  2 - RX only: FW generates → Host RX (RX throughput)"
}

# Set sample size for loopback test
cmd_loopback_sample() {
    local size="${1:-1024}"

    check_loopback_debugfs

    if ! [[ "$size" =~ ^[0-9]+$ ]] || [ "$size" -lt 1 ] || [ "$size" -gt 1600 ]; then
        error_exit "Invalid size. Must be between 1 and 1600 bytes"
    fi

    print_msg "$CYAN" "Setting sample size..."
    echo "  Size: $size bytes"

    echo "$size" > "$DEBUGFS_LB_SAMPLE" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "Sample size set to $size bytes"
    else
        error_exit "Failed to set sample size"
    fi
}

# Set loopback frame count
cmd_loopback_count() {
    local count="${1:-100}"

    check_loopback_debugfs

    if ! [[ "$count" =~ ^[0-9]+$ ]] || [ "$count" -lt 1 ] || [ "$count" -gt 65535 ]; then
        error_exit "Invalid count. Must be between 1 and 65535"
    fi

    echo "$count" > "$DEBUGFS_LB_COUNT" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "Frame count set to $count"
    else
        error_exit "Failed to set frame count"
    fi
}

# Run loopback test
cmd_loopback_run() {
    local mode="${1:-0}"

    check_loopback_debugfs

    if ! [[ "$mode" =~ ^[0-9]+$ ]] || [ "$mode" -lt 0 ] || [ "$mode" -gt 2 ]; then
        error_exit "Invalid mode. Must be 0 (round-trip), 1 (TX only), or 2 (RX only)"
    fi

    local mode_name=""
    case "$mode" in
        0) mode_name="Round-trip" ;;
        1) mode_name="TX only" ;;
        2) mode_name="RX only" ;;
    esac

    local count=$(cat "$DEBUGFS_LB_COUNT" 2>/dev/null)

    print_msg "$CYAN" "Running loopback test..."
    echo "  Mode: $mode_name ($mode)"
    echo "  Frame Count: $count"
    echo

    echo "$mode" > "$DEBUGFS_LB_TEST" 2>/dev/null
    if [ $? -eq 0 ]; then
        print_msg "$GREEN" "Loopback test started"
        echo "  Check kernel log (dmesg) for progress"
        echo "  View results: $0 loopback report"
        echo
        # Wait a bit and show report
        sleep 1
        print_msg "$CYAN" "Test Results:"
        cat "$DEBUGFS_LB_REPORT" 2>/dev/null
    else
        error_exit "Failed to start loopback test"
    fi
}

# Show loopback test report
cmd_loopback_report() {
    check_loopback_debugfs

    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "HIF Loopback Test Report"
    print_msg "$CYAN" "=========================================="
    echo

    if [ -f "$DEBUGFS_LB_REPORT" ]; then
        cat "$DEBUGFS_LB_REPORT" 2>/dev/null
    else
        print_msg "$YELLOW" "No report available. Run a test first."
    fi
}

# Enable/disable hex dump
cmd_loopback_hexdump() {
    local enable="${1:-1}"

    check_loopback_debugfs

    if [ "$enable" = "on" ] || [ "$enable" = "1" ] || [ "$enable" = "enable" ]; then
        echo 1 > "$DEBUGFS_LB_HEXDUMP" 2>/dev/null
        print_msg "$GREEN" "Hex dump enabled"
    elif [ "$enable" = "off" ] || [ "$enable" = "0" ] || [ "$enable" = "disable" ]; then
        echo 0 > "$DEBUGFS_LB_HEXDUMP" 2>/dev/null
        print_msg "$GREEN" "Hex dump disabled"
    else
        error_exit "Invalid option. Use: on/off, 1/0, or enable/disable"
    fi
}

# Quick loopback test (all-in-one)
cmd_loopback_quick() {
    local size="${1:-1024}"
    local count="${2:-100}"
    local mode="${3:-0}"

    check_loopback_debugfs

    print_msg "$CYAN" "=========================================="
    print_msg "$CYAN" "Quick Loopback Test"
    print_msg "$CYAN" "=========================================="
    echo

    # Set sample size
    print_msg "$BLUE" "Step 1: Setting sample size ($size bytes)..."
    cmd_loopback_sample "$size"
    echo

    # Set count
    print_msg "$BLUE" "Step 2: Setting frame count ($count)..."
    cmd_loopback_count "$count"
    echo

    # Run test
    print_msg "$BLUE" "Step 3: Running test (mode $mode)..."
    cmd_loopback_run "$mode"
}

# Display comprehensive status of all modules
cmd_driver_status() {
    print_msg "$CYAN" "========================================"
    print_msg "$CYAN" "NRC Driver Status"
    print_msg "$CYAN" "========================================"
    echo ""

    # SPI Module Status
    print_msg "$BLUE" "┌─ SPI Module"
    if [ -f "$DEBUGFS_SPI" ]; then
        local mask=$(cat "$DEBUGFS_SPI" 2>/dev/null)
        print_msg "$GREEN" "│  Debug Mask: 0x$(printf '%08x' $mask) [$(mask_to_text $mask)]"
        print_msg "$GREEN" "│  Status: Loaded"
    else
        print_msg "$YELLOW" "│  Status: Not loaded"
    fi
    print_msg "$BLUE" "└─"
    echo ""

    # Core HAL Module Status
    print_msg "$BLUE" "┌─ Core HAL Module"
    if [ -d "/sys/kernel/debug/nrc_core" ]; then
        local mask=$(cat "$DEBUGFS_CORE" 2>/dev/null)
        local level=$(cat "$DEBUGFS_DEBUG_LEVEL" 2>/dev/null)
        local level_name=""
        case $level in
            0) level_name="ERR" ;;
            1) level_name="WARN" ;;
            2) level_name="INFO" ;;
            3) level_name="DBG" ;;
            *) level_name="UNKNOWN" ;;
        esac

        print_msg "$GREEN" "│  Debug Mask: 0x$(printf '%08x' $mask) [$(mask_to_text $mask)]"
        print_msg "$GREEN" "│  Debug Level: $level ($level_name)"
        print_msg "$GREEN" "│  Status: Loaded"
    else
        print_msg "$YELLOW" "│  Status: Not loaded"
    fi
    print_msg "$BLUE" "└─"
    echo ""

    # Credit Queue Status
    if [ -f "$DEBUGFS_CREDIT" ]; then
        print_msg "$BLUE" "┌─ Credit Queue"
        cat "$DEBUGFS_CREDIT" 2>/dev/null | while IFS= read -r line; do
            print_msg "$GREEN" "│  $line"
        done
        print_msg "$BLUE" "└─"
        echo ""
    fi

    # Slot Queue Status
    if [ -f "$DEBUGFS_SLOT" ]; then
        print_msg "$BLUE" "┌─ Slot Queue"
        cat "$DEBUGFS_SLOT" 2>/dev/null | while IFS= read -r line; do
            print_msg "$GREEN" "│  $line"
        done
        print_msg "$BLUE" "└─"
        echo ""
    fi

    # SKB Statistics
    if [ -f "$DEBUGFS_SKB_STATS" ]; then
        print_msg "$BLUE" "┌─ SKB Statistics"
        cat "$DEBUGFS_SKB_STATS" 2>/dev/null | head -20 | while IFS= read -r line; do
            if echo "$line" | grep -qi "leak"; then
                print_msg "$RED" "│  $line"
            else
                print_msg "$GREEN" "│  $line"
            fi
        done
        print_msg "$BLUE" "└─"
        echo ""
    fi

    # WLAN Module Status
    print_msg "$BLUE" "┌─ WLAN Module"
    if [ -d "/sys/kernel/debug/ieee80211/nrc80211" ]; then
        local mask=$(cat "$DEBUGFS_WLAN" 2>/dev/null)
        print_msg "$GREEN" "│  Debug Mask: 0x$(printf '%08x' $mask) [$(mask_to_text $mask)]"
        print_msg "$GREEN" "│  Status: Loaded"
    else
        print_msg "$YELLOW" "│  Status: Not loaded"
    fi
    print_msg "$BLUE" "└─"
    echo ""

    # WLAN Interface Information
    if [ -f "$DEBUGFS_WLAN_INFO" ]; then
        print_msg "$BLUE" "┌─ WLAN Interface"
        cat "$DEBUGFS_WLAN_INFO" 2>/dev/null | while IFS= read -r line; do
            if echo "$line" | grep -q "==="; then
                print_msg "$CYAN" "│  $line"
            elif echo "$line" | grep -q "S1G"; then
                print_msg "$YELLOW" "│  $line"
            else
                print_msg "$GREEN" "│  $line"
            fi
        done
        print_msg "$BLUE" "└─"
        echo ""
    fi

    # Signal Quality
    if [ -d "/sys/kernel/debug/ieee80211/nrc80211" ]; then
        print_msg "$BLUE" "┌─ Signal Quality"
        local rssi=$(cat "$DEBUGFS_RSSI" 2>/dev/null)
        local snr=$(cat "$DEBUGFS_SNR" 2>/dev/null)
        local beacon=$(cat "$DEBUGFS_BEACON" 2>/dev/null)
        local tput=$(cat "$DEBUGFS_TPUT" 2>/dev/null)

        if [ -n "$rssi" ]; then
            print_msg "$GREEN" "│  RSSI: $rssi dBm"
        fi
        if [ -n "$snr" ]; then
            print_msg "$GREEN" "│  SNR: $snr dB"
        fi
        if [ -n "$beacon" ]; then
            print_msg "$GREEN" "│  Beacon Updated: $beacon"
        fi
        if [ -n "$tput" ]; then
            print_msg "$GREEN" "│  Expected Throughput: $tput"
        fi
        print_msg "$BLUE" "└─"
        echo ""
    fi

    # MCP Module Status
    print_msg "$BLUE" "┌─ MCP Module"
    if [ -f "$DEBUGFS_MCP" ]; then
        local mask=$(cat "$DEBUGFS_MCP" 2>/dev/null)
        print_msg "$GREEN" "│  Debug Mask: 0x$(printf '%08x' $mask) [$(mask_to_text $mask)]"
        print_msg "$GREEN" "│  Status: Loaded"
    else
        print_msg "$YELLOW" "│  Status: Not loaded"
    fi
    print_msg "$BLUE" "└─"
    echo ""

    # Firmware Status
    if [ -f "$DEBUGFS_FW_CONTROL" ]; then
        print_msg "$BLUE" "┌─ Firmware Status"
        cat "$DEBUGFS_FW_CONTROL" 2>/dev/null | while IFS= read -r line; do
            if echo "$line" | grep -q "==="; then
                print_msg "$CYAN" "│  $line"
            elif echo "$line" | grep -qi "state:"; then
                print_msg "$GREEN" "│  $line"
            elif echo "$line" | grep -qi "available"; then
                print_msg "$BLUE" "│  $line"
            else
                print_msg "$GREEN" "│  $line"
            fi
        done
        print_msg "$BLUE" "└─"
        echo ""
    fi

    print_msg "$CYAN" "========================================"
}

# Display help information
cmd_help() {
    echo -e "${CYAN}========================================"
    echo -e "NRC Driver Debug Mask Control Script"
    echo -e "========================================${NC}"
    echo ""
    echo -e "${BLUE}DESCRIPTION:${NC}"
    echo "    This script provides comprehensive control over debug masks for all NRC"
    echo "    driver modules (SPI, Core, WLAN, MCP). Each module has its own debug mask"
    echo "    that controls which debug messages are printed to the kernel log."
    echo ""
    echo -e "${BLUE}USAGE:${NC}"
    echo "    $0 info"
    echo "    $0 mask <command> [options]"
    echo "    $0 level <command> [options]"
    echo "    $0 skb <command>"
    echo "    $0 device <command> [options]"
    echo "    $0 signal"
    echo "    $0 pm <command>"
    echo "    $0 ps <command> [options]"
    echo "    $0 fw <command> [options]"
    echo "    $0 loopback <command> [options]"
    echo "    $0 help"
    echo ""
    echo -e "${BLUE}DRIVER INFO COMMAND:${NC}"
    echo ""
    echo -e "    ${GREEN}info${NC}"
    echo "        Display comprehensive status of all loaded modules."
    echo "        Shows debug masks, levels, credit/slot queues, SKB stats,"
    echo "        WLAN interface info, and signal quality."
    echo ""
    echo "        Example:"
    echo "            $0 info"
    echo ""
    echo -e "${BLUE}DEBUG MASK COMMANDS:${NC}"
    echo ""
    echo -e "    ${GREEN}mask status${NC}"
    echo "        Show current debug mask status for all modules."
    echo ""
    echo "        Example:"
    echo "            $0 mask status"
    echo ""
    echo -e "    ${GREEN}mask list${NC}"
    echo "        List all available debug bit definitions with descriptions."
    echo ""
    echo "        Example:"
    echo "            $0 mask list"
    echo ""
    echo -e "    ${GREEN}mask set <module> <value|bits...>${NC}"
    echo "        Set debug mask for specified module(s) to an exact value."
    echo ""
    echo "        Module: spi, core, wlan, mcp, or all"
    echo "        Value:  Hex value (0x00000001), decimal value (1), or bit names (HIF WIM)"
    echo ""
    echo "        Examples:"
    echo "            $0 mask set all 0x0000000F        # Set all modules to hex value"
    echo "            $0 mask set wlan HIF MAC TX       # Set WLAN to HIF+MAC+TX bits"
    echo "            $0 mask set core 15               # Set Core to decimal value 15"
    echo ""
    echo -e "    ${GREEN}mask enable <module> <bits...>${NC}"
    echo "        Enable (add) specific debug bits to current mask (OR operation)."
    echo ""
    echo "        Module: spi, core, wlan, mcp, or all"
    echo "        Bits:   Space-separated bit names"
    echo ""
    echo "        Examples:"
    echo "            $0 mask enable all HIF WIM       # Enable HIF and WIM bits"
    echo "            $0 mask enable wlan TX RX        # Add TX/RX debug to WLAN module"
    echo ""
    echo -e "    ${GREEN}mask disable <module> <bits...>${NC}"
    echo "        Disable (remove) specific debug bits from current mask (AND NOT operation)."
    echo ""
    echo "        Module: spi, core, wlan, mcp, or all"
    echo "        Bits:   Space-separated bit names"
    echo ""
    echo "        Examples:"
    echo "            $0 mask disable all TX RX        # Disable TX/RX debug on all modules"
    echo "            $0 mask disable core HIF WIM     # Remove HIF and WIM debug from Core"
    echo ""
    echo -e "    ${GREEN}mask preset <name>${NC}"
    echo "        Apply quick preset configurations to all modules."
    echo ""
    echo "        Presets:"
    echo "            all-on        - Enable all debug bits (0xFFFFFFFF)"
    echo "            all-off       - Disable all debug bits (0x00000000)"
    echo "            errors-only   - Disable all masks and set level to ERR (errors only)"
    echo "            verbose       - Enable HIF+WIM+MAC+TX+RX"
    echo ""
    echo "        Examples:"
    echo "            $0 mask preset errors-only"
    echo "            $0 mask preset verbose"
    echo ""
    echo -e "${BLUE}DEBUG LEVEL COMMANDS:${NC}"
    echo ""
    echo -e "    ${GREEN}level status${NC}"
    echo "        Show current debug level for all modules."
    echo "        Debug level controls verbosity independent of category masks."
    echo ""
    echo "        Example:"
    echo "            $0 level status"
    echo ""
    echo -e "    ${GREEN}level set <level>${NC}"
    echo "        Set debug level for all modules."
    echo "        Accepts level name (ERR/WARN/INFO/DBG) or numeric value (0-3)."
    echo ""
    echo "        Levels:"
    echo "            ERR  (0) - Show only error messages"
    echo "            WARN (1) - Show errors and warnings"
    echo "            INFO (2) - Show errors, warnings, and info (default in production)"
    echo "            DBG  (3) - Show all debug messages (default when DEBUG defined)"
    echo ""
    echo "        Examples:"
    echo "            $0 level set DBG       # Set to DBG level (maximum verbosity)"
    echo "            $0 level set INFO      # Set to INFO level"
    echo "            $0 level set 2         # Set to level 2 (INFO)"
    echo ""
    echo -e "${BLUE}SKB DEBUG COMMANDS:${NC}"
    echo ""
    echo -e "    ${GREEN}skb status${NC}"
    echo "        Show current SKB debug status (enabled/disabled)."
    echo ""
    echo "        Example:"
    echo "            $0 skb status"
    echo ""
    echo -e "    ${GREEN}skb enable${NC}"
    echo "        Enable SKB allocation/free tracking."
    echo "        When enabled, the driver tracks all SKB operations for debugging."
    echo ""
    echo "        Example:"
    echo "            $0 skb enable"
    echo ""
    echo -e "    ${GREEN}skb disable${NC}"
    echo "        Disable SKB allocation/free tracking."
    echo "        Disables statistics collection for better performance."
    echo ""
    echo "        Example:"
    echo "            $0 skb disable"
    echo ""
    echo -e "${BLUE}DEVICE CONTROL COMMANDS:${NC}"
    echo ""
    echo -e "    ${GREEN}device status${NC}"
    echo "        Show device status including credit queues and SPI status."
    echo ""
    echo "        Example:"
    echo "            $0 device status"
    echo ""
    echo -e "    ${GREEN}device reset${NC}"
    echo "        Trigger device hardware reset."
    echo "        WARNING: This resets the device at hardware level!"
    echo ""
    echo "        Example:"
    echo "            $0 device reset"
    echo ""
    echo -e "    ${GREEN}device restart${NC}"
    echo "        Trigger network device restart."
    echo "        WARNING: This restarts the network stack!"
    echo ""
    echo "        Example:"
    echo "            $0 device restart"
    echo ""
    echo -e "${BLUE}SIGNAL QUALITY COMMANDS:${NC}"
    echo ""
    echo -e "    ${GREEN}signal${NC}"
    echo "        Show signal quality information (RSSI, SNR, throughput, beacon)."
    echo ""
    echo "        Example:"
    echo "            $0 signal"
    echo ""
    echo -e "${BLUE}POWER MANAGEMENT COMMANDS:${NC}"
    echo ""
    echo -e "    ${GREEN}pm wakeup${NC}"
    echo "        Trigger device wakeup from sleep/power-save mode."
    echo ""
    echo "        Example:"
    echo "            $0 pm wakeup"
    echo ""
    echo -e "    ${GREEN}pm sleep${NC}"
    echo "        Trigger device to enter sleep/power-save mode."
    echo ""
    echo "        Example:"
    echo "            $0 pm sleep"
    echo ""
    echo -e "${BLUE}POWER SAVE CONTROL COMMANDS (Core Module):${NC}"
    echo ""
    echo -e "    ${GREEN}ps status${NC}"
    echo "        Show current power save status from Core module."
    echo "        Displays PS state, mode, enabled flags, and timeout."
    echo ""
    echo "        Example:"
    echo "            $0 ps status"
    echo ""
    echo -e "    ${GREEN}ps wake [timeout_ms]${NC}"
    echo "        Request device wake from power save mode via Core module."
    echo "        Timeout in milliseconds (default: 2000)."
    echo ""
    echo "        Examples:"
    echo "            $0 ps wake          # Wake with default 2000ms timeout"
    echo "            $0 ps wake 5000     # Wake with 5 second timeout"
    echo ""
    echo -e "    ${GREEN}ps sleep <mode> <timeout_ms>${NC}"
    echo "        Request device enter power save mode via Core module."
    echo "        Mode: 1=MODEMSLEEP, 2=DEEPSLEEP_TIM, 3=DEEPSLEEP_NONTIM"
    echo "        Timeout in milliseconds."
    echo ""
    echo "        Examples:"
    echo "            $0 ps sleep 2 1000   # DEEPSLEEP_TIM with 1 second timeout"
    echo "            $0 ps sleep 3 5000   # DEEPSLEEP_NONTIM with 5 second timeout"
    echo ""
    echo -e "${BLUE}FIRMWARE CONTROL COMMANDS (Core Module):${NC}"
    echo ""
    echo -e "    ${GREEN}fw status${NC}"
    echo "        Show current firmware status from Core module."
    echo "        Displays NRC FW state and HW state."
    echo ""
    echo "        Example:"
    echo "            $0 fw status"
    echo ""
    echo -e "    ${GREEN}fw unload${NC}"
    echo "        Unload firmware from device (future implementation)."
    echo "        This feature is planned for future releases."
    echo ""
    echo "        Example:"
    echo "            $0 fw unload"
    echo ""
    echo -e "${BLUE}LOOPBACK TEST COMMANDS (HIF TX/RX throughput):${NC}"
    echo ""
    echo -e "    ${GREEN}loopback status${NC}"
    echo "        Show current loopback test settings and sample data status."
    echo ""
    echo "        Example:"
    echo "            $0 loopback status"
    echo ""
    echo -e "    ${GREEN}loopback sample <size>${NC}"
    echo "        Create sample data for loopback test."
    echo "        Size in bytes (1-1600, default: 1024)."
    echo ""
    echo "        Example:"
    echo "            $0 loopback sample 1024"
    echo ""
    echo -e "    ${GREEN}loopback count <count>${NC}"
    echo "        Set number of frames for loopback test."
    echo "        Count (1-65535, default: 100)."
    echo ""
    echo "        Example:"
    echo "            $0 loopback count 100"
    echo ""
    echo -e "    ${GREEN}loopback run <mode>${NC}"
    echo "        Run loopback test with specified mode."
    echo "        Mode: 0=Round-trip, 1=TX only, 2=RX only (default: 0)"
    echo ""
    echo "        Modes:"
    echo "            0 - Round-trip: Host TX → FW echo → Host RX (full RTT)"
    echo "            1 - TX only: Host TX → FW (TX throughput)"
    echo "            2 - RX only: FW generates → Host RX (RX throughput)"
    echo ""
    echo "        Example:"
    echo "            $0 loopback run 0    # Round-trip test"
    echo ""
    echo -e "    ${GREEN}loopback report${NC}"
    echo "        Show loopback test results (throughput, timing)."
    echo ""
    echo "        Example:"
    echo "            $0 loopback report"
    echo ""
    echo -e "    ${GREEN}loopback hexdump <on|off>${NC}"
    echo "        Enable/disable hex dump output during test."
    echo ""
    echo "        Example:"
    echo "            $0 loopback hexdump on"
    echo ""
    echo -e "    ${GREEN}loopback quick [size] [count] [mode]${NC}"
    echo "        Quick all-in-one loopback test."
    echo "        Creates sample, sets count, and runs test."
    echo ""
    echo "        Example:"
    echo "            $0 loopback quick 1024 100 0"
    echo ""
    echo -e "${BLUE}GENERAL COMMANDS:${NC}"
    echo ""
    echo -e "    ${GREEN}help${NC}"
    echo "        Display this help information."
    echo ""
    echo -e "${BLUE}DEBUG CATEGORY BITS (controlled by mask):${NC}"
    echo "    BASIC          - Basic general debug messages"
    echo "    HIF            - Host Interface operations"
    echo "    WIM            - Wireless Interface Message"
    echo "    TX             - TX operations"
    echo "    RX             - RX operations"
    echo "    MAC            - MAC layer operations"
    echo "    CAPI           - C API operations"
    echo "    PS             - Power save"
    echo "    STATS          - Statistics"
    echo "    STATE          - State machine"
    echo "    BD             - Board data operations"
    echo "    FW             - Firmware state and operations"
    echo "    AMPDU          - A-MPDU aggregation monitoring"
    echo "    CREDIT         - Credit management and updates"
    echo "    SLOT           - TX/RX slot management"
    echo "    BUS            - Backend bus operations (SPI/SDIO/UART)"
    echo ""
    echo -e "${BLUE}DEBUG LEVELS (controlled by level):${NC}"
    echo "    ERR  (0)       - Error messages only (always shown regardless of mask)"
    echo "    WARN (1)       - Warnings and above"
    echo "    INFO (2)       - Information and above (default in production)"
    echo "    DBG  (3)       - All debug messages (default when DEBUG defined)"
    echo ""
    echo -e "${BLUE}MODULES:${NC}"
    echo "    spi   - SPI backend module       (/sys/kernel/debug/nrc_spi/debug_mask)"
    echo "    core  - Core HAL module          (/sys/kernel/debug/nrc_core/debug_mask)"
    echo "    wlan  - WLAN frontend module     (/sys/kernel/debug/ieee80211/nrc80211/debug_mask)"
    echo "    mcp   - MCP frontend module      (/sys/kernel/debug/nrc_mcp/debug_mask)"
    echo "    all   - All modules"
    echo ""
    echo -e "${BLUE}EXAMPLES:${NC}"
    echo "    # Show comprehensive driver info"
    echo "    $0 info"
    echo ""
    echo "    # Check current debug mask status"
    echo "    $0 mask status"
    echo ""
    echo "    # List all available debug bits"
    echo "    $0 mask list"
    echo ""
    echo "    # Enable errors only on all modules"
    echo "    $0 mask preset errors-only"
    echo ""
    echo "    # Enable verbose debugging on WLAN module"
    echo "    $0 mask set wlan HIF WIM MAC TX RX"
    echo ""
    echo "    # Add TX/RX debug to all modules"
    echo "    $0 mask enable all TX RX"
    echo ""
    echo "    # Disable HIF and WIM debug on Core module"
    echo "    $0 mask disable core HIF WIM"
    echo ""
    echo "    # Set Core module to exact hex value"
    echo "    $0 mask set core 0x0000003F"
    echo ""
    echo "    # Enable all debugging (maximum verbosity)"
    echo "    $0 mask preset all-on"
    echo ""
    echo "    # Check current debug level"
    echo "    $0 level status"
    echo ""
    echo "    # Set debug level to maximum verbosity (DBG)"
    echo "    $0 level set DBG"
    echo ""
    echo "    # Set debug level to INFO (default in production)"
    echo "    $0 level set INFO"
    echo ""
    echo "    # Check SKB debug status"
    echo "    $0 skb status"
    echo ""
    echo "    # Enable SKB tracking for memory debugging"
    echo "    $0 skb enable"
    echo ""
    echo "    # View SKB statistics"
    echo "    cat /sys/kernel/debug/nrc_core/skb_stats"
    echo ""
    echo "    # Disable SKB tracking"
    echo "    $0 skb disable"
    echo ""
    echo "    # Check device status (credit queues, SPI)"
    echo "    $0 device status"
    echo ""
    echo "    # Reset device hardware"
    echo "    $0 device reset"
    echo ""
    echo "    # Restart network device"
    echo "    $0 device restart"
    echo ""
    echo "    # Run loopback throughput test"
    echo "    $0 loopback quick"
    echo ""
    echo "    # Check signal quality"
    echo "    $0 signal"
    echo ""
    echo "    # Wakeup device from sleep"
    echo "    $0 pm wakeup"
    echo ""
    echo "    # Put device to sleep"
    echo "    $0 pm sleep"
    echo ""
    echo "    # Check PS status (Core module)"
    echo "    $0 ps status"
    echo ""
    echo "    # Wake device via Core module"
    echo "    $0 ps wake 3000"
    echo ""
    echo "    # Sleep device via Core module (DEEPSLEEP_TIM, 1 second)"
    echo "    $0 ps sleep 2 1000"
    echo ""
    echo "    # Check firmware status"
    echo "    $0 fw status"
    echo ""
    echo "    # Unload firmware (future feature)"
    echo "    $0 fw unload"
    echo ""
    echo "    # Quick loopback test (1KB, 100 frames, round-trip)"
    echo "    $0 loopback quick 1024 100 0"
    echo ""
    echo "    # Manual loopback test steps"
    echo "    $0 loopback sample 1024   # Create 1KB sample"
    echo "    $0 loopback count 100     # Set 100 frames"
    echo "    $0 loopback run 0         # Run round-trip test"
    echo "    $0 loopback report        # View results"
    echo ""
    echo "    # View kernel messages in real-time"
    echo "    dmesg -wH"
    echo ""
    echo -e "${BLUE}NOTES:${NC}"
    echo "    - This script must be run as root (su mode)"
    echo "    - Changes take effect immediately"
    echo "    - Debug messages appear in kernel log (dmesg)"
    echo "    - Each module maintains its own independent debug mask"
    echo "    - Not all modules may be loaded at runtime"
    echo ""
    echo -e "${BLUE}AUTHOR:${NC}"
    echo "    Newracom, Inc."
    echo ""
    echo -e "${BLUE}VERSION:${NC}"
    echo "    1.0.0 (2025)"
    echo ""
}

################################################################################
# Main Script Entry Point
################################################################################

# Check if running as root (except for help command)
if [ "$EUID" -ne 0 ] && [ "${1}" != "help" ] && [ "${1}" != "--help" ] && [ "${1}" != "-h" ]; then
    print_msg "$YELLOW" "This script requires root privileges."
    print_msg "$YELLOW" "Re-running with sudo..."
    echo
    exec sudo "$0" "$@"
fi

# Parse main command
MAIN_COMMAND="${1:-help}"

case "$MAIN_COMMAND" in
    mask)
        # Parse mask subcommand
        SUBCOMMAND="${2:-status}"
        shift 2

        case "$SUBCOMMAND" in
            status|show)
                cmd_status
                ;;
            list|bits)
                cmd_list_bits
                ;;
            set)
                cmd_set "$@"
                ;;
            enable|add)
                cmd_enable "$@"
                ;;
            disable|remove)
                cmd_disable "$@"
                ;;
            preset)
                cmd_preset "$@"
                ;;
            help|--help|-h)
                cmd_help
                ;;
            *)
                error_exit "Unknown mask command '$SUBCOMMAND'. Use '$0 help' for usage information."
                ;;
        esac
        ;;
    skb)
        # Parse skb subcommand
        SUBCOMMAND="${2:-status}"
        shift 2

        case "$SUBCOMMAND" in
            status|show)
                cmd_skb_status
                ;;
            enable|on)
                cmd_skb_enable
                ;;
            disable|off)
                cmd_skb_disable
                ;;
            help|--help|-h)
                cmd_help
                ;;
            *)
                error_exit "Unknown skb command '$SUBCOMMAND'. Use '$0 help' for usage information."
                ;;
        esac
        ;;
    device)
        # Parse device subcommand
        SUBCOMMAND="${2:-status}"
        shift 2

        case "$SUBCOMMAND" in
            status)
                cmd_device_status
                ;;
            reset)
                cmd_device_reset
                ;;
            restart)
                cmd_device_restart
                ;;
            help|--help|-h)
                cmd_help
                ;;
            *)
                error_exit "Unknown device command '$SUBCOMMAND'. Use '$0 help' for usage information."
                ;;
        esac
        ;;
    signal)
        # Show signal quality
        cmd_signal_status
        ;;
    pm)
        # Parse power management subcommand
        SUBCOMMAND="${2:-help}"
        shift 2

        case "$SUBCOMMAND" in
            wakeup|wake)
                cmd_pm_wakeup
                ;;
            sleep)
                cmd_pm_sleep
                ;;
            help|--help|-h)
                cmd_help
                ;;
            *)
                error_exit "Unknown pm command '$SUBCOMMAND'. Use '$0 help' for usage information."
                ;;
        esac
        ;;
    ps)
        # Parse power save control subcommand
        SUBCOMMAND="${2:-status}"
        shift 2

        case "$SUBCOMMAND" in
            status)
                cmd_ps_status
                ;;
            wake|wakeup)
                cmd_ps_wake "$@"
                ;;
            sleep)
                cmd_ps_sleep "$@"
                ;;
            help|--help|-h)
                cmd_help
                ;;
            *)
                error_exit "Unknown ps command '$SUBCOMMAND'. Use '$0 help' for usage information."
                ;;
        esac
        ;;
    fw)
        # Parse firmware control subcommand
        SUBCOMMAND="${2:-status}"
        shift 2

        case "$SUBCOMMAND" in
            status)
                cmd_fw_status
                ;;
            unload)
                cmd_fw_unload
                ;;
            help|--help|-h)
                cmd_help
                ;;
            *)
                error_exit "Unknown fw command '$SUBCOMMAND'. Use '$0 help' for usage information."
                ;;
        esac
        ;;
    loopback|lb)
        # Parse loopback test subcommand
        SUBCOMMAND="${2:-status}"
        shift 2

        case "$SUBCOMMAND" in
            status)
                cmd_loopback_status
                ;;
            sample)
                cmd_loopback_sample "$@"
                ;;
            count)
                cmd_loopback_count "$@"
                ;;
            run|test)
                cmd_loopback_run "$@"
                ;;
            report)
                cmd_loopback_report
                ;;
            hexdump)
                cmd_loopback_hexdump "$@"
                ;;
            quick)
                cmd_loopback_quick "$@"
                ;;
            help|--help|-h)
                cmd_help
                ;;
            *)
                error_exit "Unknown loopback command '$SUBCOMMAND'. Use '$0 help' for usage information."
                ;;
        esac
        ;;
    level)
        # Parse level subcommand
        SUBCOMMAND="${2:-status}"
        shift 2

        case "$SUBCOMMAND" in
            status|show)
                cmd_level_status
                ;;
            set)
                cmd_level_set "$@"
                ;;
            help|--help|-h)
                cmd_help
                ;;
            *)
                error_exit "Unknown level command '$SUBCOMMAND'. Use '$0 help' for usage information."
                ;;
        esac
        ;;
    info)
        cmd_driver_status
        ;;
    help|--help|-h)
        cmd_help
        ;;
    *)
        error_exit "Unknown command '$MAIN_COMMAND'. Use '$0 help' for usage information."
        ;;
esac

exit 0
