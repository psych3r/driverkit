#include "driverkit.hpp"
#include <exception>
#include <CoreGraphics/CoreGraphics.h>

// Magic marker to identify our own CGEvent output (prevents CGEventTap feedback loop)
#define KANATA_EVENT_MARKER 0x4B4E5441ULL // "KNTA"

// CGEventTap state
static bool cgeventtap_active = false;
static CFMachPortRef cgeventtap_ref = NULL;

// Reverse mapping: HID usage code (page 0x07) -> macOS CGKeyCode
// Index = HID usage code, value = CGKeyCode, 0xFF = unmapped
static const uint16_t hid_to_cg[256] = {
    [0x04] = 0x00, // a
    [0x05] = 0x0B, // b
    [0x06] = 0x08, // c
    [0x07] = 0x02, // d
    [0x08] = 0x0E, // e
    [0x09] = 0x03, // f
    [0x0A] = 0x05, // g
    [0x0B] = 0x04, // h
    [0x0C] = 0x22, // i
    [0x0D] = 0x26, // j
    [0x0E] = 0x28, // k
    [0x0F] = 0x25, // l
    [0x10] = 0x2E, // m
    [0x11] = 0x2D, // n
    [0x12] = 0x1F, // o
    [0x13] = 0x23, // p
    [0x14] = 0x0C, // q
    [0x15] = 0x0F, // r
    [0x16] = 0x01, // s
    [0x17] = 0x11, // t
    [0x18] = 0x20, // u
    [0x19] = 0x09, // v
    [0x1A] = 0x0D, // w
    [0x1B] = 0x07, // x
    [0x1C] = 0x10, // y
    [0x1D] = 0x06, // z
    [0x1E] = 0x12, // 1
    [0x1F] = 0x13, // 2
    [0x20] = 0x14, // 3
    [0x21] = 0x15, // 4
    [0x22] = 0x17, // 5
    [0x23] = 0x16, // 6
    [0x24] = 0x1A, // 7
    [0x25] = 0x1C, // 8
    [0x26] = 0x19, // 9
    [0x27] = 0x1D, // 0
    [0x28] = 0x24, // Return
    [0x29] = 0x35, // Escape
    [0x2A] = 0x33, // Backspace
    [0x2B] = 0x30, // Tab
    [0x2C] = 0x31, // Space
    [0x2D] = 0x1B, // -
    [0x2E] = 0x18, // =
    [0x2F] = 0x21, // [
    [0x30] = 0x1E, // ]
    [0x31] = 0x2A, // backslash
    [0x33] = 0x29, // ;
    [0x34] = 0x27, // '
    [0x35] = 0x32, // `
    [0x36] = 0x2B, // ,
    [0x37] = 0x2F, // .
    [0x38] = 0x2C, // /
    [0x39] = 0x39, // Caps Lock
    [0x3A] = 0x7A, // F1
    [0x3B] = 0x78, // F2
    [0x3C] = 0x63, // F3
    [0x3D] = 0x76, // F4
    [0x3E] = 0x60, // F5
    [0x3F] = 0x61, // F6
    [0x40] = 0x62, // F7
    [0x41] = 0x64, // F8
    [0x42] = 0x65, // F9
    [0x43] = 0x6D, // F10
    [0x44] = 0x67, // F11
    [0x45] = 0x6F, // F12
    [0x46] = 0x69, // F13 / PrintScreen
    [0x47] = 0x6B, // F14 / ScrollLock
    [0x48] = 0x71, // F15 / Pause
    [0x49] = 0x72, // Insert / Help
    [0x4A] = 0x73, // Home
    [0x4B] = 0x74, // Page Up
    [0x4C] = 0x75, // Delete Forward
    [0x4D] = 0x77, // End
    [0x4E] = 0x79, // Page Down
    [0x4F] = 0x7C, // Right Arrow
    [0x50] = 0x7B, // Left Arrow
    [0x51] = 0x7D, // Down Arrow
    [0x52] = 0x7E, // Up Arrow
    [0x53] = 0x47, // Num Lock
    [0x54] = 0x4B, // Numpad /
    [0x55] = 0x43, // Numpad *
    [0x56] = 0x4E, // Numpad -
    [0x57] = 0x45, // Numpad +
    [0x58] = 0x4C, // Numpad Enter
    [0x59] = 0x53, // Numpad 1
    [0x5A] = 0x54, // Numpad 2
    [0x5B] = 0x55, // Numpad 3
    [0x5C] = 0x56, // Numpad 4
    [0x5D] = 0x57, // Numpad 5
    [0x5E] = 0x58, // Numpad 6
    [0x5F] = 0x59, // Numpad 7
    [0x60] = 0x5B, // Numpad 8
    [0x61] = 0x5C, // Numpad 9
    [0x62] = 0x52, // Numpad 0
    [0x63] = 0x41, // Numpad .
    [0x64] = 0x0A, // non-US backslash
    [0x67] = 0x51, // Numpad =
    // Modifiers (HID page 0x07, codes 0xE0-0xE7)
    [0xE0] = 0x3B, // Left Control
    [0xE1] = 0x38, // Left Shift
    [0xE2] = 0x3A, // Left Alt/Option
    [0xE3] = 0x37, // Left Command/GUI
    [0xE4] = 0x3E, // Right Control
    [0xE5] = 0x3C, // Right Shift
    [0xE6] = 0x3D, // Right Alt/Option
    [0xE7] = 0x36, // Right Command/GUI
};

