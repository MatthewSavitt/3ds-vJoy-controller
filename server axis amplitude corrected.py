#!/usr/bin/env python
import pyvjoy
import socket
import re
import pyautogui
import time
import math
import threading

# Settings
buttonsPerClient = 16
axisPerClient = 2
host = "0.0.0.0"
port = 9999

# Touch/mouse optimization
mouse_smoothing = 0.6  # Higher = smoother but more latency
mouse_update_interval = 0.0666  # 15fps theoretical
touch_scale_factor = 0.95  # Reduce travel distance slightly for easier access to screen edges

# Mouse state tracking
last_mouse_x, last_mouse_y = None, None
last_mouse_update = 0
mouse_l_pressed = False
mouse_r_pressed = False

# Debug flags
debug_mode = True
display_stats = True
last_stats_time = 0
stats_interval = 5.0  # Show stats every 5 seconds
packet_count = 0
last_packet_count = 0

# Create the vJoy device
j = pyvjoy.VJoyDevice(1)

# Print vJoy device info
print(f"vJoy Device {j.rID} info:")
try:
    print(f"Number of buttons: {j.GetVJDButtonNumber()}")
    print(f"Number of axes: {j.GetVJDAxisNumber()}")
except Exception as e:
    print(f"Could not get vJoy device info: {e}")

# Regex pattern for updated input format: <buttons;cpadX;cpadY;touchActive;touchX;touchY>
pattern = re.compile(r'<(\d+);\s*(\d+);\s*(\d+);\s*(\d+);\s*(\d+);\s*(\d+)>')

# Get screen dimensions for scaling touch input
screen_width, screen_height = pyautogui.size()
touch_width, touch_height = 320, 240  # 3DS bottom screen dimensions

def compensate_circle_pad(cpad_x_raw, cpad_y_raw):
    """Compensate circle pad magnitude based on angle"""
    # Convert to normalized coordinates (-1 to 1)
    center = 16384
    max_range = 16384
    
    x_norm = (cpad_x_raw - center) / max_range
    y_norm = (cpad_y_raw - center) / max_range
    
    # Calculate angle in radians
    angle = math.atan2(y_norm, x_norm)
    
    # Calculate current magnitude
    current_magnitude = math.sqrt(x_norm**2 + y_norm**2)
    
    # Calculate compensation factor based on angle
    # Using the formula: abs(sin(theta) * sqrt(2) / 2)
    compensation = 1.0
    if abs(x_norm) > 0.01 or abs(y_norm) > 0.01:  # Avoid division by zero
        # For diagonal inputs, we need to boost the magnitude
        compensation = 1.0 / max(abs(math.cos(angle)), abs(math.sin(angle)))
    
    # Apply compensation
    compensated_magnitude = min(current_magnitude * compensation, 1.0)
    
    # Convert back to coordinates
    x_compensated = compensated_magnitude * math.cos(angle)
    y_compensated = compensated_magnitude * math.sin(angle)
    
    # Convert back to vJoy range (0-32768)
    vjoy_x = int(center + x_compensated * max_range)
    vjoy_y = int(center + y_compensated * max_range)
    
    # Clamp values
    vjoy_x = max(0, min(32768, vjoy_x))
    vjoy_y = max(0, min(32768, vjoy_y))
    
    return vjoy_x, vjoy_y

def setAxis(axis, value):
    """Set a vJoy axis with bounds checking"""
    try:
        value = max(0, min(value, 32768))
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
                if debug_mode:
                    print(f"ERROR: Unknown axis: {axis}")
    except Exception as e:
        if debug_mode:
            print(f"Error setting axis {axis} to {value}: {e}")

def setButtons(buttons, offset=0):
    """Set vJoy buttons with enhanced debugging and reliability"""
    try:
        # If debug mode, print the button values we're trying to set
        if debug_mode and buttons > 0:
            btn_names = []
            if buttons & (1 << 0): btn_names.append("A")
            if buttons & (1 << 1): btn_names.append("B")
            if buttons & (1 << 2): btn_names.append("X") 
            if buttons & (1 << 3): btn_names.append("Y")
            if buttons & (1 << 4): btn_names.append("DUp")
            if buttons & (1 << 5): btn_names.append("DDown")
            if buttons & (1 << 6): btn_names.append("DLeft") 
            if buttons & (1 << 7): btn_names.append("DRight")
            if buttons & (1 << 8): btn_names.append("L")
            if buttons & (1 << 9): btn_names.append("R")
            if buttons & (1 << 10): btn_names.append("Select")
            if buttons & (1 << 11): btn_names.append("Start")
            print(f"Setting buttons: {buttons:012b} - {', '.join(btn_names) if btn_names else 'None'}")
        
        # Apply button inputs to vJoy using the mask technique from the older code
        # First clear the button state for this client
        j.data.lButtons &= ~(((1 << buttonsPerClient) - 1) << offset)
        
        # Then set the new state
        j.data.lButtons |= (buttons << offset)
        
        # Force immediate update
        result = j.update()
        if debug_mode and buttons > 0:
            print(f"vJoy update result: {result}")
            
    except Exception as e:
        print(f"Error setting buttons: {e}")
        print(f"Button value that caused error: {buttons:012b} ({buttons})")

