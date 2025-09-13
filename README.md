# Tripwire - Door Monitoring System

A door monitoring device built with ESP32 that tracks when a door is opened or closed using a magnetic reed switch and sends real-time alerts over WiFi to a TCP server.

## Project Structure

```
Tripwire/
├── Client/          # ESP32 firmware
│   ├── main/
│   │   ├── door_monitor.c    # Main application code
│   │   └── CMakeLists.txt
│   └── CMakeLists.txt
└── Server/          # TCP server implementation
    └── tcp_server.py        # Python TCP server
```

## Hardware Requirements

- ESP32 development board
- Magnetic reed switch
- Jumper wires
- WiFi network for connectivity

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

- **Door State Detection**: Monitors reed switch to detect door open/closed states
- **WiFi Connectivity**: Connects to WiFi network for remote communication
- **TCP Communication**: Sends JSON messages to a remote server
- **State Change Logging**: Only triggers actions when door state changes
- **Visual Feedback**: LED blinks once for door open, twice for door closed
- **Serial Logging**: Outputs door state changes to serial monitor
- **Real-time Alerts**: Sends `{"STATUS":"OPENED"}` or `{"STATUS":"CLOSED"}` messages
- **Efficient Polling**: 100ms polling interval for responsive detection

## Signal Logic

- **HIGH (1)**: Door is open
- **LOW (0)**: Door is closed

## Getting Started

### Prerequisites

- ESP-IDF framework installed
- ESP32 development board
- USB cable for programming and power
- Python 3.x for the server
- WiFi network access

### Configuration

1. **ESP32 Configuration**: Navigate to the Client directory and run menuconfig:
   ```bash
   cd Client
   idf.py menuconfig
   ```

2. **Configure WiFi and Server Settings**:
   - Navigate to `Door Monitor Configuration`
   - Set your WiFi SSID
   - Set your WiFi password
   - Set your server IP address
   - Set your server port (default: 8080)

   **Alternative 1**: Create a local config file `Client/sdkconfig.local`:
   ```
   CONFIG_DOOR_WIFI_SSID="YourWiFiNetwork"
   CONFIG_DOOR_WIFI_PASSWORD="YourPassword"
   CONFIG_DOOR_SERVER_IP="192.168.1.100"
   CONFIG_DOOR_SERVER_PORT=8080
   ```
   
   **Alternative 2**: Non-interactive configuration using environment variables:
   ```bash
   cd Client
   export CONFIG_DOOR_WIFI_SSID="YourWiFiNetwork"
   export CONFIG_DOOR_WIFI_PASSWORD="YourPassword"
   export CONFIG_DOOR_SERVER_IP="192.168.1.100"
   export CONFIG_DOOR_SERVER_PORT=8080
   idf.py reconfigure
   ```
   
   **Note**: `sdkconfig.local` is gitignored for security and won't be committed to your repository.

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

### Running the Server

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
I (456) DOOR_SENSOR: Door monitoring system started. Monitoring GPIO 23 for door state changes.
I (789) DOOR_SENSOR: Door Opened!
I (890) DOOR_SENSOR: Message sent: {"STATUS":"OPENED"}
I (1234) DOOR_SENSOR: Door Closed!
I (1345) DOOR_SENSOR: Message sent: {"STATUS":"CLOSED"}
```

**Server Output:**
```
Starting Door Monitor TCP Server on 0.0.0.0:8080
Waiting for ESP32 connections...
[2023-12-15 14:30:15] Connection from ('192.168.1.50', 54321)
[2023-12-15 14:30:20] Door Status: OPENED
[2023-12-15 14:30:25] Door Status: CLOSED
```

## Architecture

The system consists of two main components:

1. **ESP32 Client**: 
   - Monitors door state via reed switch
   - Connects to WiFi network
   - Sends JSON status messages via TCP

2. **Python Server**:
   - Listens for TCP connections on port 8080
   - Receives and processes JSON door status messages
   - Logs all door events with timestamps
   - Supports multiple concurrent ESP32 connections

## Future Enhancements

- Database storage for historical data
- Web dashboard for monitoring
- Email/SMS notifications
- Mobile app integration
- Multiple door support
- Battery optimization for portable use

## License

This project is open source and available under the MIT License.