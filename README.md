# Tripwire - Door Monitoring System

A simple door monitoring device built with ESP32 that tracks when a door is opened or closed using a magnetic reed switch.

## Project Structure

```
Tripwire/
├── Client/          # ESP32 firmware
│   ├── main/
│   │   ├── door_monitor.c    # Main application code
│   │   └── CMakeLists.txt
│   └── CMakeLists.txt
└── Server/          # Future server implementation
```

## Hardware Requirements

- ESP32 development board
- Magnetic reed switch
- Jumper wires

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
- **State Change Logging**: Only triggers actions when door state changes
- **Visual Feedback**: LED blinks once for door open, twice for door closed
- **Serial Logging**: Outputs door state changes to serial monitor
- **Efficient Polling**: 100ms polling interval for responsive detection

## Signal Logic

- **HIGH (1)**: Door is open
- **LOW (0)**: Door is closed

## Getting Started

### Prerequisites

- ESP-IDF framework installed
- ESP32 development board
- USB cable for programming and power

### Building and Flashing

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

### Serial Monitor Output

When running, you'll see output like:
```
I (123) DOOR_SENSOR: Door monitoring system started. Monitoring GPIO 23 for door state changes.
I (456) DOOR_SENSOR: Door Opened!
I (789) DOOR_SENSOR: Door Closed!
```

## Future Enhancements

- Server component for remote monitoring
- Alert notifications
- Data logging and analytics
- Web dashboard
- Mobile app integration

## License

This project is open source and available under the MIT License.