def handleMouse(touch_active, touch_x, touch_y, buttons):
    """Handle mouse movement and clicks with improved smoothing"""
    global last_mouse_x, last_mouse_y, last_mouse_update
    global mouse_l_pressed, mouse_r_pressed
    
    # Button constants
    BTN_L = 1 << 8
    BTN_R = 1 << 9
    
    current_time = time.time()
    
    # Only process L/R as mouse clicks when touch is active
    if touch_active:
        # Handle mouse buttons
        l_button_pressed = bool(buttons & BTN_L)
        r_button_pressed = bool(buttons & BTN_R)
        
        # Left mouse button (L button)
        if l_button_pressed != mouse_l_pressed:
            try:
                if l_button_pressed:
                    pyautogui.mouseDown(button='left')
                else:
                    pyautogui.mouseUp(button='left')
                mouse_l_pressed = l_button_pressed
            except Exception as e:
                if debug_mode:
                    print(f"Mouse button error: {e}")
        
        # Right mouse button (R button)
        if r_button_pressed != mouse_r_pressed:
            try:
                if r_button_pressed:
                    pyautogui.mouseDown(button='right')
                else:
                    pyautogui.mouseUp(button='right')
                mouse_r_pressed = r_button_pressed
            except Exception as e:
                if debug_mode:
                    print(f"Mouse button error: {e}")
    else:
        # If touch is not active, ensure mouse buttons are released
        if mouse_l_pressed:
            pyautogui.mouseUp(button='left')
            mouse_l_pressed = False
        if mouse_r_pressed:
            pyautogui.mouseUp(button='right')
            mouse_r_pressed = False
    
    # Handle mouse movement (separate from buttons)
    if touch_active and current_time - last_mouse_update >= mouse_update_interval:
        # Scale touch screen coordinates to monitor resolution with scaling factor
        center_x = touch_width / 2
        center_y = touch_height / 2
        
        # Apply touch scale factor to make edges easier to reach
        scaled_x = center_x + (touch_x - center_x) * touch_scale_factor
        scaled_y = center_y + (touch_y - center_y) * touch_scale_factor
        
        # Map to screen coordinates
        target_x = int((scaled_x / touch_width) * screen_width)
        target_y = int((scaled_y / touch_height) * screen_height)
        
        # Initialize last position if this is first touch
        if last_mouse_x is None:
            last_mouse_x = target_x
            last_mouse_y = target_y
        
        # Apply smoothing
        smooth_x = last_mouse_x + (target_x - last_mouse_x) * (1.0 - mouse_smoothing)
        smooth_y = last_mouse_y + (target_y - last_mouse_y) * (1.0 - mouse_smoothing)
        
        # Bound check
        smooth_x = max(0, min(screen_width - 1, smooth_x))
        smooth_y = max(0, min(screen_height - 1, smooth_y))
        
        try:
            # Only move if position changed meaningfully
            if math.sqrt((smooth_x - last_mouse_x)**2 + (smooth_y - last_mouse_y)**2) > 1:
                pyautogui.moveTo(int(smooth_x), int(smooth_y))
                last_mouse_x, last_mouse_y = smooth_x, smooth_y
        except Exception as e:
            if debug_mode:
                print(f"Mouse button error: {e}")
        
        last_mouse_update = current_time
    
    # If touch is released, reset last position
    if not touch_active:
        last_mouse_x = None
        last_mouse_y = None

def display_statistics():
    """Display periodic statistics in a separate thread"""
    global last_stats_time, packet_count, last_packet_count
    
    while True:
        current_time = time.time()
        if current_time - last_stats_time >= stats_interval:
            packets_per_second = (packet_count - last_packet_count) / stats_interval
            
            # Format mouse position, handling None values
            if last_mouse_x is None or last_mouse_y is None:
                mouse_pos = "N/A"
            else:
                mouse_pos = f"{int(last_mouse_x)}, {int(last_mouse_y)}"
            
            print(f"Performance: {packets_per_second:.1f} packets/sec | Mouse pos: "
                  f"{mouse_pos} | Buttons: L:{mouse_l_pressed} R:{mouse_r_pressed}")
            
            last_packet_count = packet_count
            last_stats_time = current_time
        
        time.sleep(1)

# UDP socket setup
try:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)  # Larger receive buffer
    sock.bind((host, port))
    print(f"UDP server listening on {host}:{port}")
except Exception as e:
    print(f"Socket setup error: {e}")
    exit(1)

# Client tracking dictionary
clients = {}

# Initialize stats
last_stats_time = time.time()

# Start stats display thread if enabled
if display_stats:
    stats_thread = threading.Thread(target=display_statistics, daemon=True)
    stats_thread.start()

# Main server loop
print("\n3DS Controller Server Running")
print("----------------------------")
print("- Buttons and D-pad mapped to vJoy buttons 1-12")
print("- Circle pad mapped to X/Y axes with angle compensation")
print("- Touchscreen controls mouse movement")
print("- L button = left mouse click")
print("- R button = right mouse click")
print("Ready for connections...\n")

while True:
    try:
        data, addr = sock.recvfrom(1024)
        if not data:
            continue
        
        # Count packet for statistics
        packet_count += 1
        
        # Convert bytes to string
        data_str = data.decode('utf-8')
        
        # Check if disconnect message
        if data_str == "d":
            if addr in clients:
                print(f"Client disconnected: {addr[0]}:{addr[1]}")
                
                # Clean up when client disconnects
                if mouse_l_pressed:
                    pyautogui.mouseUp(button='left')
                    mouse_l_pressed = False
                if mouse_r_pressed:
                    pyautogui.mouseUp(button='right')
                    mouse_r_pressed = False
                
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
            
            # Process button inputs first
            setButtons(buttons, offsetB)

            # Apply angle compensation to circle pad values
            compensated_x, compensated_y = compensate_circle_pad(cpad_x, cpad_y)
            
            # Now set the compensated axis values
            setAxis(1 + offsetA, compensated_x)
            setAxis(2 + offsetA, 32768 - compensated_y)  # Invert Y-axis
            
            # Handle mouse separately
            handleMouse(touch_active, touch_x, touch_y, buttons)
            
            # Force vJoy update
            j.update()
        
    except Exception as e:
        print(f"Error in main loop: {e}")