// Forward mapping: macOS CGKeyCode -> HID usage code (page 0x07)
static const uint32_t cg_to_hid[] = {
    [0x00] = 0x04, // a
    [0x01] = 0x16, // s
    [0x02] = 0x07, // d
    [0x03] = 0x09, // f
    [0x04] = 0x0B, // h
    [0x05] = 0x0A, // g
    [0x06] = 0x1D, // z
    [0x07] = 0x1B, // x
    [0x08] = 0x06, // c
    [0x09] = 0x19, // v
    [0x0A] = 0x64, // non-US backslash
    [0x0B] = 0x05, // b
    [0x0C] = 0x14, // q
    [0x0D] = 0x1A, // w
    [0x0E] = 0x08, // e
    [0x0F] = 0x15, // r
    [0x10] = 0x1C, // y
    [0x11] = 0x17, // t
    [0x12] = 0x1E, // 1
    [0x13] = 0x1F, // 2
    [0x14] = 0x20, // 3
    [0x15] = 0x21, // 4
    [0x16] = 0x23, // 6
    [0x17] = 0x22, // 5
    [0x18] = 0x2E, // =
    [0x19] = 0x26, // 9
    [0x1A] = 0x24, // 7
    [0x1B] = 0x2D, // -
    [0x1C] = 0x25, // 8
    [0x1D] = 0x27, // 0
    [0x1E] = 0x30, // ]
    [0x1F] = 0x12, // o
    [0x20] = 0x18, // u
    [0x21] = 0x2F, // [
    [0x22] = 0x0C, // i
    [0x23] = 0x13, // p
    [0x24] = 0x28, // Return
    [0x25] = 0x0F, // l
    [0x26] = 0x0D, // j
    [0x27] = 0x34, // '
    [0x28] = 0x0E, // k
    [0x29] = 0x33, // ;
    [0x2A] = 0x31, // backslash
    [0x2B] = 0x36, // ,
    [0x2C] = 0x38, // /
    [0x2D] = 0x11, // n
    [0x2E] = 0x10, // m
    [0x2F] = 0x37, // .
    [0x30] = 0x2B, // Tab
    [0x31] = 0x2C, // Space
    [0x32] = 0x35, // `
    [0x33] = 0x2A, // Backspace
    [0x35] = 0x29, // Escape
    [0x36] = 0xE7, // Right Command
    [0x37] = 0xE3, // Left Command
    [0x38] = 0xE1, // Left Shift
    [0x39] = 0x39, // Caps Lock
    [0x3A] = 0xE2, // Left Alt/Option
    [0x3B] = 0xE0, // Left Control
    [0x3C] = 0xE5, // Right Shift
    [0x3D] = 0xE6, // Right Alt/Option
    [0x3E] = 0xE4, // Right Control
    [0x3F] = 0x00, // Fn
    [0x41] = 0x63, // Numpad .
    [0x43] = 0x55, // Numpad *
    [0x45] = 0x57, // Numpad +
    [0x47] = 0x53, // Num Lock
    [0x4B] = 0x54, // Numpad /
    [0x4C] = 0x58, // Numpad Enter
    [0x4E] = 0x56, // Numpad -
    [0x51] = 0x67, // Numpad =
    [0x52] = 0x62, // Numpad 0
    [0x53] = 0x59, // Numpad 1
    [0x54] = 0x5A, // Numpad 2
    [0x55] = 0x5B, // Numpad 3
    [0x56] = 0x5C, // Numpad 4
    [0x57] = 0x5D, // Numpad 5
    [0x58] = 0x5E, // Numpad 6
    [0x59] = 0x5F, // Numpad 7
    [0x5B] = 0x60, // Numpad 8
    [0x5C] = 0x61, // Numpad 9
    [0x60] = 0x3E, // F5
    [0x61] = 0x3F, // F6
    [0x62] = 0x40, // F7
    [0x63] = 0x3C, // F3
    [0x64] = 0x41, // F8
    [0x65] = 0x42, // F9
    [0x67] = 0x44, // F11
    [0x69] = 0x46, // F13
    [0x6B] = 0x47, // F14
    [0x6D] = 0x43, // F10
    [0x6F] = 0x45, // F12
    [0x71] = 0x48, // F15
    [0x72] = 0x49, // Insert/Help
    [0x73] = 0x4A, // Home
    [0x74] = 0x4B, // Page Up
    [0x75] = 0x4C, // Delete Forward
    [0x76] = 0x3D, // F4
    [0x77] = 0x4D, // End
    [0x78] = 0x3B, // F2
    [0x79] = 0x4E, // Page Down
    [0x7A] = 0x3A, // F1
    [0x7B] = 0x50, // Left Arrow
    [0x7C] = 0x4F, // Right Arrow
    [0x7D] = 0x51, // Down Arrow
    [0x7E] = 0x52, // Up Arrow
};
static const size_t cg_to_hid_size = sizeof(cg_to_hid) / sizeof(cg_to_hid[0]);

