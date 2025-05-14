#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

#include <fcntl.h>

#include <sys/types.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <3ds.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

// Optimization settings
#define TOUCH_THRESHOLD 10      // Minimum pixel change for touch updates
#define TOUCH_THROTTLE_TIME 16666667  // About 60fps in system ticks
#define CPAD_THRESHOLD 5        // Minimum circle pad movement for updates
#define HELD_INPUT_UPDATE_TIME 200000000 // Send updates for held inputs every ~200ms
#define CONSOLE_UPDATE_INTERVAL 180000000 // Console update rate

// Reduce circle pad resolution for faster network
#define CPAD_RESOLUTION 256  // Use 256 steps instead of 32768 (reduces precision by 99%)

// Touch mode settings
#define TOUCH_MODE_THRESHOLD 2       // Minimum pixel change for touch mode
#define TOUCH_MODE_THROTTLE_TIME 8333333  // ~120fps in system ticks
#define TOUCH_MODE_BUTTONS (KEY_L | KEY_R | KEY_SELECT)
#define TOUCH_MODE_DEBOUNCE_TIME 250000000  // 250ms debounce delay
#define MODE_SYNC_INTERVAL 1000000000  // Send mode sync every 1 second

// Keyboard settings
#define KEYBOARD_TRIGGER_BUTTONS (KEY_L | KEY_R | KEY_START)
#define KEYBOARD_DEBOUNCE_TIME 1000000000  // 1 second debounce delay
#define KEYBOARD_PACKET_PREFIX "K:" // Prefix for keyboard packets

// Screenshot settings
#define SCREENSHOT_TRIGGER_BUTTONS (KEY_L | KEY_R | KEY_SELECT | KEY_START)
#define SCREENSHOT_DEBOUNCE_TIME 1000000000  // 1 second debounce delay
#define SCREENSHOT_PACKET_PREFIX "S:"

// Networking reliability settings
#define MAX_SEND_RETRIES 3         // Try sending important packets multiple times
#define SEND_RETRY_DELAY 5000000   // 5ms between retries

static u32 *SOC_buffer = NULL;
s32 sock = -1, csock = -1;
struct sockaddr_in serverAddr;
static char address[60];

// Screenshot reception state
typedef struct {
    bool active;
    u8* buffer;
    u32 total_size;
    u32 total_chunks;
    u32 chunks_received;
    bool* chunk_received_flags;
    u16 width;
    u16 height;
    u64 start_time;
    u32 timeout_ms;
    bool display_screenshot;  // Flag to show screenshot on bottom screen
} ScreenshotReceiver;

static ScreenshotReceiver screenshot_rx = {0};

// Konami code sequence and tracking
#define KONAMI_CODE_LENGTH 11
u32 konamiCode[KONAMI_CODE_LENGTH] = {
    KEY_DUP, KEY_DUP, KEY_DDOWN, KEY_DDOWN, 
    KEY_DLEFT, KEY_DRIGHT, KEY_DLEFT, KEY_DRIGHT, 
    KEY_B, KEY_A, KEY_START
};
int konamiCodeIndex = 0;

// Timing variables
u64 lastTouchTime = 0;    // For throttling touch updates
u64 lastConsoleUpdate = 0; // For throttling console output
u64 lastHeldUpdate = 0;    // For periodic updates on held inputs
u64 lastModeSyncTime = 0;  // For periodic mode sync packets

// Touch mode state
bool touch_mode_active = false;
u64 touch_mode_last_time = 0;
u64 touch_mode_last_toggle = 0;  // For debouncing

// Keyboard state
u64 keyboard_last_toggle = 0;  // For debouncing keyboard activation

// Screenshot state
u64 screenshot_last_toggle = 0;  // For debouncing screenshot request

__attribute__((format(printf,1,2)))
void failExit(const char *fmt, ...);

// Forward declarations
void sendReliablePacket(const char* packet, size_t length);
void sendModeSyncPacket(u64 currentTime);
void handleKeyboardInput();
void start_screenshot_reception(const char* data, size_t len);
void handle_screenshot_chunk(const char* data, size_t len);
void check_screenshot_completion();
void display_screenshot();
void cleanup_screenshot_reception();
void clear_screenshot();

//---------------------------------------------------------------------------------
void socShutdown() {
//---------------------------------------------------------------------------------
    printf("waiting for socExit...\n");
    socExit();
}

