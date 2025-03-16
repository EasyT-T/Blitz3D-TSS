#include "std.h"
#include "gxinput.h"

#include "gxruntime.h"

#include <xinput.h>

static const int QUE_SIZE = 32;

XInputGetStatePtr XInputGetStateFunc = nullptr;
XInputSetStatePtr XInputSetStateFunc = nullptr;
HMODULE xinputLibrary = nullptr;

void UnloadXInput() {
    if (xinputLibrary) {
        FreeLibrary(xinputLibrary);
        xinputLibrary = nullptr;
        XInputGetStateFunc = nullptr;
        XInputSetStateFunc = nullptr;
    }
}

bool LoadXInput() {
    // Try to load xinput1_4.dll (Windows 8 and later) for analysis
    xinputLibrary = LoadLibraryW(L"xinput1_4.dll");
    if (!xinputLibrary) {
        // Fall back to xinput9_1_0.dll (Windows 7 and earlier) for analysis
        xinputLibrary = LoadLibraryW(L"xinput9_1_0.dll");
    }

    // Once we're able to get the dll loaded, check for specific function pointer addresses
    if (xinputLibrary) {
        XInputGetStateFunc = (XInputGetStatePtr)GetProcAddress(xinputLibrary, "XInputGetState");
        XInputSetStateFunc = (XInputSetStatePtr)GetProcAddress(xinputLibrary, "XInputSetState");
        if (XInputGetStateFunc && XInputSetStateFunc) {
            // All good and defined, return success
            return true;
        }
        // Only unload here since it wouldn't succeed it if we didn't get here in the first place
        UnloadXInput();
    }
    return false;
}

class Device : public gxDevice {
public:
    bool acquired;
    gxInput* input;

    explicit Device(gxInput* i) : acquired(false), input(i) {
    }

    ~Device() override = default;

    bool acquire() {
        return acquired = true;
    }
    void unacquire() {
        acquired = false;
    }
};

class Keyboard : public Device {
public:
    explicit Keyboard(gxInput* i) : Device(i) {}
};

class Mouse : public Device {
public:
    Mouse(gxInput* i) : Device(i) {
    }
};

static Keyboard* keyboard;
static Mouse* mouse;
static std::vector<int> chars;

static Keyboard* createKeyboard(gxInput* input) {

    return new Keyboard(input);
}

static Mouse* createMouse(gxInput* input) {
    return new Mouse(input);
}

gxInput::gxInput(gxRuntime* rt) :
    runtime(rt) {
    keyboard = createKeyboard(this);
    mouse = createMouse(this);

    if (LoadXInput()) {
        for (int i = 0; i < XUSER_MAX_COUNT; i++) {
            if (XInputGetStateFunc) {
                XINPUT_STATE state;
                if (XInputGetStateFunc(i, &state) == ERROR_SUCCESS) {
                    xinput_controllers.push_back(new XInputController(i));
                }
            }
        }
    }
}

gxInput::~gxInput() {
    for (size_t i = 0; i < xinput_controllers.size(); ++i) {
        delete xinput_controllers[i];
    }
    xinput_controllers.clear();

    delete mouse;
    delete keyboard;

    UnloadXInput();
}

void gxInput::wm_keydown(int key) {
    if (keyboard) keyboard->downEvent(key);
}

void gxInput::wm_keyup(int key) {
    if (keyboard) keyboard->upEvent(key);
}

void gxInput::wm_mousedown(int key) {
    if (mouse) mouse->downEvent(key);
}

void gxInput::wm_mouseup(int key) {
    if (mouse) mouse->upEvent(key);
}

void gxInput::wm_mousemove(int x, int y) {
    if (mouse) {
        mouse->axis_states[0] = x;
        mouse->axis_states[1] = y;
    }
}

void gxInput::wm_mousewheel(int dz) {
    if (mouse) mouse->axis_states[2] += dz;
}

void gxInput::wm_char(int wParam, int lParam) {
    int repeats = lParam & 0xffff;
    for (int i = 0; i < repeats; i++) {
        chars.push_back(wParam);
    }
}

void gxInput::reset() {
    if (mouse) mouse->reset();
    if (keyboard) keyboard->reset();
}

bool gxInput::acquire() {
    bool m_ok = true, k_ok = true;
    if (mouse) m_ok = mouse->acquire();
    if (keyboard) k_ok = keyboard->acquire();
    if (m_ok && k_ok) return true;
    if (k_ok) keyboard->unacquire();
    if (m_ok) mouse->unacquire();
    return false;
}

void gxInput::unacquire() {
    if (keyboard) keyboard->unacquire();
    if (mouse) mouse->unacquire();
}