// --- CGEvent output (used when CGEventTap is active) ---

// Track modifier state for CGEvent output so flags are set correctly
static CGEventFlags cgevent_mod_flags = 0;

static CGEventFlags hid_mod_to_cgflag(uint32_t hid_code) {
    switch(hid_code) {
        case 0xE0: case 0xE4: return kCGEventFlagMaskControl;
        case 0xE1: case 0xE5: return kCGEventFlagMaskShift;
        case 0xE2: case 0xE6: return kCGEventFlagMaskAlternate;
        case 0xE3: case 0xE7: return kCGEventFlagMaskCommand;
        default: return 0;
    }
}

int send_key_via_cgevent(struct DKEvent* e) {
    if (e->code >= 256 || e->code == 0) return 1;
    uint16_t cg_keycode = hid_to_cg[e->code];
    bool key_down = (e->value == 1);

    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (!source) return 1;

    // Modifier keys need explicit flag handling
    if (e->code >= 0xE0 && e->code <= 0xE7) {
        CGEventFlags flag = hid_mod_to_cgflag(e->code);
        if (key_down) cgevent_mod_flags |= flag;
        else          cgevent_mod_flags &= ~flag;

        CGEventRef event = CGEventCreateKeyboardEvent(source, (CGKeyCode)cg_keycode, key_down);
        if (!event) { CFRelease(source); return 1; }
        CGEventSetFlags(event, cgevent_mod_flags);
        CGEventSetIntegerValueField(event, kCGEventSourceUserData, KANATA_EVENT_MARKER);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
        CFRelease(source);
        return 0;
    }

    // Regular keys — include current modifier flags
    CGEventRef event = CGEventCreateKeyboardEvent(source, (CGKeyCode)cg_keycode, key_down);
    if (!event) { CFRelease(source); return 1; }
    CGEventSetFlags(event, cgevent_mod_flags);
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, KANATA_EVENT_MARKER);
    CGEventPost(kCGHIDEventTap, event);

    CFRelease(event);
    CFRelease(source);
    return 0;
}

// --- Original Karabiner VirtualHIDDevice output ---

template<typename T>
int send_key(T& keyboard, struct DKEvent* e) {
    if(e->value == 1) keyboard.keys.insert(e->code);
    else if(e->value == 0) keyboard.keys.erase(e->code);
    else return 1;
    #ifdef USE_KEXT
    return pqrs::karabiner_virtual_hid_device_methods::post_keyboard_input_report(connect, keyboard);
    #else
    client->async_post_report(keyboard);
    return 0;
    #endif
}

#ifdef USE_KEXT

