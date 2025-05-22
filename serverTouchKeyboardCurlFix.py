#!/usr/bin/env python
import pyvjoy
import socket
import re
import pyautogui
import time
import math
import threading
import tkinter as tk

# For keyboard functionality - reinstall if needed: pip install pyautogui keyboard
import pyautogui
try:
    import keyboard as kb
    keyboard_module_available = True
except ImportError:
    keyboard_module_available = False
    print("Keyboard module not available - install with: pip install keyboard")

# Hide console window (Windows only)
try:
    import ctypes
    ctypes.windll.user32.ShowWindow(ctypes.windll.kernel32.GetConsoleWindow(), 0)
except:
    pass  # Not on Windows or error occurred

# Settings
buttonsPerClient = 16
axisPerClient = 2
host = "0.0.0.0"
port = 9999
pyautogui.FAILSAFE = False

# Touch/mouse optimization
mouse_smoothing = 0.6  # Higher = smoother but more latency
mouse_update_interval = 0.0666  # 15fps theoretical
touch_scale_factor = 0.95  # Reduce travel distance slightly for easier access to screen edges

touch_mode_active = False
touch_mode_smoothing = 0.5  # Much less smoothing for responsiveness
touch_mode_smoothing_initial = 0.05  # Even less smoothing for first stroke (NEW LINE)
touch_mode_initial_strokes = 5  # Number of updates with reduced smoothing (NEW LINE)
touch_mode_update_interval = 0.0041  # 120fps theoretical
touch_mode_scale_factor = 1.0  # No scaling - direct mapping
touch_mode_acceleration = 1.0

# Keyboard handling
keyboard_prefix = "K:"  # Must match the prefix in the 3DS code

stroke_counter = 0  # Track initial strokes after pen down (NEW LINE)

# Mouse state tracking
last_mouse_x, last_mouse_y = None, None
last_mouse_update = 0
mouse_l_pressed = False
mouse_r_pressed = False

# Touch mode click tracking
touch_mode_click_start_time = 0
touch_mode_click_position = None
touch_mode_drag_threshold = 5  # Reduced to 5px - detect drags sooner
touch_mode_last_toggle = 0
touch_mode_debounce_time = 0.25  # 250ms debounce

# Disable auto-release completely - we'll handle touch state ourselves
disable_auto_release = True

# Debug flags
debug_mode = False
display_stats = True
last_stats_time = 0
stats_interval = 5.0  # Show stats every 5 seconds
packet_count = 0
last_packet_count = 0

# Client tracking dictionary - INITIALIZE HERE
clients = {}

# 3DS calibration data
# The actual input ranges from the touchscreen
touch_min_x = 5
touch_max_x = 314
touch_min_y = 5
touch_max_y = 234
# The full theoretical range
touch_width, touch_height = 320, 240  # 3DS bottom screen dimensions

# Create the vJoy device
j = pyvjoy.VJoyDevice(1)

# Create a simple GUI window to prevent application crashes
# This will capture mouse events instead of the console
root = tk.Tk()
root.title("3DS Controller Server")
root.geometry("400x300")
root.resizable(True, True)

# Add some information to the window
status_label = tk.Label(root, text="Server Running", font=("Arial", 12, "bold"))
status_label.pack(pady=10)

info_text = tk.Text(root, height=15, width=45)
info_text.pack(pady=5, padx=5, fill=tk.BOTH, expand=True)
info_text.insert(tk.END, "3DS Controller Server\n")
info_text.insert(tk.END, "-----------------------\n")
info_text.insert(tk.END, "- Buttons and D-pad mapped to vJoy buttons 1-12\n")
info_text.insert(tk.END, "- Circle pad mapped to X/Y axes with angle compensation\n")
info_text.insert(tk.END, "- Touchscreen controls mouse movement\n")
info_text.insert(tk.END, "- L button = left mouse click\n")
info_text.insert(tk.END, "- R button = right click (in normal mode) or right mouse click (in touch mode)\n")
info_text.insert(tk.END, "- L+R+Select = Toggle Touch Mode\n")
info_text.insert(tk.END, "- L+R+Start = Open keyboard to send text\n")
info_text.insert(tk.END, f"- Touchscreen calibration: ({touch_min_x},{touch_min_y}) to ({touch_max_x},{touch_max_y})\n\n")
info_text.insert(tk.END, "Ready for connections...\n")
info_text.config(state=tk.DISABLED)

