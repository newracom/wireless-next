#!/bin/bash
# Remote Target Build Script using rsync
# Sync local source to target device and build kernel modules
# Usage: ./remote-build.sh [target_ip] [target_user] [target_password] [debug_option] [deploy_path]
#   target_ip: IP address (default: 192.168.0.4, "custom" for manual input)
#   target_user: username (default: pi, "custom" for manual input)
#   target_password: password (default: raspberry, "custom" for manual input)
#   debug_option: "release" or "debug" (default: release)
#   deploy_path: deployment path on target (default: none, use "-" to skip, or path to deploy)

set -e

# Default configuration
DEFAULT_IP="192.168.0.4"
DEFAULT_USER="pi"
DEFAULT_PASS="raspberry"
DEFAULT_DEBUG="release"
DEFAULT_DEPLOY_PATH="/home/pi/nrc_pkg/sw/driver"

# Parse IP address
TARGET_IP="${1:-$DEFAULT_IP}"
if [ "$TARGET_IP" = "custom" ]; then
    read -p "Enter target IP address: " TARGET_IP
    TARGET_IP="${TARGET_IP:-$DEFAULT_IP}"
fi

# Parse username
TARGET_USER="${2:-$DEFAULT_USER}"
if [ "$TARGET_USER" = "custom" ]; then
    read -p "Enter target username: " TARGET_USER
    TARGET_USER="${TARGET_USER:-$DEFAULT_USER}"
fi

# Parse password
TARGET_PASS="${3:-$DEFAULT_PASS}"
if [ "$TARGET_PASS" = "custom" ]; then
    read -sp "Enter target password: " TARGET_PASS
    echo ""
    TARGET_PASS="${TARGET_PASS:-$DEFAULT_PASS}"
fi

# Parse debug option
DEBUG_OPTION="${4:-$DEFAULT_DEBUG}"
if [ "$DEBUG_OPTION" = "custom" ]; then
    read -p "Enter debug option (release/debug): " DEBUG_OPTION
    DEBUG_OPTION="${DEBUG_OPTION:-$DEFAULT_DEBUG}"
fi

# Set DEBUG flag based on option
if [ "$DEBUG_OPTION" = "debug" ]; then
    MAKE_DEBUG="DEBUG=1"
else
    MAKE_DEBUG=""
fi

# Parse deploy path
DEPLOY_PATH="${5}"
if [ "$DEPLOY_PATH" = "custom" ]; then
    read -p "Enter deploy path on target: " DEPLOY_PATH
    DEPLOY_PATH="${DEPLOY_PATH:-$DEFAULT_DEPLOY_PATH}"
elif [ "$DEPLOY_PATH" = "-" ] || [ -z "$DEPLOY_PATH" ]; then
    DEPLOY_PATH=""
fi

LOCAL_SOURCE="$(cd "$(dirname "$0")/.." && pwd)"
REMOTE_SOURCE="/tmp/nrc_modular"

echo "NRC Modular Remote Build System"
echo "================================"
echo "Local source: $LOCAL_SOURCE"
echo "Target: $TARGET_USER@$TARGET_IP"
echo "Remote path: $REMOTE_SOURCE"
echo "Debug option: $DEBUG_OPTION"
if [ -n "$DEPLOY_PATH" ]; then
    echo "Deploy path: $DEPLOY_PATH"
else
    echo "Deploy path: (none - modules will only be fetched locally)"
fi
echo ""

# Check sshpass installation
if ! command -v sshpass &> /dev/null; then
    echo "[INFO] Installing sshpass..."
    sudo apt-get install -y sshpass
fi

# Test SSH connection
echo "[INFO] Testing SSH connection..."
if ! sshpass -p "$TARGET_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$TARGET_USER@$TARGET_IP" "echo 'Connected'" 2>/dev/null; then
    echo "[ERROR] SSH connection failed: $TARGET_USER@$TARGET_IP"
    exit 1
fi
echo "[OK] SSH connection successful"

# Sync source to target using rsync
echo ""
echo "[INFO] Syncing source to target..."
sshpass -p "$TARGET_PASS" rsync -avz --delete \
    --exclude='.git' \
    --exclude='*.o' \
    --exclude='*.ko' \
    --exclude='*.mod' \
    --exclude='*.mod.c' \
    --exclude='*.mod.o' \
    --exclude='.*.cmd' \
    --exclude='.*.o.d' \
    --exclude='*.d' \
    --exclude='.tmp_versions' \
    --exclude='Module.symvers' \
    --exclude='modules.order' \
    --exclude='build_output' \
    --exclude='docker' \
    --exclude='docs' \
    --exclude='scripts' \
    --exclude='.vscode' \
    --exclude='.idea' \
    --exclude='.cache' \
    --exclude='__pycache__' \
    --exclude='*.swp' \
    --exclude='*.swo' \
    --exclude='.DS_Store' \
    --exclude='.clang-format' \
    --exclude='.clang-tidy' \
    --exclude='.gitignore' \
    --exclude='.gitattributes' \
    --exclude='.editorconfig' \
    --exclude='*.md' \
    --exclude='*.code-workspace' \
    -e "ssh -o StrictHostKeyChecking=no" \
    "$LOCAL_SOURCE/" "$TARGET_USER@$TARGET_IP:$REMOTE_SOURCE/"

echo "[OK] Source sync completed"

# Build on target
echo ""
echo "[INFO] Building on target..."
sshpass -p "$TARGET_PASS" ssh -o StrictHostKeyChecking=no "$TARGET_USER@$TARGET_IP" bash << REMOTE_BUILD
set -e
cd $REMOTE_SOURCE
make clean 2>/dev/null || true
make $MAKE_DEBUG

echo ""
echo "[OK] Build completed!"
echo ""
echo "Built modules:"
find . -name "*.ko" -exec ls -lh {} \;
REMOTE_BUILD

# Deploy modules if deploy path is specified
if [ -n "$DEPLOY_PATH" ]; then
    echo ""
    echo "[INFO] Deploying modules to $DEPLOY_PATH..."
    sshpass -p "$TARGET_PASS" ssh -o StrictHostKeyChecking=no "$TARGET_USER@$TARGET_IP" bash << REMOTE_DEPLOY
set -e
mkdir -p $DEPLOY_PATH
cd $REMOTE_SOURCE
find . -name "*.ko" -exec cp -v {} $DEPLOY_PATH/ \;
echo ""
echo "[OK] Modules deployed to $DEPLOY_PATH"
echo ""
echo "Deployed modules:"
ls -lh $DEPLOY_PATH/*.ko
REMOTE_DEPLOY
fi

# Fetch built modules back to local (same directory structure)
# NOTE: Commented out as modules are now deployed directly on target
# Uncomment if you need to fetch .ko files back to local machine
# echo ""
# echo "[INFO] Fetching built modules to local..."
# sshpass -p "$TARGET_PASS" rsync -avz \
#     --include='*/' \
#     --include='*.ko' \
#     --exclude='*' \
#     -e "ssh -o StrictHostKeyChecking=no" \
#     "$TARGET_USER@$TARGET_IP:$REMOTE_SOURCE/" "$LOCAL_SOURCE/"
# echo ""
# echo "Built modules (local):"
# find "$LOCAL_SOURCE" -name "*.ko" -exec ls -lh {} \;

echo ""
echo "================================"
echo "Remote build completed!"
echo ""