int init_sink() {
    kern_return_t kr;
    connect = IO_OBJECT_NULL;
    service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                          IOServiceNameMatching(pqrs::karabiner_virtual_hid_device::get_virtual_hid_root_name()));
    if (!service) {
        print_iokit_error("IOServiceGetMatchingService");
        return 1;
    }
    kr = IOServiceOpen(service, mach_task_self(), kIOHIDServerConnectType, &connect);
    if (kr != KERN_SUCCESS) {
        print_iokit_error("IOServiceOpen", kr);
        return kr;
    }
    {
        pqrs::karabiner_virtual_hid_device::properties::keyboard_initialization properties;
        kr = pqrs::karabiner_virtual_hid_device_methods::initialize_virtual_hid_keyboard(connect, properties);
        if (kr != KERN_SUCCESS) {
            print_iokit_error("initialize_virtual_hid_keyboard", kr);
            return 1;
        }
        while (true) {
            bool ready;
            kr = pqrs::karabiner_virtual_hid_device_methods::is_virtual_hid_keyboard_ready(connect, ready);
            if (kr != KERN_SUCCESS) {
                print_iokit_error("is_virtual_hid_keyboard_ready", kr);
                return kr;
            } else {
                if (ready)
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    {
        pqrs::karabiner_virtual_hid_device::properties::keyboard_initialization properties;
        properties.country_code = 33;
        kr = pqrs::karabiner_virtual_hid_device_methods::initialize_virtual_hid_keyboard(connect, properties);
        if (kr != KERN_SUCCESS) {
            print_iokit_error("initialize_virtual_hid_keyboard", kr);
            return kr;
        }
    }
    return 0;
}

int exit_sink() {
    int retval = 0;
    kern_return_t kr = pqrs::karabiner_virtual_hid_device_methods::reset_virtual_hid_keyboard(connect);
    if (kr != KERN_SUCCESS) {
        print_iokit_error("reset_virtual_hid_keyboard", kr);
        retval = 1;
    }
    if (connect) {
        kr = IOServiceClose(connect);
        if(kr != KERN_SUCCESS) {
            print_iokit_error("IOServiceClose", kr);
            retval = 1;
        }
        connect = IO_OBJECT_NULL;
    }
    if (service) {
        kr = IOObjectRelease(service);
        if(kr != KERN_SUCCESS) {
            print_iokit_error("IOObjectRelease", kr);
            retval = 1;
        }
        service = IO_OBJECT_NULL;
    }
    return retval;
}

#else

int init_sink() {
    try {
        pqrs::dispatcher::extra::initialize_shared_dispatcher();

        client = new pqrs::karabiner::driverkit::virtual_hid_device_service::client();
        auto copy = client;

        client->connected.connect([copy] {
            std::cout << "connected" << std::endl;
            pqrs::karabiner::driverkit::virtual_hid_device_service::virtual_hid_keyboard_parameters parameters;
            parameters.set_country_code(pqrs::hid::country_code::us);
            copy->async_virtual_hid_keyboard_initialize(parameters);
        });

        client->virtual_hid_keyboard_ready.connect([](auto&& ready) {
            std::cout << "virtual_hid_keyboard_ready " << ready << std::endl;
            sink_ready.store(ready, std::memory_order_release);
        });

        client->closed.connect([] {
            std::cout << "closed" << std::endl;
            sink_ready.store(false, std::memory_order_release);
        });

        client->connect_failed.connect([](auto&& error_code) {
            std::cout << "connect_failed " << error_code << std::endl;
            sink_ready.store(false, std::memory_order_release);
        });

        client->error_occurred.connect([](auto&& error_code) {
            std::cout << "error_occurred " << error_code << std::endl;
            sink_ready.store(false, std::memory_order_release);
        });

        client->driver_activated.connect([](auto&& driver_activated) {
            static std::optional<bool> previous_value;
            if (previous_value != driver_activated) {
                std::cout << "driver activated: " << std::boolalpha  << driver_activated << std::endl;
                previous_value = driver_activated;
            }
        });

        client->driver_connected.connect([](auto&& driver_connected) {
            static std::optional<bool> previous_value;
            if (previous_value != driver_connected) {
                std::cout << "driver connected: " << driver_connected << std::endl;
                previous_value = driver_connected;
            }
        });

        client->driver_version_mismatched.connect([](auto&& driver_version_mismatched) {
            static std::optional<bool> previous_value;
            if (previous_value != driver_version_mismatched) {
                std::cout << "driver version matched: " << !driver_version_mismatched << std::endl;
                previous_value = driver_version_mismatched;
            }
        });

        client->async_start();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception in init_sink: " << e.what() << std::endl;
        return 1;
    }
}

int exit_sink() {
    if (client) {
        delete client;
        client = nullptr;
    }
    pqrs::dispatcher::extra::terminate_shared_dispatcher();
    return 0;
}

#endif

void fire_listener_thread() {
    if (!listener_thread.joinable())
        listener_thread = std::thread{
        [&]() {
            listener_loop = CFRunLoopGetCurrent();
            capture_registered_devices();
            CFRunLoopRun();
        } };
}

void input_callback(void* context, IOReturn result, void* sender, IOHIDValueRef value) {
    struct DKEvent e;
    IOHIDElementRef element = IOHIDValueGetElement(value);
    e.value = IOHIDValueGetIntegerValue(value);
    e.page = IOHIDElementGetUsagePage(element);
    e.code = IOHIDElementGetUsage(element);
    write(fd[1], &e, sizeof(struct DKEvent));
}

void device_connected_callback(void* context, io_iterator_t iter) {
    uint64_t device_hash = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context));
    for (mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter)) {
        uint64_t curr_hash = hash_device(curr);
        if ( curr_hash == device_hash )
            capture_device(IOHIDDeviceCreate(kCFAllocatorDefault, curr));
        IOObjectRelease(curr);
    }
}