# Status display at bottom
status_display = tk.StringVar()
status_display.set("Waiting for connections...")
status_label_bottom = tk.Label(root, textvariable=status_display, bd=1, relief=tk.SUNKEN, anchor=tk.W)
status_label_bottom.pack(side=tk.BOTTOM, fill=tk.X)

# Print vJoy device info
try:
    info_text.config(state=tk.NORMAL)
    info_text.insert(tk.END, f"\nvJoy Device {j.rID} info:\n")
    info_text.insert(tk.END, f"Number of buttons: {j.GetVJDButtonNumber()}\n")
    info_text.insert(tk.END, f"Number of axes: {j.GetVJDAxisNumber()}\n")
    info_text.config(state=tk.DISABLED)
except Exception as e:
    info_text.config(state=tk.NORMAL)
    info_text.insert(tk.END, f"Could not get vJoy device info: {e}\n")
    info_text.config(state=tk.DISABLED)

# Regex pattern for updated input format: <buttons;cpadX;cpadY;touchActive;touchX;touchY;mode>
pattern = re.compile(r'<(\d+);\s*(\d+);\s*(\d+);\s*(\d+);\s*(\d+);\s*(\d+);\s*(\d+)>')

# Get screen dimensions for scaling touch input
screen_width, screen_height = pyautogui.size()

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
                    log_message(f"ERROR: Unknown axis: {axis}")
    except Exception as e:
        if debug_mode:
            log_message(f"Error setting axis {axis} to {value}: {e}")

def setButtons(buttons, offset=0):
    """Set vJoy buttons with enhanced debugging and reliability"""
    try:
        # If debug mode, log the button values we're trying to set
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
            log_message(f"Setting buttons: {buttons:012b} - {', '.join(btn_names) if btn_names else 'None'}")
        
        # Apply button inputs to vJoy using the mask technique from the older code
        # First clear the button state for this client
        j.data.lButtons &= ~(((1 << buttonsPerClient) - 1) << offset)
        
        # Then set the new state
        j.data.lButtons |= (buttons << offset)
        
        # Force immediate update
        result = j.update()
        if debug_mode and buttons > 0:
            log_message(f"vJoy update result: {result}")
            
    except Exception as e:
        log_message(f"Error setting buttons: {e}")
        log_message(f"Button value that caused error: {buttons:012b} ({buttons})")

def normalize_touch_coordinate(x, y):
    """Normalize touch coordinates to account for calibration differences"""
    # Normalize x from actual range to 0-1
    norm_x = (x - touch_min_x) / (touch_max_x - touch_min_x)
    # Normalize y from actual range to 0-1
    norm_y = (y - touch_min_y) / (touch_max_y - touch_min_y)
    
    # Clamp values to 0-1 range to handle edge cases
    norm_x = max(0.0, min(1.0, norm_x))
    norm_y = max(0.0, min(1.0, norm_y))
    
    return norm_x, norm_y

