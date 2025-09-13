# Tripwire - Door Monitoring System

A battery-optimized door monitoring device built with ESP32 that tracks when a door is opened or closed using a magnetic reed switch. Features intelligent event batching, NTP time synchronization, automatic WiFi reconnection, and sends push notifications directly to your phone via ntfy.sh.

## Project Structure

```
Tripwire/
├── Client/          # ESP32 firmware
│   ├── main/
│   │   ├── door_monitor.c     # Main application code
│   │   ├── Kconfig.projbuild  # Configuration menu
│   │   └── CMakeLists.txt
│   ├── sdkconfig.defaults     # Default configuration (safe for git)
│   └── CMakeLists.txt
└── Server/          # Legacy TCP server (optional)
    └── tcp_server.py        # Python TCP server
```

## Hardware Requirements

- ESP32 development board (2.4GHz WiFi supported)
- Magnetic reed switch
- Jumper wires
- WiFi network with internet access (for NTP sync and ntfy.sh)
- Battery pack (optional, for portable operation)

## Pin Connections

| Component | ESP32 Pin |
|-----------|-----------|
| Reed Switch | GPIO 23 |
| Built-in LED | GPIO 2 |

## Hardware Setup

1. Connect the magnetic reed switch to GPIO 23
2. The reed switch uses the ESP32's internal pull-up resistor
3. The built-in LED on GPIO 2 provides visual feedback

## Features

### **Core Functionality**
- **Door State Detection**: Monitors reed switch to detect door open/closed states
- **Visual Feedback**: LED blinks once for door open, twice for door closed
- **State Change Logging**: Only triggers actions when door state changes

### **Smart Event Processing**
- **Intelligent Event Batching**: Automatically combines OPEN→CLOSE pairs
- **Edge Case Handling**: Properly handles complex sequences (OPEN→CLOSE→OPEN)
- **60-Second Batching Window**: Reduces notification spam by ~50%

### **Connectivity & Time**
- **WiFi Auto-Reconnection**: Continuous monitoring with automatic reconnection
- **NTP Time Synchronization**: Real local timestamps (configurable timezone)
- **Offline Message Queuing**: No events lost during WiFi outages

### **Push Notifications**
- **ntfy.sh Integration**: Direct push notifications to your phone
- **Custom Priority Levels**: min/low/default/high/max notification importance
- **Rich Notifications**: Includes timestamps, duration, and emoji indicators

### **Battery Optimization**
- **Efficient Polling**: 100ms polling interval for responsive detection
- **Ready for Deep Sleep**: Architecture prepared for battery-powered operation
- **Smart Sync Strategy**: Minimal WiFi usage for maximum battery life

### **Security & Configuration**
- **Secure Configuration**: WiFi credentials and ntfy URLs never committed to git
- **Environment Variable Support**: Easy CI/CD and automated builds
- **Multiple Config Methods**: menuconfig, environment variables, or config files

## Signal Logic

- **HIGH (1)**: Door is open
- **LOW (0)**: Door is closed

## Getting Started

### Prerequisites

- ESP-IDF framework installed (v5.x recommended)
- ESP32 development board
- USB cable for programming and power
- WiFi network with internet access
- ntfy.sh topic URL (free service)

### Configuration

1. **ESP32 Configuration**: Navigate to the Client directory and run menuconfig:
   ```bash
   cd Client
   idf.py menuconfig
   ```

2. **Configure Settings in menuconfig**:
   - Navigate to `Door Monitor Configuration`
   - **WiFi Settings**:
     - Set your WiFi SSID
     - Set your WiFi password
   - **ntfy.sh Settings**:
     - Set your ntfy.sh topic URL (e.g., `https://ntfy.sh/my_door_sensor_2024`)
     - Choose notification priority level
   - **Optional Legacy Server** (if using Python server):
     - Set server IP address
     - Set server port (default: 8080)

   **Alternative 1**: Create a local config file `Client/sdkconfig.local`:
   ```
   CONFIG_DOOR_WIFI_SSID="YourWiFiNetwork"
   CONFIG_DOOR_WIFI_PASSWORD="YourPassword"
   CONFIG_DOOR_NTFY_URL="https://ntfy.sh/your_unique_topic"
   CONFIG_DOOR_NTFY_PRIORITY_DEFAULT=y
   ```
   
   **Alternative 2**: Non-interactive configuration using environment variables:
   ```bash
   cd Client
   export CONFIG_DOOR_WIFI_SSID="YourWiFiNetwork"
   export CONFIG_DOOR_WIFI_PASSWORD="YourPassword"
   export CONFIG_DOOR_NTFY_URL="https://ntfy.sh/your_unique_topic"
   export CONFIG_DOOR_NTFY_PRIORITY_VALUE="default"
   idf.py reconfigure
   ```
   
   **Note**: `sdkconfig` and `sdkconfig.local` are gitignored for security.

