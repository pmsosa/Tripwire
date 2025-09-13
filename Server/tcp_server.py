#!/usr/bin/env python3
"""
Simple TCP Server for Door Monitor
Receives JSON messages from ESP32 door sensor
"""

import socket
import json
import datetime
import threading

# Server configuration
HOST = '0.0.0.0'  # Listen on all interfaces
PORT = 8080

def handle_client(conn, addr):
    """Handle individual client connections"""
    print(f"[{datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] Connection from {addr}")
    
    try:
        with conn:
            while True:
                # Receive data from client
                data = conn.recv(1024)
                if not data:
                    break
                
                # Decode the message
                message = data.decode('utf-8')
                timestamp = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                
                try:
                    # Parse JSON message
                    json_data = json.loads(message)
                    status = json_data.get('STATUS', 'UNKNOWN')
                    msg_timestamp = json_data.get('TIMESTAMP', 'N/A')
                    
                    # Log the door status
                    if msg_timestamp != 'N/A':
                        msg_time = datetime.datetime.fromtimestamp(int(msg_timestamp)).strftime('%Y-%m-%d %H:%M:%S')
                        print(f"[{timestamp}] Door Status: {status} (Event time: {msg_time})")
                    else:
                        print(f"[{timestamp}] Door Status: {status}")
                    
                    # Send acknowledgment back to ESP32
                    ack_response = "ACK"
                    conn.send(ack_response.encode('utf-8'))
                    
                    # You can add additional processing here:
                    # - Save to database
                    # - Send notifications
                    # - Trigger other actions
                    
                except json.JSONDecodeError:
                    print(f"[{timestamp}] Invalid JSON received: {message}")
                    # Send NACK for invalid JSON
                    nack_response = "NACK"
                    conn.send(nack_response.encode('utf-8'))
                
    except ConnectionResetError:
        print(f"[{datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] Connection reset by {addr}")
    except Exception as e:
        print(f"[{datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] Error handling client {addr}: {e}")
    finally:
        print(f"[{datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] Disconnected from {addr}")

def main():
    """Main server function"""
    print(f"Starting Door Monitor TCP Server on {HOST}:{PORT}")
    print("Waiting for ESP32 connections...")
    
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server_socket:
            # Allow socket reuse
            server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            
            # Bind and listen
            server_socket.bind((HOST, PORT))
            server_socket.listen(5)
            
            while True:
                # Accept incoming connections
                conn, addr = server_socket.accept()
                
                # Handle each client in a separate thread
                client_thread = threading.Thread(target=handle_client, args=(conn, addr))
                client_thread.daemon = True
                client_thread.start()
                
    except KeyboardInterrupt:
        print("\nShutting down server...")
    except Exception as e:
        print(f"Server error: {e}")

if __name__ == "__main__":
    main()