def handleMouse(touch_active, touch_x, touch_y, buttons, client_mode=None):
    """Handle mouse movement and clicks with improved smoothing and calibration"""
    global last_mouse_x, last_mouse_y, last_mouse_update
    global mouse_l_pressed, mouse_r_pressed, touch_mode_active
    global touch_mode_click_start_time, touch_mode_click_position, touch_mode_last_toggle
    
    current_time = time.time()
    
    # Touch mode toggle handling
    BTN_L = 1 << 8
    BTN_R = 1 << 9
    BTN_SELECT = 1 << 10
    
    # Client mode sync handling
    if client_mode is not None:
        if client_mode != touch_mode_active:
            touch_mode_active = client_mode
            if touch_mode_active:
                log_message("TOUCH MODE SYNCED - Using client mode setting")
            else:
                log_message("NORMAL MODE SYNCED - Using client mode setting")
    
    # Toggle handling
    elif (buttons & BTN_L) and (buttons & BTN_R) and (buttons & BTN_SELECT):
        if current_time - touch_mode_last_toggle > touch_mode_debounce_time:
            touch_mode_active = not touch_mode_active
            touch_mode_last_toggle = current_time
            
            if touch_mode_active:
                log_message("TOUCH MODE ACTIVATED - Optimized for real-time touch")
            else:
                log_message("TOUCH MODE DEACTIVATED - Normal mode restored")
                
            # Release any held mouse buttons when changing modes
            if mouse_l_pressed:
                pyautogui.mouseUp(button='left')
                mouse_l_pressed = False
            if mouse_r_pressed:
                pyautogui.mouseUp(button='right')
                mouse_r_pressed = False
        return
    
    # Normalize touch coordinates
    norm_x, norm_y = normalize_touch_coordinate(touch_x, touch_y)
    
    # TOUCH MODE HANDLING
    if touch_mode_active:
        # Process position first - map normalized coordinates directly to screen
        target_x = int(norm_x * screen_width)
        target_y = int(norm_y * screen_height)
        
        # Only update position if touch is active
        if touch_active:
            # OPTIMIZATION: Use different smoothing depending on button state
            r_button_pressed = bool(buttons & BTN_R)
            l_button_pressed = bool(buttons & BTN_L)
            
            # First touch or position needs init
            if last_mouse_x is None:
                # Direct positioning for first touch - no smoothing at all
                pyautogui.moveTo(target_x, target_y, _pause=False)
                last_mouse_x = target_x
                last_mouse_y = target_y
                if debug_mode:
                    log_message("First touch - direct positioning")
            else:
                # NEW: Use minimal or no smoothing when drawing (left button pressed)
                # This fixes the lag issue with left click drawing operations
                
                # Regular non-drawing cursor movement
                if l_button_pressed or r_button_pressed:
                    # Use light smoothing for cursor movement (when L or R is held)
                    smooth_factor = 0.5
                    smooth_x = last_mouse_x + (target_x - last_mouse_x) * (1.0 - smooth_factor)
                    smooth_y = last_mouse_y + (target_y - last_mouse_y) * (1.0 - smooth_factor)
                    pyautogui.moveTo(int(smooth_x), int(smooth_y), _pause=False)
                    last_mouse_x, last_mouse_y = smooth_x, smooth_y
                elif mouse_l_pressed and not l_button_pressed and not r_button_pressed:
                    # CRITICAL FIX: Use almost no smoothing during drawing operations
                    # This is the key change to fix left-click lag for drawing
                    smooth_factor = 0.5  # Minimal smoothing for drawing
                    
                    # Apply very light smoothing
                    smooth_x = last_mouse_x + (target_x - last_mouse_x) * (1.0 - smooth_factor)
                    smooth_y = last_mouse_y + (target_y - last_mouse_y) * (1.0 - smooth_factor)
                    
                    # During drawing - use _pause=False to prevent any delay
                    pyautogui.moveTo(int(smooth_x), int(smooth_y), _pause=False)
                    last_mouse_x, last_mouse_y = smooth_x, smooth_y
                else:
                    # Normal cursor movement - balanced smoothing
                    smooth_factor = 0.5
                    smooth_x = last_mouse_x + (target_x - last_mouse_x) * (1.0 - smooth_factor)
                    smooth_y = last_mouse_y + (target_y - last_mouse_y) * (1.0 - smooth_factor)
                    pyautogui.moveTo(int(smooth_x), int(smooth_y), _pause=False)
                    last_mouse_x, last_mouse_y = smooth_x, smooth_y
        
        # Button handling - first check the simpler L button tracking mode
        r_button_pressed = bool(buttons & BTN_R)
        l_button_pressed = bool(buttons & BTN_L)
        
        # Handle left click based on touch_active
        # If L is held in touch mode, only move cursor (disable clicking)
        if touch_active and l_button_pressed:
            # CURSOR TRACKING MODE - Ensure mouse buttons are released
            if mouse_l_pressed:
                pyautogui.mouseUp(button='left')
                mouse_l_pressed = False
            if mouse_r_pressed:
                pyautogui.mouseUp(button='right')
                mouse_r_pressed = False
        
        # Normal touch mode behavior (only if L is not pressed)
        # OPTIMIZED LEFT CLICK HANDLING - More responsive
        elif touch_active and not r_button_pressed and not l_button_pressed:
            # TOUCH ACTIVE = LEFT MOUSE SHOULD BE DOWN
            if not mouse_l_pressed:
                # Use _pause=False for the mouse down event to reduce lag
                pyautogui.mouseDown(button='left', _pause=False)
                mouse_l_pressed = True
        elif not touch_active or r_button_pressed or l_button_pressed:
            # TOUCH NOT ACTIVE OR R OR L PRESSED = LEFT MOUSE SHOULD BE UP
            if mouse_l_pressed:
                # Use _pause=False for the mouse up event to reduce lag
                pyautogui.mouseUp(button='left', _pause=False)
                mouse_l_pressed = False
        
        # Handle right click with R button (only if L is not pressed)
        if touch_active and r_button_pressed and not l_button_pressed:
            # R BUTTON + TOUCH = RIGHT CLICK (unless L is also pressed)
            if not mouse_r_pressed:
                pyautogui.mouseDown(button='right', _pause=False)
                mouse_r_pressed = True
        elif not touch_active or not r_button_pressed or l_button_pressed:
            # NO TOUCH OR NO R OR L PRESSED = NO RIGHT CLICK
            if mouse_r_pressed:
                pyautogui.mouseUp(button='right', _pause=False)
                mouse_r_pressed = False
        
        # Reset if touch ended
        if not touch_active:
            last_mouse_x = None
            last_mouse_y = None
        
        # Skip normal processing
        return