void getAddressText(SwkbdState swkbd) {
    swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, -1);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN | SWKBD_ALLOW_HOME | SWKBD_ALLOW_RESET | SWKBD_ALLOW_POWER);
    swkbdSetHintText(&swkbd, "Enter the server IP");
    SwkbdButton button = SWKBD_BUTTON_NONE;
    bool shouldQuit = false;
    do {
        swkbdSetInitialText(&swkbd, "");
        button = swkbdInputText(&swkbd, address, sizeof(address));
        if (button != SWKBD_BUTTON_NONE)
            break;

        SwkbdResult res = swkbdGetResult(&swkbd);
        if (res == SWKBD_RESETPRESSED) {
            shouldQuit = true;
            aptSetChainloaderToSelf();
            break;
        }
        else if (res != SWKBD_HOMEPRESSED && res != SWKBD_POWERPRESSED) {
            failExit("Error on input\n");
        }

        shouldQuit = !aptMainLoop();
    } while (!shouldQuit);
}

// Function to display keyboard and send input
void handleKeyboardInput() {
    SwkbdState swkbd;
    char textBuffer[256];
    char keyboardPacket[320]; // Buffer for the keyboard packet (K:text)
    
    // Initialize the keyboard with appropriate settings
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, "Type message to send to PC");
    swkbdSetFeatures(&swkbd, SWKBD_MULTILINE);
    
    // Show the keyboard and get input
    SwkbdButton button = swkbdInputText(&swkbd, textBuffer, sizeof(textBuffer));
    
    // Only send if OK was pressed and text isn't empty
    if (button == SWKBD_BUTTON_CONFIRM && strlen(textBuffer) > 0) {
        // Format keyboard packet with prefix
        snprintf(keyboardPacket, sizeof(keyboardPacket), "%s%s", 
                 KEYBOARD_PACKET_PREFIX, textBuffer);
        
        // Send the keyboard packet ONCE - no retries
        sendto(sock, keyboardPacket, strlen(keyboardPacket), 0, 
               (struct sockaddr*)&serverAddr, sizeof(serverAddr));
        
        // Notify the user
        printf("Sent keyboard input: %s\n", textBuffer);
    } else {
        printf("Keyboard input canceled\n");
    }
}

int map_range(int value, int from_min, int from_max, int to_min, int to_max) {
    return (value - from_min) * (to_max - to_min) / (from_max - from_min) + to_min;
}

// Check if the Konami code sequence has been entered
bool checkKonamiCode(u32 key) {
    // Only track key down events
    if (key & KEY_DUP || key & KEY_DDOWN || key & KEY_DLEFT || key & KEY_DRIGHT ||
        key & KEY_A || key & KEY_B || key & KEY_START) {
        
        // Check if the current key matches the next in the sequence
        if ((key & konamiCode[konamiCodeIndex])) {
            konamiCodeIndex++;
            
            // Check if complete sequence entered
            if (konamiCodeIndex >= KONAMI_CODE_LENGTH) {
                printf("Konami code activated!\n");
                return true;
            }
        } else {
            // Reset sequence on wrong input
            konamiCodeIndex = 0;
            
            // If we got the first key wrong, check if it's the start of a new sequence
            if (key & konamiCode[0]) {
                konamiCodeIndex = 1;
            }
        }
    }
    return false;
}