void gxInput::moveMouse(int x, int y) {
    if (!mouse) return;
    mouse->axis_states[0] = x;
    mouse->axis_states[1] = y;
    runtime->moveMouse(x, y);
}

gxDevice* gxInput::getMouse()const {
    return mouse;
}

gxDevice* gxInput::getKeyboard()const {
    return keyboard;
}

bool gxInput::getControllerConnected(int port) {
    if (port < xinput_controllers.size()) {
        XINPUT_STATE state;
        if (XInputGetStateFunc) {
            return XInputGetStateFunc(xinput_controllers[port]->index, &state) == ERROR_SUCCESS;
        }
    }

    return false;
}

gxDevice* gxInput::getJoystick(int n)const {
    return xinput_controllers[n % xinput_controllers.size()];
}

std::vector<int> gxInput::getChars() {
    std::vector<int> chrs = chars;
    chars.clear();
    return chrs;
}

int gxInput::getJoystickType(int n)const {
    return 3;
}

int gxInput::numJoysticks()const {
    return xinput_controllers.size();
}

int gxInput::toAscii(int scan)const {
    scan &= 0x7f;
    const int virt = MapVirtualKey(scan, MAPVK_VSC_TO_VK);
    if (!virt) return 0;

    switch (virt) {
    case VK_INSERT:return ASC_INSERT;
    case VK_DELETE:return ASC_DELETE;
    case VK_HOME:return ASC_HOME;
    case VK_END:return ASC_END;
    case VK_PRIOR:return ASC_PAGEUP;
    case VK_NEXT:return ASC_PAGEDOWN;
    case VK_UP:return ASC_UP;
    case VK_DOWN:return ASC_DOWN;
    case VK_LEFT:return ASC_LEFT;
    case VK_RIGHT:return ASC_RIGHT;
    }

    static unsigned char keyboardState[256];

    if (!GetKeyboardState(keyboardState)) {
        return 0;
    }

    WORD ch;
    if (ToAscii(virt, scan, keyboardState, &ch, 0) != 1) return 0;
    return ch & 255;
}

XInputController::XInputController(int idx) : index(idx) {
    memset(&state, 0, sizeof(XINPUT_STATE));
    memset(&prev_state, 0, sizeof(XINPUT_STATE));
    reset();
}

void XInputController::update() {
    prev_state = state;
    if (XInputGetStateFunc) {
        if (XInputGetStateFunc(index, &state) != ERROR_SUCCESS) {
            memset(&state, 0, sizeof(XINPUT_STATE));
            return;
        }
    }

    // This based off mapping Xbox One Controller keys, unsure if 1:1 to other controllers
    // For Thumbstick support
    axis_states[0] = state.Gamepad.sThumbLX / 32767.0f;  // Left Thumbstick X
    axis_states[1] = state.Gamepad.sThumbLY / 32767.0f;  // Left Thumbstick Y
    axis_states[3] = state.Gamepad.sThumbRX / 32767.0f;  // Right Thumbstick X
    axis_states[4] = state.Gamepad.sThumbRY / 32767.0f;  // Right Thumbstick Y

    //Controller Triggers
    axis_states[5] = state.Gamepad.bLeftTrigger / 255.0f;   // Left Trigger, goes from (0 to 1) based on controller pressure
    axis_states[6] = state.Gamepad.bRightTrigger / 255.0f;  // Right Trigger, goes from (0 to 1) based on controller pressure

    // Differential Trigger (for backward compatibility :P with Z)
    axis_states[2] = axis_states[5] - axis_states[6];  // -1 to 1 range

    // Update button states
    const WORD buttons = state.Gamepad.wButtons;
    for (int i = 0; i < 23; ++i) {
        setDownState(i, (buttons & (1 << i)) ? true : false);
    }

    // D-PAD/POV Hat hook
    int pov = -1;
    if (buttons & XINPUT_GAMEPAD_DPAD_UP) {
        if (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) pov = 45;
        else if (buttons & XINPUT_GAMEPAD_DPAD_LEFT) pov = 315;
        else pov = 0;
    }
    else if (buttons & XINPUT_GAMEPAD_DPAD_DOWN) {
        if (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) pov = 135;
        else if (buttons & XINPUT_GAMEPAD_DPAD_LEFT) pov = 225;
        else pov = 180;
    }
    else if (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) {
        pov = 90;
    }
    else if (buttons & XINPUT_GAMEPAD_DPAD_LEFT) {
        pov = 270;
    }
    axis_states[8] = pov; // Store for JoyHat support later
}