void close_registered_devices() {
    for(auto hash : registered_devices_hashes) {
        IOHIDDeviceRef device_ref = get_device_by_hash(hash);
        kern_return_t kr = IOHIDDeviceClose(device_ref, kIOHIDOptionsTypeSeizeDevice);
        if(kr != KERN_SUCCESS) { print_iokit_error("IOHIDDeviceClose", kr); return; }
        CFRelease(device_ref);
    }
}

void init_keyboards_dictionary() {
    if( matching_dictionary ) return;
    matching_dictionary      = IOServiceMatching(kIOHIDDeviceKey);
    UInt32 generic_desktop   = kHIDPage_GenericDesktop;
    UInt32 gd_keyboard       = kHIDUsage_GD_Keyboard;
    CFNumberRef page_number  = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &generic_desktop );
    CFNumberRef usage_number = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &gd_keyboard );
    CFDictionarySetValue(matching_dictionary, CFSTR(kIOHIDDeviceUsagePageKey), page_number);
    CFDictionarySetValue(matching_dictionary, CFSTR(kIOHIDDeviceUsageKey),     usage_number);
    release_strings(page_number, usage_number);
}

io_iterator_t get_keyboards_iterator() {
    io_iterator_t iter = IO_OBJECT_NULL;
    CFRetain(matching_dictionary);
    IOServiceGetMatchingServices(kIOMainPortDefault, matching_dictionary, &iter);
    return iter;
}

template <typename Func>
bool consume_devices(Func consume) {
    init_keyboards_dictionary();
    io_iterator_t iter = get_keyboards_iterator();
    if(iter == IO_OBJECT_NULL) return false;
    bool result = false;
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter))
        result = consume(curr) || result;
    IOObjectRelease(iter);
    return result;
}

void subscribe_to_notification(const char* notification_type, void* cb_arg, callback_type callback) {
    io_iterator_t iter = IO_OBJECT_NULL;
    CFRetain(matching_dictionary);
    kern_return_t kr = IOServiceAddMatchingNotification(notification_port, notification_type,
                       matching_dictionary, callback, cb_arg, &iter);
    if (kr != KERN_SUCCESS) { print_iokit_error(notification_type, kr); return; }
    for (io_object_t obj = IOIteratorNext(iter); obj; obj = IOIteratorNext(iter))
        IOObjectRelease(obj);
}

bool capture_device(IOHIDDeviceRef device_ref) {
    std::string dev_name = CFStringToStdString(get_device_name(device_ref));
    kern_return_t kr = IOHIDDeviceOpen(device_ref, kIOHIDOptionsTypeSeizeDevice);
    if(kr != kIOReturnSuccess) {
        std::cerr << "IOHIDDeviceOpen error: " << mach_error_string(kr)
                  << " (0x" << std::hex << kr << std::dec << ") " << dev_name << std::endl;
        return false;
    }
    std::cerr << "Seized device: " << dev_name << std::endl;
    IOHIDDeviceRegisterInputValueCallback(device_ref, input_callback, NULL);
    IOHIDDeviceScheduleWithRunLoop(device_ref, listener_loop, kCFRunLoopDefaultMode);
    return true;
}

// --- CGEventTap fallback for protected devices (macOS Tahoe SPI keyboards) ---