void buttonsToString(u32 keys, circlePosition pos, touchPosition touch, char* buttons) {
    int keysFormatted = 0;
    
    // Face buttons
    if (keys & KEY_A) keysFormatted |= (1<<0);
    if (keys & KEY_B) keysFormatted |= (1<<1);
    if (keys & KEY_X) keysFormatted |= (1<<2);
    if (keys & KEY_Y) keysFormatted |= (1<<3);
    
    // D-pad
    if (keys & KEY_DUP) keysFormatted |= (1<<4);
    if (keys & KEY_DDOWN) keysFormatted |= (1<<5);
    if (keys & KEY_DLEFT) keysFormatted |= (1<<6);
    if (keys & KEY_DRIGHT) keysFormatted |= (1<<7);
    
    // Shoulder buttons
    if (keys & KEY_L) keysFormatted |= (1<<8);
    if (keys & KEY_R) keysFormatted |= (1<<9);
    
    // Special buttons
    if (keys & KEY_SELECT) keysFormatted |= (1<<10);
    if (keys & KEY_START) keysFormatted |= (1<<11);
    
    // Touch status
    bool touchActive = (keys & KEY_TOUCH) != 0;
    
    // Scale inputs with reduced resolution for better performance
    // Map from -160~160 to 0~CPAD_RESOLUTION, then scale to 0~32768
    int scaledCPadX = map_range(pos.dx, -160, 160, 0, CPAD_RESOLUTION);
    int scaledCPadY = map_range(pos.dy, -160, 160, 0, CPAD_RESOLUTION);
    
    // Scale back up to 0-32768 range for vJoy
    scaledCPadX = map_range(scaledCPadX, 0, CPAD_RESOLUTION, 0, 32768);
    scaledCPadY = map_range(scaledCPadY, 0, CPAD_RESOLUTION, 0, 32768);
    
    // Touch coordinates (only meaningful if touch is active)
    int touchX = touchActive ? touch.px : 0;
    int touchY = touchActive ? touch.py : 0;
    
    // Format with mode information: <buttons;cpadX;cpadY;touchActive;touchX;touchY;mode>
    sprintf(buttons, "<%d;%d;%d;%d;%d;%d;%d>", 
            keysFormatted, scaledCPadX, scaledCPadY, 
            touchActive ? 1 : 0, touchX, touchY,
            touch_mode_active ? 1 : 0);  // Add mode parameter
}

