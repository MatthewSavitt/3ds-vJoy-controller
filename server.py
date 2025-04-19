#!/usr/bin/env python
import pyvjoy
import socket
import re
import pyautogui  # For mouse control

# Settings
buttonsPerClient = 16
axisPerClient = 2
host = "0.0.0.0"
port = 9999

# Create the vJoy device
j = pyvjoy.VJoyDevice(1)

# Regex pattern for updated input format: <buttons;cpadX;cpadY;touchActive;touchX;touchY>
pattern = re.compile(r'<(\d+);\s*(\d+);\s*(\d+);\s*(\d+);\s*(\d+);\s*(\d+)>')

# Get screen dimensions for scaling touch input
screen_width, screen_height = pyautogui.size()
touch_width, touch_height = 320, 240  # 3DS bottom screen dimensions

def setAxis(axis, value):
    match axis:
        case 1:
            j.data.wAxisX = value
        case 2:
            j.data.wAxisY = value
        case 3:
            j.data.wAxisZ = value
        case 4:
            j.data.wAxisXRot = value
        case 5:
            j.data.wAxisYRot = value
        case 6:
            j.data.wAxisZRot = value
        case 7:
            j.data.wSlider = value
        case 8:
            j.data.wDial = value
        case _:
            print("ERROR: Unknown axis:", axis)

# UDP socket setup
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((host, port))
print(f"UDP server listening on {host}:{port}")

# Client tracking dictionary
clients = {}

# Button mapping constants
BTN_A = 1 << 0
BTN_B = 1 << 1
BTN_X = 1 << 2
BTN_Y = 1 << 3
BTN_DUP = 1 << 4
BTN_DDOWN = 1 << 5
BTN_DLEFT = 1 << 6
BTN_DRIGHT = 1 << 7
BTN_L = 1 << 8
BTN_R = 1 << 9
BTN_SELECT = 1 << 10
BTN_START = 1 << 11

# Main server loop
while True:
    try:
        data, addr = sock.recvfrom(1024)
        if not data:
            continue
            
        # Convert bytes to string
        data_str = data.decode('utf-8')
        
        # Check if disconnect message
        if data_str == "d":
            if addr in clients:
                print(f"Client disconnected: {addr[0]}:{addr[1]}")
                del clients[addr]
            continue
            
        # Parse the input data
        match = pattern.match(data_str)
        if match:
            if addr not in clients:
                # New client
                client_id = len(clients)
                clients[addr] = client_id
                offsetB = client_id * buttonsPerClient
                offsetA = client_id * axisPerClient
                print(f"New client: {addr[0]}:{addr[1]} (ID: {client_id})")
            else:
                client_id = clients[addr]
                offsetB = client_id * buttonsPerClient
                offsetA = client_id * axisPerClient
                
            # Parse values from the input string
            buttons = int(match.group(1))
            cpad_x = int(match.group(2))
            cpad_y = int(match.group(3))
            touch_active = int(match.group(4)) == 1
            touch_x = int(match.group(5))
            touch_y = int(match.group(6))
            
            # Apply button inputs to vJoy
            # First clear and set face buttons (A,B,X,Y)
            j.data.lButtons &= ~(((1 << buttonsPerClient) - 1) << offsetB)
            j.data.lButtons |= (buttons << offsetB)
            
            # Set circle pad axes
            setAxis(1 + offsetA, cpad_x)
            # Invert Y-axis for circle pad
            setAxis(2 + offsetA, 32768 - cpad_y)
            
            # Handle touchscreen input as mouse
            if touch_active:
                # Scale touch screen coordinates to monitor resolution
                mouse_x = int((touch_x / touch_width) * screen_width)
                mouse_y = int((touch_y / touch_height) * screen_height)
                
                # Move mouse pointer
                pyautogui.moveTo(mouse_x, mouse_y)
                
                # Optional: handle touches as mouse clicks
                # For example, could implement L as left click, R as right click
                if buttons & BTN_L:
                    pyautogui.mouseDown(button='left')
                else:
                    pyautogui.mouseUp(button='left')
                    
                if buttons & BTN_R:
                    pyautogui.mouseDown(button='right')
                else:
                    pyautogui.mouseUp(button='right')
            
            # Update the controller state
            j.update()
            
    except Exception as e:
        print(f"Error: {e}")