CGEventRef cgeventtap_callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void* refcon) {
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (cgeventtap_ref) CGEventTapEnable(cgeventtap_ref, true);
        return event;
    }

    if (type != kCGEventKeyDown && type != kCGEventKeyUp && type != kCGEventFlagsChanged)
        return event;

    // Pass through our own output events (marked with magic)
    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == KANATA_EVENT_MARKER)
        return event;

    // Drop auto-repeat events — HID layer doesn't generate these,
    // and they break tap-hold timing in kanata
    if (type != kCGEventFlagsChanged &&
        CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat))
        return NULL;

    int64_t keycode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);

    struct DKEvent e;
    e.page = 0x07; // Keyboard/Keypad usage page

    if (type == kCGEventFlagsChanged) {
        if ((size_t)keycode >= cg_to_hid_size || cg_to_hid[keycode] == 0)
            return event;
        e.code = cg_to_hid[keycode];
        CGEventFlags flags = CGEventGetFlags(event);
        bool pressed = false;
        switch(keycode) {
            case 0x39: pressed = (flags & kCGEventFlagMaskAlphaShift) != 0; break;
            case 0x38: case 0x3C: pressed = (flags & kCGEventFlagMaskShift) != 0; break;
            case 0x3B: case 0x3E: pressed = (flags & kCGEventFlagMaskControl) != 0; break;
            case 0x3A: case 0x3D: pressed = (flags & kCGEventFlagMaskAlternate) != 0; break;
            case 0x37: case 0x36: pressed = (flags & kCGEventFlagMaskCommand) != 0; break;
            case 0x3F: pressed = (flags & kCGEventFlagMaskSecondaryFn) != 0; break;
            default: pressed = true; break;
        }
        e.value = pressed ? 1 : 0;
        write(fd[1], &e, sizeof(struct DKEvent));
        return NULL;
    }

    if ((size_t)keycode >= cg_to_hid_size || cg_to_hid[keycode] == 0)
        return event;

    e.code = cg_to_hid[keycode];
    e.value = (type == kCGEventKeyDown) ? 1 : 0;
    write(fd[1], &e, sizeof(struct DKEvent));
    return NULL;
}

bool activate_cgeventtap() {
    if (cgeventtap_active) return true;

    CGEventMask mask = (1 << kCGEventKeyDown) | (1 << kCGEventKeyUp) | (1 << kCGEventFlagsChanged);
    CFMachPortRef tap = CGEventTapCreate(
        kCGHIDEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        mask,
        cgeventtap_callback,
        NULL
    );

    if (!tap) {
        std::cerr << "CGEventTapCreate failed — grant Accessibility permission in System Settings" << std::endl;
        return false;
    }

    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    CFRunLoopAddSource(listener_loop, src, kCFRunLoopDefaultMode);
    cgeventtap_ref = tap;
    CGEventTapEnable(tap, true);
    CFRelease(src);

    cgeventtap_active = true;
    std::cerr << "CGEventTap active — fallback for protected devices" << std::endl;
    return true;
}

bool capture_registered_devices() {
    CFRunLoopAddSource(listener_loop, IONotificationPortGetRunLoopSource(notification_port), kCFRunLoopDefaultMode);
    bool any_failed = false;
    bool result = consume_devices([&any_failed](mach_port_t c) {
        uint64_t device_hash = hash_device(c);
        if ( registered_devices_hashes.find(device_hash) != registered_devices_hashes.end() ) {
            bool captured = capture_device(IOHIDDeviceCreate(kCFAllocatorDefault, c));
            if ( captured ) {
                void* dev_hash = reinterpret_cast<void*>(static_cast<uintptr_t>(device_hash));
                subscribe_to_notification(kIOMatchedNotification, dev_hash, device_connected_callback);
            } else {
                any_failed = true;
            }
            return captured;
        } else return false;
    });

    if (any_failed) {
        std::cerr << "Some devices failed IOHIDDeviceOpen — activating CGEventTap fallback" << std::endl;
        activate_cgeventtap();
        result = true;
    }

    return result;
}

IOHIDDeviceRef get_device_by_hash(uint64_t device_hash) {
    init_keyboards_dictionary();
    io_iterator_t iter = get_keyboards_iterator();
    if(iter == IO_OBJECT_NULL) return nullptr;
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter))
        if ( hash_device(curr) == device_hash )
            return IOHIDDeviceCreate(kCFAllocatorDefault, curr);
    IOObjectRelease(iter);
    return nullptr;
}

uint64_t fnv_hash(const std::string& key) {
    const uint64_t FNV_OFFSET = 14695981039346656037ull;
    const uint64_t FNV_PRIME  = 1099511628211ull;
    uint64_t hash = FNV_OFFSET;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= FNV_PRIME;
    }
    return hash;
}

uint64_t hash_device(mach_port_t device) {
    std::string product_key = CFStringToStdString(get_property(device, kIOHIDProductKey));
    uint32_t vendor_id      = get_number_property(device, kIOHIDVendorIDKey);
    uint32_t product_id     = get_number_property(device, kIOHIDProductIDKey);
    std::string key         = std::to_string(vendor_id) + ":" + std::to_string(product_id) + ":" + product_key;
    return fnv_hash(key);
}