// Function to send reliable packets for critical information
void sendReliablePacket(const char* packet, size_t length) {
    // Send the packet multiple times to ensure delivery
    for (int i = 0; i < MAX_SEND_RETRIES; i++) {
        sendto(sock, packet, length, 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
        
        // Short delay between retries to avoid congestion but ensure delivery
        if (i < MAX_SEND_RETRIES - 1) {
            svcSleepThread(SEND_RETRY_DELAY);
        }
    }
}

// Initialize screenshot reception
void start_screenshot_reception(const char* data, size_t len) {
    // Parse header: total_size, total_chunks, width, height
    u32 total_size, total_chunks;
    u16 width, height;
    
    memcpy(&total_size, data + 9, sizeof(u32));
    memcpy(&total_chunks, data + 13, sizeof(u32));
    memcpy(&width, data + 17, sizeof(u16));
    memcpy(&height, data + 19, sizeof(u16));
    
    // Clean up any previous reception
    cleanup_screenshot_reception();
    
    // Allocate buffers
    screenshot_rx.buffer = (u8*)malloc(total_size);
    screenshot_rx.chunk_received_flags = (bool*)calloc(total_chunks, sizeof(bool));
    
    screenshot_rx.active = true;
    screenshot_rx.total_size = total_size;
    screenshot_rx.total_chunks = total_chunks;
    screenshot_rx.chunks_received = 0;
    screenshot_rx.width = width;
    screenshot_rx.height = height;
    screenshot_rx.start_time = svcGetSystemTick();
    screenshot_rx.timeout_ms = 5000;  // 5 second timeout
    screenshot_rx.display_screenshot = false;
    
    printf("Starting screenshot: %lu bytes, %lu chunks\n", (unsigned long)total_size, (unsigned long)total_chunks);
}

// Handle incoming chunk
void handle_screenshot_chunk(const char* data, size_t len) {
    if (!screenshot_rx.active) return;
    
    u32 sequence_num;
    memcpy(&sequence_num, data + 9, sizeof(u32));
    
    // Validate sequence number
    if (sequence_num >= screenshot_rx.total_chunks) {
        printf("Invalid chunk sequence: %lu\n", (unsigned long)sequence_num);
        return;
    }
    
    // Skip if we already have this chunk
    if (screenshot_rx.chunk_received_flags[sequence_num]) {
        return;
    }
    
    // Calculate offset and copy data
    u32 chunk_size = 1024;
    u32 offset = sequence_num * chunk_size;
    u32 data_size = len - 13;  // Subtract header size
    
    // Don't overflow buffer
    if (offset + data_size > screenshot_rx.total_size) {
        data_size = screenshot_rx.total_size - offset;
    }
    
    memcpy(screenshot_rx.buffer + offset, data + 13, data_size);
    
    // Mark chunk as received
    screenshot_rx.chunk_received_flags[sequence_num] = true;
    screenshot_rx.chunks_received++;
    
    printf("Chunk %lu/%lu received\n", (unsigned long)screenshot_rx.chunks_received, (unsigned long)screenshot_rx.total_chunks);
}

// Check if screenshot is complete or timed out
void check_screenshot_completion() {
    if (!screenshot_rx.active) return;
    
    // Check for timeout
    u64 current_time = svcGetSystemTick();
    u64 elapsed_ms = (current_time - screenshot_rx.start_time) * 1000 / SYSCLOCK_ARM11;
    
    if (elapsed_ms > screenshot_rx.timeout_ms) {
        printf("Screenshot reception timed out\n");
        cleanup_screenshot_reception();
        return;
    }
    
    // Check if all chunks received
    if (screenshot_rx.chunks_received == screenshot_rx.total_chunks) {
        printf("Screenshot complete!\n");
        screenshot_rx.display_screenshot = true;
        screenshot_rx.active = false;  // Stop receiving but keep data for display
    }
}

// Display the screenshot on bottom screen
void display_screenshot() {
    if (!screenshot_rx.display_screenshot || !screenshot_rx.buffer) return;
    
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    
    // The 3DS framebuffer is in BGR888 format, our data is RGB565
    // We need to convert while copying
    for (int y = 0; y < screenshot_rx.height; y++) {
        for (int x = 0; x < screenshot_rx.width; x++) {
            // Get RGB565 pixel
            int src_idx = (y * screenshot_rx.width + x) * 2;
            u16 rgb565 = screenshot_rx.buffer[src_idx] | 
                        (screenshot_rx.buffer[src_idx + 1] << 8);
            
            // Extract RGB components
            u8 r = ((rgb565 >> 11) & 0x1F) << 3;
            u8 g = ((rgb565 >> 5) & 0x3F) << 2;
            u8 b = (rgb565 & 0x1F) << 3;
            
            // Write to framebuffer in BGR format with rotation
            int fb_idx = ((x * 240) + (239 - y)) * 3;
            fb[fb_idx] = b;
            fb[fb_idx + 1] = g;
            fb[fb_idx + 2] = r;
        }
    }
    
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

// Clean up screenshot resources
void cleanup_screenshot_reception() {
    if (screenshot_rx.buffer) {
        free(screenshot_rx.buffer);
        screenshot_rx.buffer = NULL;
    }
    if (screenshot_rx.chunk_received_flags) {
        free(screenshot_rx.chunk_received_flags);
        screenshot_rx.chunk_received_flags = NULL;
    }
    screenshot_rx.active = false;
    screenshot_rx.display_screenshot = false;
}

void clear_screenshot() {
    if (screenshot_rx.display_screenshot) {
        screenshot_rx.display_screenshot = false;
        
        // Clear bottom screen
        u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        if (fb) {
            memset(fb, 0, 320 * 240 * 3); // Fill with black
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
        
        // Free resources
        cleanup_screenshot_reception();
        
        printf("Screenshot cleared\n");
    }
}

// Periodic mode sync function
void sendModeSyncPacket(u64 currentTime) {
    static char syncPacket[64];
    
    // Send mode sync packet every 1 second
    if (currentTime - lastModeSyncTime > MODE_SYNC_INTERVAL) {
        // Create a special mode sync packet with default values but correct mode
        sprintf(syncPacket, "<0;16384;16384;0;0;0;%d>", touch_mode_active ? 1 : 0);
        
        // Send mode sync packet reliably
        sendReliablePacket(syncPacket, strlen(syncPacket));
               
        lastModeSyncTime = currentTime;
    }
}

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
    int ret;
    char buttons[64]; // Increased buffer size for the expanded input format
    static SwkbdState swkbd;

    gfxInitDefault();

    // register gfxExit to be run when app quits
    // this can help simplify error handling
    atexit(gfxExit);

    consoleInit(GFX_TOP, NULL);

    printf("\n3DS UDP Controller v3.1\n");
    printf("-------------------\n");

    // allocate buffer for SOC service
    SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);

    if(SOC_buffer == NULL) {
        failExit("memalign: failed to allocate\n");
    }

    // Now intialise soc:u service
    if ((ret = socInit(SOC_buffer, SOC_BUFFERSIZE)) != 0) {
        failExit("socInit: 0x%08X\n", (unsigned int)ret);
    }
    printf("Initialized socket service\n");

    // register socShutdown to run at exit
    // atexit functions execute in reverse order so this runs before gfxExit
    atexit(socShutdown);

    // libctru provides BSD sockets so most code from here is standard
    // Using UDP for more realtime connection
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock < 0) {
        failExit("UDP socket failed: %d %s\n", errno, strerror(errno));
    }
    printf("Created UDP socket\n");
    
    // Set socket send buffer size to improve performance
    int sendbuf = 8192;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf)) < 0) {
        printf("Warning: Could not set socket buffer size\n");
    }
    
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(9999);

    // Get IP address from user
    getAddressText(swkbd);
    printf("Address: %s\n", address);
    
    // Set the IP address in serverAddr
    if (inet_pton(AF_INET, address, &serverAddr.sin_addr) <= 0) {
        failExit("\nInvalid address/ Address not supported\n");
    }

    // Set socket non blocking so we can still read input to exit
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    
    printf("Ready to send UDP packets to %s:9999\n", address);
    printf("Controls:\n");
    printf("- All buttons and Circle Pad are mapped to vJoy\n");
    printf("- Touch screen controls mouse movement\n");
    printf("- L button = left mouse click\n");
    printf("- R button = right mouse click\n");
    printf("- L+R+Select = Toggle Touch Mode\n");
    printf("  * In touch mode, tap to left click, R button to right click\n");
    printf("- L+R+Start = Open keyboard to send text\n");
    printf("- L+R+Select+Start = Take screenshot\n");
    printf("- Select+Start = Clear displayed screenshot\n");
    printf("- Konami code to exit (Up,Up,Down,Down,Left,Right,Left,Right,B,A,Start)\n\n");
    
    static u32 lastKeys = 0;
    static circlePosition lastPos = {0};
    static touchPosition lastTouch = {0};
    static bool lastTouchActive = false;
    
    // Initialize time trackers
    u64 currentTime = svcGetSystemTick();
    lastTouchTime = currentTime;
    lastConsoleUpdate = currentTime;
    lastHeldUpdate = currentTime;
    touch_mode_last_time = currentTime;
    lastModeSyncTime = currentTime;
    keyboard_last_toggle = currentTime;
    screenshot_last_toggle = currentTime;
    
    // Make sure screenshot state is initialized
    cleanup_screenshot_reception();
    
    // For display status tracking
    static bool was_screenshot_displaying = false;
    
    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
    
        circlePosition pos;
        touchPosition touch;
        hidCircleRead(&pos);
        hidTouchRead(&touch);
        
        u32 keys = hidKeysHeld();
        bool touchActive = (keys & KEY_TOUCH) != 0;
        bool shouldSendUpdate = false;
        currentTime = svcGetSystemTick();

        // Check for Konami code using direct function call
        if (checkKonamiCode(hidKeysDown())) {
            printf("Exit code activated!\n");
            sendto(sock, "d", strlen("d"), 0, 
                  (struct sockaddr*)&serverAddr, sizeof(serverAddr));
            break;
        }

        // select + start to clear screenshot
        if ((keys & (KEY_SELECT | KEY_START)) == (KEY_SELECT | KEY_START) && 
            !(keys & (KEY_L | KEY_R))) {
            // Only activate if enough time has passed since last activation
            if (currentTime - screenshot_last_toggle > SCREENSHOT_DEBOUNCE_TIME && 
                screenshot_rx.display_screenshot) {
                
                screenshot_last_toggle = currentTime;
                printf("Clearing displayed screenshot\n");
                clear_screenshot();
            }
        }

        // Check for screenshot activation (L+R+Select+Start) with proper debouncing
        else if ((keys & SCREENSHOT_TRIGGER_BUTTONS) == SCREENSHOT_TRIGGER_BUTTONS) {
            // Only activate if enough time has passed since last activation
            // AND we're not already receiving a screenshot
            if (currentTime - screenshot_last_toggle > SCREENSHOT_DEBOUNCE_TIME && 
                !screenshot_rx.active && !screenshot_rx.display_screenshot) {
                
                screenshot_last_toggle = currentTime;
                printf("Screenshot requested\n");
                
                // Send screenshot request packet
                char screenshotPacket[32];
                snprintf(screenshotPacket, sizeof(screenshotPacket), "%sscreenshot", 
                        SCREENSHOT_PACKET_PREFIX);
                
                // Send reliably since it's important
                sendReliablePacket(screenshotPacket, strlen(screenshotPacket));
            }
        }
        
        // Check for keyboard activation (L+R+Start)
        else if ((keys & KEYBOARD_TRIGGER_BUTTONS) == KEYBOARD_TRIGGER_BUTTONS) {
            // Only activate if enough time has passed since last activation
            if (currentTime - keyboard_last_toggle > KEYBOARD_DEBOUNCE_TIME) {
                keyboard_last_toggle = currentTime;
                printf("Keyboard activated\n");
                
                // Show keyboard and send input
                handleKeyboardInput();
            }
        }
    
        // Check for touch mode toggle with debouncing
        else if ((keys & TOUCH_MODE_BUTTONS) == TOUCH_MODE_BUTTONS) {
            // Only toggle if enough time has passed since last toggle
            if (currentTime - touch_mode_last_toggle > TOUCH_MODE_DEBOUNCE_TIME) {
                touch_mode_active = !touch_mode_active;
                touch_mode_last_toggle = currentTime;
                
                if (touch_mode_active) {
                    printf("TOUCH MODE ACTIVATED - Optimized for drawing\n");
                } else {
                    printf("TOUCH MODE DEACTIVATED - Normal control mode\n");
                }
                
                // Send update immediately to sync with server
                shouldSendUpdate = true;
                
                // Send multiple updates to ensure mode change is received
                char modeChangePacket[64];
                buttonsToString(keys, pos, touch, modeChangePacket);
                
                // Use reliable packet sending for important mode change
                sendReliablePacket(modeChangePacket, strlen(modeChangePacket));
            }
        }
        
        // Send periodic mode sync packet
        sendModeSyncPacket(currentTime);
        
        // Touch mode processing
        if (touch_mode_active && touchActive) {
            // In touch mode, send updates much more frequently
            if (currentTime - touch_mode_last_time > TOUCH_MODE_THROTTLE_TIME) {
                // Even smaller threshold for movement
                int dx = touch.px - lastTouch.px;
                int dy = touch.py - lastTouch.py;
                
                if ((dx*dx + dy*dy) > (TOUCH_MODE_THRESHOLD*TOUCH_MODE_THRESHOLD)) {
                    shouldSendUpdate = true;
                    touch_mode_last_time = currentTime;
                }
            }
        } else {
            // Normal mode logic
            // Always send updates for new button presses or releases
            if (keys != lastKeys) {
                shouldSendUpdate = true;
            }
            
            // Send updates for significant circle pad movements
            if (abs(pos.dx - lastPos.dx) > CPAD_THRESHOLD || 
                abs(pos.dy - lastPos.dy) > CPAD_THRESHOLD) {
                shouldSendUpdate = true;
            }
            
            // Periodically send updates even for held inputs
            if (keys != 0 && currentTime - lastHeldUpdate > HELD_INPUT_UPDATE_TIME) {
                shouldSendUpdate = true;
                lastHeldUpdate = currentTime;
            }

            // For touch, add throttling and threshold checks
            if (touchActive != lastTouchActive) {
                // Always send on touch state change (press/release)
                shouldSendUpdate = true;
                lastTouchTime = currentTime; // Reset timer on state change
            } else if (touchActive) {
                // For continued touch, use threshold and throttling
                int dx = touch.px - lastTouch.px;
                int dy = touch.py - lastTouch.py;
                
                // Use squared distance for performance (avoid sqrt)
                if ((dx*dx + dy*dy) > (TOUCH_THRESHOLD*TOUCH_THRESHOLD)) {
                    if (currentTime - lastTouchTime > TOUCH_THROTTLE_TIME) {
                        shouldSendUpdate = true;
                        lastTouchTime = currentTime;
                    }
                }
            }
        }

        if (shouldSendUpdate) {
            // Update last states
            lastKeys = keys;
            lastPos = pos;
            lastTouch = touch;
            lastTouchActive = touchActive;
            
            // Create formatted input string
            buttonsToString(keys, pos, touch, buttons);
            
            // Send updated button data with extra reliability for touch events
            if (touch_mode_active && (touchActive || lastTouchActive)) {
                // In touch mode, touch events need extra reliability
                sendReliablePacket(buttons, strlen(buttons));
            } else {
                // Regular sends for non-critical updates
                int sent = sendto(sock, buttons, strlen(buttons), 0,
                            (struct sockaddr*)&serverAddr, sizeof(serverAddr));
                
                // Handle console output - throttle to avoid slowdown
                if (currentTime - lastConsoleUpdate > CONSOLE_UPDATE_INTERVAL) {
                    if (sent < 0) {
                        printf("Send error: %d - %s\n", errno, strerror(errno));
                    } else if (!touchActive || !(keys & (KEY_L | KEY_R))) {
                        // Only print for non-touch or when not clicking to reduce spam
                        printf("Sent: %s\n", buttons);
                    }
                    lastConsoleUpdate = currentTime;
                }
            }
        }
        
        // Check for incoming data (screenshot chunks)
        char recv_buffer[1500];  // Larger buffer for screenshot chunks
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        
        ssize_t recv_len = recvfrom(sock, recv_buffer, sizeof(recv_buffer), 0,
                                  (struct sockaddr*)&from_addr, &from_len);
        
        if (recv_len > 0) {
            // Null-terminate for string operations
            if (recv_len < sizeof(recv_buffer)) {
                recv_buffer[recv_len] = '\0';
            } else {
                recv_buffer[sizeof(recv_buffer) - 1] = '\0';
            }
            
            // Check for screenshot packets
            if (strncmp(recv_buffer, "IMGSTART:", 9) == 0) {
                printf("Received IMGSTART packet (%d bytes)\n", (int)recv_len);
                start_screenshot_reception(recv_buffer, recv_len);
            }
            else if (strncmp(recv_buffer, "IMGCHUNK:", 9) == 0) {
                // Process the chunk (don't log every chunk to avoid spam)
                handle_screenshot_chunk(recv_buffer, recv_len);
            }
            else if (strncmp(recv_buffer, "IMGEND:", 7) == 0) {
                printf("Received IMGEND packet\n");
                
                // If we have most chunks, display anyway
                if (screenshot_rx.active && 
                    screenshot_rx.chunks_received >= screenshot_rx.total_chunks * 0.9) {
                    printf("Displaying partial screenshot after END (%lu%% complete)\n", 
                           (unsigned long)(screenshot_rx.chunks_received * 100 / screenshot_rx.total_chunks));
                    screenshot_rx.display_screenshot = true;
                    screenshot_rx.active = false;
                }
            }
        }
        
        // Check if screenshot is complete
        check_screenshot_completion();
        
        // Display screenshot if available
        if (screenshot_rx.display_screenshot) {
            if (!was_screenshot_displaying) {
                display_screenshot();
                was_screenshot_displaying = true;
            }
        } else {
            was_screenshot_displaying = false;
        }
    
        // In touch mode, use shorter sleep time for higher responsiveness
        if (touch_mode_active) {
            svcSleepThread(8000000); // ~120fps pacing
        } else {
            svcSleepThread(16000000); // ~60fps pacing
        }
    }
    
    // Clean up screenshot resources
    cleanup_screenshot_reception();
    
    close(sock);
    return 0;
}