### Building and Flashing ESP32

1. Navigate to the Client directory:
   ```bash
   cd Client
   ```

2. Build the project:
   ```bash
   idf.py build
   ```

3. Flash to ESP32 and monitor:
   ```bash
   idf.py flash monitor
   ```

### Setting Up ntfy.sh Notifications

1. **Create a unique topic**: Choose a unique topic name (e.g., `my_door_sensor_2024`)
2. **Subscribe on your phone**:
   - Install ntfy app from App Store/Google Play
   - Subscribe to your topic: `https://ntfy.sh/your_unique_topic`
3. **Configure ESP32**: Use the full URL in your ESP32 configuration

### Optional: Running Legacy TCP Server

If you prefer using the Python server instead of ntfy.sh:

1. Navigate to the Server directory:
   ```bash
   cd Server
   ```

2. Run the Python TCP server:
   ```bash
   python3 tcp_server.py
   ```

The server will listen on port 8080 and display door status messages as they arrive.

### Example Output

**ESP32 Serial Monitor:**
```
I (123) DOOR_SENSOR: connected to ap SSID:YourWiFiNetwork
I (456) DOOR_SENSOR: Time synced successfully: Thu Dec 15 14:30:15 2023
I (567) DOOR_SENSOR: Door monitoring system with NTP sync and event batching started.
I (789) DOOR_SENSOR: Door Opened!
I (890) DOOR_SENSOR: Added event to batch: OPEN (buffer size: 1)
I (1234) DOOR_SENSOR: Door Closed!
I (1345) DOOR_SENSOR: Complete pair detected, processing immediately
I (1400) DOOR_SENSOR: Notification sent immediately via ntfy.sh
```

**Phone Notification (via ntfy.sh):**
```
📱 Door Monitor
🚪 Door opened & closed (14:32-14:34) - 2 min duration
Tags: door,security
```

**Batching Examples:**
- **Simple pair**: `🚪 Door opened & closed (14:32-14:34) - 2 min duration`
- **Solo event**: `🚪 Door opened at 14:32` (after 60s timeout)
- **Edge case**: OPEN→CLOSE→OPEN = Pair notification + Solo notification

## Architecture

### **ESP32 Client (Main Component)**
- **Hardware Interface**: Monitors reed switch on GPIO 23, LED feedback on GPIO 2
- **WiFi Management**: Auto-connect with continuous reconnection monitoring
- **Time Synchronization**: NTP sync with configurable timezone support
- **Event Processing**: 60-second batching window with intelligent OPEN/CLOSE pairing
- **Notification System**: HTTP POST requests to ntfy.sh with rich formatting
- **Offline Resilience**: Message queuing during WiFi outages, replay on reconnect

### **ntfy.sh Integration**
- **Push Notifications**: Direct delivery to phone via ntfy.sh service
- **Rich Formatting**: Timestamps, duration calculations, emoji indicators
- **Priority Levels**: Configurable notification importance (min/low/default/high/max)
- **Reliable Delivery**: Automatic retry with exponential backoff

### **Optional Python Server (Legacy)**
- **TCP Listener**: Port 8080 for JSON message reception
- **Multi-client Support**: Handles multiple ESP32 connections
- **Message Acknowledgment**: Confirms receipt to ESP32 client
- **Event Logging**: Timestamped door events with duration tracking

## Power Management & Battery Life

The system is designed for battery-powered operation:

- **Current Implementation**: Always-on WiFi monitoring (~80mA average)
- **Planned Enhancement**: Deep sleep between events (~10µA idle, 5-10s wake time)
- **Battery Recommendations**: 
  - **Always-on**: 10,000mAh power bank = ~5 days
  - **With deep sleep**: 18650 battery = several months
  - **Solar option**: Small solar panel for indefinite operation

## Configuration Security

All sensitive information is kept secure:

- **WiFi credentials**: Never committed to repository
- **ntfy.sh URLs**: Stored in gitignored `sdkconfig` files  
- **Environment variables**: Support for CI/CD pipelines
- **Multiple config methods**: Choose what works for your workflow

## Timezone Configuration

Current timezone setting can be changed in `main/door_monitor.c`:
- **Pacific Time**: `"PST8PDT,M3.2.0/2,M11.1.0"` (default)
- **Eastern Time**: `"EST5EDT,M3.2.0,M11.1.0"`
- **Central Time**: `"CST6CDT,M3.2.0,M11.1.0"`
- **UTC**: `"UTC0"`

## Future Enhancements

- **Deep sleep implementation** for maximum battery life
- **Multiple sensor support** for doors, windows, cabinets
- **Encrypted communication** for enhanced security
- **Local web dashboard** for configuration and monitoring
- **Database integration** for historical data analysis
- **GPS location tracking** for multi-site deployments

## License

This project is licensed under the BSD 3-Clause License. See the [LICENSE](LICENSE) file for details.