def handleKeyboard(text):
    """Process and type keyboard input received from the 3DS"""
    try:
        if not text:
            log_message("Received empty keyboard input")
            return
            
        log_message(f"Typing keyboard input: {text}")
        
        # Try different methods in order of reliability
        
        # 1. Try keyboard module if available (most reliable)
        if keyboard_module_available:
            try:
                kb.write(text)
                log_message("Text typed using keyboard module")
                return
            except Exception as e:
                log_message(f"Keyboard module typing failed: {e}")
        
        # 2. Simple pyautogui method
        try:
            pyautogui.typewrite(text)
            log_message("Text typed using pyautogui")
            return
        except Exception as e:
            log_message(f"Pyautogui typing failed: {e}")
        
        log_message("WARNING: All typing methods failed!")
        
    except Exception as e:
        log_message(f"Critical error in keyboard handling: {e}")
def log_message(message):
    """Log a message to the GUI and console"""
    print(message)
    try:
        info_text.config(state=tk.NORMAL)
        info_text.insert(tk.END, f"{message}\n")
        info_text.see(tk.END)  # Scroll to see the latest message
        info_text.config(state=tk.DISABLED)
        root.update_idletasks()  # Update the UI
    except:
        pass  # If the window is closed, we don't want to crash

def update_stats():
    """Update statistics display"""
    global last_stats_time, packet_count, last_packet_count
    
    current_time = time.time()
    if current_time - last_stats_time >= stats_interval:
        packets_per_second = (packet_count - last_packet_count) / stats_interval
        
        # Format mouse position, handling None values
        if last_mouse_x is None or last_mouse_y is None:
            mouse_pos = "N/A"
        else:
            mouse_pos = f"{int(last_mouse_x)}, {int(last_mouse_y)}"
        
        status_text = f"Performance: {packets_per_second:.1f} packets/sec | Mouse pos: {mouse_pos} | Buttons: L:{mouse_l_pressed} R:{mouse_r_pressed}"
        log_message(status_text)
        status_display.set(status_text)
        
        last_packet_count = packet_count
        last_stats_time = current_time
    
    # Schedule the next update
    root.after(1000, update_stats)

