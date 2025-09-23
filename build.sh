#!/bin/bash

# Helper script to build with environment variables
# Usage: ./build.sh [build|flash|monitor|flash monitor]

# Source environment variables
if [ -f .env ]; then
    echo "Loading environment variables from .env..."
    source .env
else
    echo "Warning: .env file not found. Please create it with your WiFi credentials."
    exit 1
fi

# Check if required variables are set
if [ -z "$DOOR_WIFI_SSID" ] || [ -z "$DOOR_WIFI_PASSWORD" ] || [ -z "$DOOR_PHONE_BT_MAC" ] || [ -z "$DOOR_NTFY_URL" ]; then
    echo "Error: Missing required environment variables in .env file"
    echo "Required: DOOR_WIFI_SSID, DOOR_WIFI_PASSWORD, DOOR_PHONE_BT_MAC, DOOR_NTFY_URL"
    exit 1
fi

echo "Building with credentials for SSID: $DOOR_WIFI_SSID"
echo "Phone MAC: $DOOR_PHONE_BT_MAC"

# Generate sdkconfig.defaults from template with environment variables
echo "Generating sdkconfig.defaults from template..."
sed -e "s/__WIFI_SSID__/$DOOR_WIFI_SSID/g" \
    -e "s/__WIFI_PASSWORD__/$DOOR_WIFI_PASSWORD/g" \
    -e "s/__PHONE_BT_MAC__/$DOOR_PHONE_BT_MAC/g" \
    -e "s|__NTFY_URL__|$DOOR_NTFY_URL|g" \
    sdkconfig.defaults.template > sdkconfig.defaults.tmp

# Replace the original with the populated version
mv sdkconfig.defaults.tmp sdkconfig.defaults

# Remove old sdkconfig to force regeneration from updated defaults
if [ -f sdkconfig ]; then
    echo "Removing old sdkconfig to regenerate with new defaults..."
    rm sdkconfig
fi

# Run idf.py with the provided arguments (default to 'build')
if [ $# -eq 0 ]; then
    idf.py build
else
    idf.py "$@"
fi