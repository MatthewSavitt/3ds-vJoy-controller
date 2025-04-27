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

// Touch input optimization settings
#define TOUCH_THRESHOLD 8      // Minimum pixel change to trigger updates (increased for less sensitivity)
#define TOUCH_THROTTLE_TIME 33000000  // About 30fps (in system ticks)
#define CONSOLE_UPDATE_INTERVAL 180000000 // Only update console every ~3 seconds to prevent slowdown

static u32 *SOC_buffer = NULL;
s32 sock = -1, csock = -1;
struct sockaddr_in serverAddr;
static char address[60];

// Konami code sequence and tracking
#define KONAMI_CODE_LENGTH 11
u32 konamiCode[KONAMI_CODE_LENGTH] = {
    KEY_DUP, KEY_DUP, KEY_DDOWN, KEY_DDOWN, 
    KEY_DLEFT, KEY_DRIGHT, KEY_DLEFT, KEY_DRIGHT, 
    KEY_B, KEY_A, KEY_START
};
int konamiCodeIndex = 0;
u64 lastTouchTime = 0;    // For throttling touch updates
u64 lastConsoleUpdate = 0; // For throttling console output

__attribute__((format(printf,1,2)))
void failExit(const char *fmt, ...);

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
    
    // Scale inputs to proper ranges
    int scaledCPadX = map_range(pos.dx, -160, 160, 0, 32768);
    int scaledCPadY = map_range(pos.dy, -160, 160, 0, 32768);
    
    // Touch coordinates (only meaningful if touch is active)
    int touchX = touchActive ? touch.px : 0;
    int touchY = touchActive ? touch.py : 0;
    
    // Format: <buttons;cpadX;cpadY;touchActive;touchX;touchY>
    sprintf(buttons, "<%d;%d;%d;%d;%d;%d>", 
            keysFormatted, scaledCPadX, scaledCPadY, 
            touchActive ? 1 : 0, touchX, touchY);
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

    printf("\n3DS UDP Controller\n");
    printf("-----------------\n");

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
    printf("- Konami code to exit (Up,Up,Down,Down,Left,Right,Left,Right,B,A,Start)\n\n");
    
    static u32 lastKeys = 0;
    static circlePosition lastPos = {0};
    static touchPosition lastTouch = {0};
    static bool lastTouchActive = false;
    
    // Initialize time trackers
    lastTouchTime = svcGetSystemTick();
    lastConsoleUpdate = svcGetSystemTick();
    
    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
    
        circlePosition pos;
        touchPosition touch;
        hidCircleRead(&pos);
        hidTouchRead(&touch);
        
        u32 keys = hidKeysHeld();
        // Use the function directly instead of storing in a variable named keysDown
        bool touchActive = (keys & KEY_TOUCH) != 0;
        bool shouldSendUpdate = false;

        // Check for Konami code using direct function call
        if (checkKonamiCode(hidKeysDown())) {
            printf("Exit code activated!\n");
            sendto(sock, "d", strlen("d"), 0, 
                  (struct sockaddr*)&serverAddr, sizeof(serverAddr));
            break;
        }
    
        // Always send updates for button changes and circle pad movement
        if (keys != lastKeys || pos.dx != lastPos.dx || pos.dy != lastPos.dy) {
            shouldSendUpdate = true;
        }

        // For touch, add throttling and threshold checks
        if (touchActive != lastTouchActive) {
            // Always send on touch state change (press/release)
            shouldSendUpdate = true;
        } else if (touchActive) {
            // For continued touch, use threshold and throttling
            int dx = touch.px - lastTouch.px;
            int dy = touch.py - lastTouch.py;
            
            // Use squared distance for performance (avoid sqrt)
            if ((dx*dx + dy*dy) > (TOUCH_THRESHOLD*TOUCH_THRESHOLD)) {
                u64 currentTime = svcGetSystemTick();
                if (currentTime - lastTouchTime > TOUCH_THROTTLE_TIME) {
                    shouldSendUpdate = true;
                    lastTouchTime = currentTime;
                }
            }
        }

        if (shouldSendUpdate) {
            lastKeys = keys;
            lastPos = pos;
            lastTouch = touch;
            lastTouchActive = touchActive;
            
            // Create formatted input string
            buttonsToString(keys, pos, touch, buttons);
            
            // Send updated button data
            int sent = sendto(sock, buttons, strlen(buttons), 0,
                         (struct sockaddr*)&serverAddr, sizeof(serverAddr));
            
            // Handle console output - throttle to avoid slowdown
            u64 currentTime = svcGetSystemTick();
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
    
        svcSleepThread(16000000); // ~60fps pacing
    }
    
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