def server_loop():
    """Main server loop function - runs in a separate thread"""
    global packet_count, clients
    
    # UDP socket setup
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)  # Larger receive buffer
        sock.bind((host, port))
        log_message(f"UDP server listening on {host}:{port}")
    except Exception as e:
        log_message(f"Socket setup error: {e}")
        return
    
    # Main loop
    while True:
        try:
            data, addr = sock.recvfrom(1024)
            if not data:
                continue
            
            # Count packet for statistics
            packet_count += 1
            
            # Convert bytes to string
            data_str = data.decode('utf-8')
            
            # Debug log for ALL incoming packets
            if debug_mode == True:
                log_message(f"DEBUG: Received packet: '{data_str}' from {addr[0]}:{addr[1]}")
            
            # Check if disconnect message
            if data_str == "d":
                if addr in clients:
                    log_message(f"Client disconnected: {addr[0]}:{addr[1]}")
                    
                    # Clean up when client disconnects
                    if mouse_l_pressed:
                        pyautogui.mouseUp(button='left')
                        mouse_l_pressed = False
                    if mouse_r_pressed:
                        pyautogui.mouseUp(button='right')
                        mouse_r_pressed = False
                    
                    del clients[addr]
                continue
            
            # Check if this is a keyboard packet
            if data_str.startswith(keyboard_prefix):
                # Extract the keyboard text (remove the prefix)
                keyboard_text = data_str[len(keyboard_prefix):]
                log_message(f"Received keyboard input from {addr[0]}:{addr[1]}")
                
                # Process the keyboard input
                handleKeyboard(keyboard_text)
                
                # Skip regular processing for keyboard packets
                continue
            
            # Parse the input data
            match = pattern.match(data_str)
            if match:
                if addr not in clients:
                    # New client
                    client_id = len(clients)
                    clients[addr] = {
                        'id': client_id,
                        'mode': False,
                        'last_active': time.time()  # Track client activity
                    }
                    offsetB = client_id * buttonsPerClient
                    offsetA = client_id * axisPerClient
                    log_message(f"New client: {addr[0]}:{addr[1]} (ID: {client_id})")
                    status_display.set(f"Connected to: {addr[0]}:{addr[1]}")
                else:
                    client_id = clients[addr]['id']
                    offsetB = client_id * buttonsPerClient
                    offsetA = client_id * axisPerClient
                
                # Parse values from the input string
                buttons = int(match.group(1))
                cpad_x = int(match.group(2))
                cpad_y = int(match.group(3))
                touch_active = int(match.group(4)) == 1
                touch_x = int(match.group(5))
                touch_y = int(match.group(6))
                client_mode = int(match.group(7)) == 1  # Parse mode (1=touch, 0=normal)
                
                # Update client mode
                clients[addr]['mode'] = client_mode
                
                # Process button inputs first
                setButtons(buttons, offsetB)
    
                # Apply angle compensation to circle pad values
                compensated_x, compensated_y = compensate_circle_pad(cpad_x, cpad_y)
                
                # Now set the compensated axis values
                setAxis(1 + offsetA, compensated_x)
                setAxis(2 + offsetA, 32768 - compensated_y)  # Invert Y-axis
                
                # Handle mouse separately
                handleMouse(touch_active, touch_x, touch_y, buttons, client_mode)
                
                # Force vJoy update
                j.update()
                
                # Update client timestamp
                clients[addr]['last_active'] = time.time()
            
        except Exception as e:
            log_message(f"Error in main loop: {e}")
            # Don't break the loop for errors

# Start the server in a separate thread
server_thread = threading.Thread(target=server_loop, daemon=True)
server_thread.start()

# Start the stats update
update_stats()

# Start the GUI main loop
try:
    root.mainloop()
except:
    # Clean up on exit
    if mouse_l_pressed:
        pyautogui.mouseUp(button='left')
    if mouse_r_pressed:
        pyautogui.mouseUp(button='right')