//---------------------------------------------------------------------------------
void failExit(const char *fmt, ...) {
//---------------------------------------------------------------------------------
    if(sock>0) close(sock);
    if(csock>0) close(csock);

    va_list ap;

    printf(CONSOLE_RED);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf(CONSOLE_RESET);
    printf("\nPress B to exit\n");

    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();

        u32 kDown = hidKeysDown();
        if (kDown & KEY_B) exit(0);
    }
}MODE_THROTTLE_TIME) {
                // Even smaller threshold for movement
                int dx = touch.px - lastTouch.px;
                int dy = touch.py - lastTouch.py;
                
                if ((dx*dx + dy*dy) > (TOUCH_MODE_THRESHOLD*TOUCH_MODE_THRESHOLD)) {
                    shouldSendUpdate = true;
                    touch_mode_last_time = currentTime;
                }
            }
        } else {
            // Normal mode logic
            // Always send updates for new button presses or releases
            if (keys != lastKeys) {
                shouldSendUpdate = true;
            }
            
            // Send updates for significant circle pad movements
            if (abs(pos.dx - lastPos.dx) > CPAD_THRESHOLD || 
                abs(pos.dy - lastPos.dy) > CPAD_THRESHOLD) {
                shouldSendUpdate = true;
            }
            
            // Periodically send updates even for held inputs
            if (keys != 0 && currentTime - lastHeldUpdate > HELD_INPUT_UPDATE_TIME) {
                shouldSendUpdate = true;
                lastHeldUpdate = currentTime;
            }

            // For touch, add throttling and threshold checks
            if (touchActive != lastTouchActive) {
                // Always send on touch state change (press/release)
                shouldSendUpdate = true;
                lastTouchTime = currentTime; // Reset timer on state change
            } else if (touchActive) {
                // For continued touch, use threshold and throttling
                int dx = touch.px - lastTouch.px;
                int dy = touch.py - lastTouch.py;
                
                // Use squared distance for performance (avoid sqrt)
                if ((dx*dx + dy*dy) > (TOUCH_THRESHOLD*TOUCH_THRESHOLD)) {
                    if (currentTime - lastTouchTime > TOUCH_THROTTLE_TIME) {
                        shouldSendUpdate = true;
                        lastTouchTime = currentTime;
                    }
                }
            }
        }

        if (shouldSendUpdate) {
            // Update last states
            lastKeys = keys;
            lastPos = pos;
            lastTouch = touch;
            lastTouchActive = touchActive;
            
            // Create formatted input string
            buttonsToString(keys, pos, touch, buttons);
            
            // Send updated button data with extra reliability for touch events
            if (touch_mode_active && (touchActive || lastTouchActive)) {
                // In touch mode, touch events need extra reliability
                sendReliablePacket(buttons, strlen(buttons));
            } else {
                // Regular sends for non-critical updates
                int sent = sendto(sock, buttons, strlen(buttons), 0,
                            (struct sockaddr*)&serverAddr, sizeof(serverAddr));
                
                // Handle console output - throttle to avoid slowdown
                if (currentTime - lastConsoleUpdate > CONSOLE_UPDATE_INTERVAL) {
                    if (sent < 0) {
                        printf("Send error: %d - %s\n", errno, strerror(errno));
                    } else if (!touchActive || !(keys & (KEY_L | KEY_R))) {
                        // Only print for non-touch or when not clicking to reduce spam
                        printf("Sent: %s\n", buttons);
                    }
                    lastConsoleUpdate = currentTime;
                }
            }
        }
        
        // Check for incoming data (screenshot chunks)
        char recv_buffer[1500];  // Larger buffer for screenshot chunks
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        
        ssize_t recv_len = recvfrom(sock, recv_buffer, sizeof(recv_buffer), 0,
                                  (struct sockaddr*)&from_addr, &from_len);
        
        if (recv_len > 0) {
            // Null-terminate for string operations
            if (recv_len < sizeof(recv_buffer)) {
                recv_buffer[recv_len] = '\0';
            } else {
                recv_buffer[sizeof(recv_buffer) - 1] = '\0';
            }
            
            // Check for screenshot packets
            if (strncmp(recv_buffer, "IMGSTART:", 9) == 0) {
                printf("Received IMGSTART packet (%d bytes)\n", (int)recv_len);
                start_screenshot_reception(recv_buffer, recv_len);
            }
            else if (strncmp(recv_buffer, "IMGCHUNK:", 9) == 0) {
                // Process the chunk (don't log every chunk to avoid spam)
                handle_screenshot_chunk(recv_buffer, recv_len);
            }
            else if (strncmp(recv_buffer, "IMGEND:", 7) == 0) {
                printf("Received IMGEND packet\n");
                
                // If we have most chunks, display anyway
                if (screenshot_rx.active && 
                    screenshot_rx.chunks_received >= screenshot_rx.total_chunks * 0.9) {
                    printf("Displaying partial screenshot after END (%lu%% complete)\n", 
                           (unsigned long)(screenshot_rx.chunks_received * 100 / screenshot_rx.total_chunks));
                    screenshot_rx.display_screenshot = true;
                    screenshot_rx.active = false;
                }
            }
        }
        
        // Check if screenshot is complete
        check_screenshot_completion();
        
        // Display screenshot if available
        if (screenshot_rx.display_screenshot) {
            if (!was_screenshot_displaying) {
                display_screenshot();
                was_screenshot_displaying = true;
            }
        } else {
            was_screenshot_displaying = false;
        }
    
        // In touch mode, use shorter sleep time for higher responsiveness
        if (touch_mode_active) {
            svcSleepThread(8000000); // ~120fps pacing
        } else {
            svcSleepThread(16000000); // ~60fps pacing
        }
    }
    
    // Clean up screenshot resources
    cleanup_screenshot_reception();
    
    close(sock);
    return 0;
}

//---------------------------------------------------------------------------------
void failExit(const char *fmt, ...) {
//---------------------------------------------------------------------------------
    if(sock>0) close(sock);
    if(csock>0) close(csock);

    va_list ap;

    printf(CONSOLE_RED);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf(CONSOLE_RESET);
    printf("\nPress B to exit\n");

    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();

        u32 kDown = hidKeysDown();
        if (kDown & KEY_B) exit(0);
    }
}