extern "C" {

    bool register_device(const char* product_key) {
        return consume_devices([product_key](mach_port_t current_device) {
            CFStringRef product_key_cfstring = product_key ? from_cstr(product_key) : from_cstr("");
            CFStringRef karabiner            = from_cstr("Karabiner");
            CFStringRef current_product_key  = get_property(current_device, kIOHIDProductKey);
            if(!current_product_key || isSubstring(karabiner, current_product_key) ) {
                release_strings(karabiner, current_product_key, product_key_cfstring);
                return false;
            }
            bool registered = false;
            if(!product_key || (CFStringCompare(current_product_key, product_key_cfstring, 0) == kCFCompareEqualTo)) {
                registered_devices_hashes.insert(hash_device(current_device));
                registered = true;
            }
            release_strings(karabiner, current_product_key, product_key_cfstring);
            return registered;
        });
    }

    void list_keyboards() {
        consume_devices([](mach_port_t c) { std::cout << CFStringToStdString( get_property(c, kIOHIDProductKey) ) << std::endl; return true;});
    }

    void list_keyboards_with_ids() {
        consume_devices([](mach_port_t current_device) {
            std::printf("vendor id: 0x%04X\t product id: 0x%04X\t Product key (name): %s hash: %llu\n",
                        get_number_property(current_device, kIOHIDVendorIDKey),
                        get_number_property(current_device, kIOHIDProductIDKey),
                        CFStringToStdString(get_property(current_device, kIOHIDProductKey)).c_str(),
                        hash_device(current_device));
            return true;
        });
    }

    #ifdef USE_KEXT
    bool driver_activated() {
        return true;
    }
    #else
    bool driver_activated() {
        auto service = IOServiceGetMatchingService(type_safe::get(pqrs::osx::iokit_mach_port::null),
                       IOServiceNameMatching("org_pqrs_Karabiner_DriverKit_VirtualHIDDeviceRoot"));
        if (!service) return false;
        IOObjectRelease(service);
        return true;
    }
    #endif

    int wait_key(struct DKEvent* e) { return read(fd[0], e, sizeof(struct DKEvent)) == sizeof(struct DKEvent); }

    bool device_matches(const char* product) {
        if (!product) return true;
        init_keyboards_dictionary();
        io_iterator_t iter = get_keyboards_iterator();
        CFStringRef device = from_cstr(product);
        for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter)) {
            CFStringRef current_device = get_property(curr, kIOHIDProductKey);
            if( current_device == NULL || CFStringCompare(current_device, device, 0) != kCFCompareEqualTo ) {
                CFRelease(current_device);
                continue;
            } else {
                release_strings(device, current_device);
                return true;
            }
        }
        CFRelease(device);
        IOObjectRelease(iter);
        return false;
    }

    int grab() {
        if (!registered_devices_hashes.size() ) {
            std::cout << "At least one device has to be registered via register_device()" << std::endl;
            return 1;
        }
        if (pipe(fd) == -1) { std::cerr << "pipe error: " << errno << std::endl; return errno; }
        // Connect output before seizing input — ensures we can emit keystrokes
        // before taking exclusive control of the keyboard.
        int sink_err = init_sink();
        if (sink_err) return sink_err;
        fire_listener_thread();
        return 0;
    }

    void release() {
        std::cout << "release called" << std::endl;
        if (cgeventtap_ref) {
            CGEventTapEnable(cgeventtap_ref, false);
            CFRelease(cgeventtap_ref);
            cgeventtap_ref = NULL;
            cgeventtap_active = false;
        }
        if(listener_thread.joinable()) { CFRunLoopStop(listener_loop); listener_thread.join(); }
        close_registered_devices();
        keyboard.keys.clear();
        close(fd[0]); close(fd[1]);
        exit_sink();
    }

    int send_key(struct DKEvent* e) {
        // When CGEventTap is active, use CGEvent output for keyboard events
        // to avoid feedback loop (VirtualHIDDevice output would be re-intercepted by the tap)
        if (cgeventtap_active) {
            #ifdef USE_KEXT
            auto usage_page = pqrs::karabiner_virtual_hid_device::usage_page(e->page);
            if(usage_page == pqrs::karabiner_virtual_hid_device::usage_page::keyboard_or_keypad)
                return send_key_via_cgevent(e);
            #else
            auto usage_page = pqrs::hid::usage_page::value_t(e->page);
            if(usage_page == pqrs::hid::usage_page::keyboard_or_keypad)
                return send_key_via_cgevent(e);
            #endif
        }

        // Original path via Karabiner VirtualHIDDevice
        #ifdef USE_KEXT
        auto usage_page = pqrs::karabiner_virtual_hid_device::usage_page(e->page);
        if(usage_page == pqrs::karabiner_virtual_hid_device::usage_page::keyboard_or_keypad)
            return send_key(keyboard, e);
        else if(usage_page == pqrs::karabiner_virtual_hid_device::usage_page::apple_vendor_top_case)
            return send_key(top_case, e);
        else if(usage_page == pqrs::karabiner_virtual_hid_device::usage_page::apple_vendor_keyboard)
            return send_key(apple_keyboard, e);
        else if(usage_page == pqrs::karabiner_virtual_hid_device::usage_page::consumer)
            return send_key(consumer, e);
        else if(usage_page == pqrs::karabiner_virtual_hid_device::usage_page::generic_desktop)
            return send_key(generic_desktop, e);
        else
            return 1;
        #else
        if(!sink_ready.load(std::memory_order_acquire)) return 2;
        auto usage_page = pqrs::hid::usage_page::value_t(e->page);
        if(usage_page == pqrs::hid::usage_page::keyboard_or_keypad)
            return send_key(keyboard, e);
        else if(usage_page == pqrs::hid::usage_page::apple_vendor_top_case)
            return send_key(top_case, e);
        else if(usage_page == pqrs::hid::usage_page::apple_vendor_keyboard)
            return send_key(apple_keyboard, e);
        else if(usage_page == pqrs::hid::usage_page::consumer)
            return send_key(consumer, e);
        else if(usage_page == pqrs::hid::usage_page::generic_desktop)
            return send_key(generic_desktop, e);
        else return 1;
        #endif
    }

    const DeviceData* get_device_list(size_t* array_length) {
        static std::vector<std::string> products;
        static std::vector<DeviceData>  devices;
        products.clear(); devices.clear();
        consume_devices([](mach_port_t current_device) {
                DeviceData d;
                products.emplace_back(CFStringToStdString(get_property(current_device, kIOHIDProductKey)));
                d.vendor_id   = get_number_property(current_device, kIOHIDVendorIDKey);
                d.product_id  = get_number_property(current_device, kIOHIDProductIDKey);
                devices.push_back({ products.back().c_str(), d.vendor_id, d.product_id });
            return true;
        });
        *array_length = devices.size();
        return devices.data();
    }

    /*
     * Returns true when the DriverKit virtual keyboard is ready for output.
     * On the kext path, always returns true (kext has no async connection).
     */
    bool is_sink_ready() {
        #ifdef USE_KEXT
        return true;
        #else
        return sink_ready.load(std::memory_order_acquire);
        #endif
    }

    /*
     * Releases seized input devices and closes the pipe, but keeps the
     * output (sink) connection alive. This allows the pqrs client to
     * continue its heartbeat and auto-reconnect while the keyboard
     * returns to normal (unseized) operation.
     *
     * After this call, wait_key() on the read end of the pipe will
     * return 0 (EOF), which the caller can use to detect the release.
     */
    void release_input_only() {
        #ifndef USE_KEXT
        if(listener_thread.joinable()) {
            CFRunLoopRemoveSource(listener_loop, IONotificationPortGetRunLoopSource(notification_port), kCFRunLoopDefaultMode);
            CFRunLoopStop(listener_loop);
            listener_thread.join();
        }
        close_registered_devices();
        keyboard.keys.clear();
        close(fd[0]); close(fd[1]);
        #endif
    }

    /*
     * Re-seizes previously registered input devices after a recovery.
     * Requires that register_device() was called before (hashes are retained).
     * Returns true if at least one device was seized.
     */
    bool regrab_input() {
        #ifdef USE_KEXT
        return true;
        #else
        if (!registered_devices_hashes.size()) return false;
        if (pipe(fd) == -1) { std::cerr << "pipe error: " << errno << std::endl; return false; }
        fire_listener_thread();
        return true;
        #endif
    }

}

#ifdef BUILD_AS_BINARY
int main() {
    list_keyboards();
    list_keyboards_with_ids();
    std::cout << "test device_matches:" << std::boolalpha << std::endl <<
                 "device_matches(NULL): " << device_matches(NULL) << std::endl <<
                 "device_matches(appl): " << device_matches("Apple Internal Keyboard / Trackpad") << std::endl <<
                 "device_matches(____): " << device_matches("nano") << std::noboolalpha << std::endl;

    const char* keeb = "Apple Internal Keyboard / Trackpad";
    const char* othr = "DZ60RGB_ANSI";

    register_device(othr);

    for ( uint64_t hash : registered_devices_hashes )
        std::cout << "registered device: " << CFStringToStdString( get_device_name( get_device_by_hash(hash) ) ) <<
                  std::hex << " hash: " << hash << std::dec << " dev: " << get_device_by_hash(hash) << std::endl;

    grab();
    listener_thread.join();
    release();

    return 0;
}
#endif
