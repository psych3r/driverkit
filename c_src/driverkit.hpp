#include <unistd.h>
#include <thread>
#include <iostream>
#include <mach/mach_error.h>
#include <AvailabilityMacros.h>
#include <filesystem> // Include this before virtual_hid_device_service.hpp to avoid compile error
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <set>

/* The name was changed from "Master" to "Main" in Apple SDK 12.0 (Monterey) */
#if (MAC_OS_X_VERSION_MIN_REQUIRED < 120000) // Before macOS 12 Monterey
    #define kIOMainPortDefault kIOMasterPortDefault
#endif

#ifdef USE_KEXT
    #include "karabiner_virtual_hid_device_methods.hpp"
    mach_port_t connect;
    io_service_t service;
    pqrs::karabiner_virtual_hid_device::hid_report::keyboard_input keyboard;
    pqrs::karabiner_virtual_hid_device::hid_report::apple_vendor_top_case_input top_case;
    pqrs::karabiner_virtual_hid_device::hid_report::apple_vendor_keyboard_input apple_keyboard;
    pqrs::karabiner_virtual_hid_device::hid_report::consumer_input consumer;
    pqrs::karabiner_virtual_hid_device::hid_report::generic_desktop_input generic_desktop;
#else
    #include "virtual_hid_device_driver.hpp"
    #include "virtual_hid_device_service.hpp"
    pqrs::karabiner::driverkit::virtual_hid_device_service::client* client;
    pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::keyboard_input keyboard;
    pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::apple_vendor_top_case_input top_case;
    pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::apple_vendor_keyboard_input apple_keyboard;
    pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::consumer_input consumer;
    pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::generic_desktop_input generic_desktop;
#endif

IONotificationPortRef notification_port = IONotificationPortCreate(kIOMainPortDefault);
std::thread listener_thread;
CFRunLoopRef listener_loop;
std::set<uint64_t> registered_devices_hashes;

int fd[2];
CFMutableDictionaryRef matching_dictionary = NULL;

/*
 * Key event information that's shared between C++ and Rust
 * value: represents key up or key down
 * page: represents IOKit usage page
 * code: represents IOKit usage
 */
struct DKEvent {
    uint64_t value;
    uint32_t page;
    uint32_t code;
};

/*
 * Device data
 * product_key: device name IOKit (kIOHIDProductKey)
 * vendor_id:   IOKit (kIOHIDVendorIDKey)
 * product_id:  IOKit (kIOHIDProductIDKey)
 */
struct DeviceData {
    const char* product_key;
    uint32_t vendor_id;
    uint32_t product_id;
};

using callback_type = void(*)(void*, io_iterator_t);
void subscribe_to_notification(const char* notification_type, void* cb_arg, callback_type callback);
void device_connected_callback(void* context, io_iterator_t iter);
void fire_listener_thread();
void init_keyboards_dictionary();
void close_registered_devices();
void input_callback(void* context, IOReturn result, void* sender, IOHIDValueRef value);

template <typename Func>
bool consume_devices(Func consume);
bool capture_registered_devices();
bool capture_device(IOHIDDeviceRef device_ref);

int  init_sink();
int  exit_sink();
uint64_t hash_device(mach_port_t device);

// // to be used if we want to move notofication subscribing inside capture_device()
// // will allow converting IOHIDDeviceRef to a hash
// uint64_t get_hash_from_device(IOHIDDeviceRef device) {
//     std::string product_key = CFStringToStdString((CFStringRef) IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey)));
//     uint32_t vendor_id, product_id = 0;
//     CFNumberGetValue((CFNumberRef)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey)),  kCFNumberSInt32Type, &vendor_id);
//     CFNumberGetValue((CFNumberRef)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey)), kCFNumberSInt32Type, &product_id);
//     std::string key         = std::to_string(vendor_id) + ":" + std::to_string(product_id) + ":" + product_key;
//     return std::hash<std::string> {}(key);
// }

io_iterator_t get_keyboards_iterator();
IOHIDDeviceRef get_device_by_hash(uint64_t device_hash);

// Helper functions...
inline void print_iokit_error(const char* fname, int freturn, std::string data = "") {
    std::cerr << fname << " error: " << ( freturn ? mach_error_string(freturn) : "" ) << " " << data << std::endl;
}

inline CFStringRef from_cstr( const char* str) {
    if (!str) return nullptr;
    return CFStringCreateWithCString(kCFAllocatorDefault, str, CFStringGetSystemEncoding());
}

inline CFStringRef get_property(mach_port_t item, const char* property) {
    return (CFStringRef) IORegistryEntryCreateCFProperty(item, from_cstr(property), kCFAllocatorDefault, kIOHIDOptionsTypeNone);
}

inline uint32_t get_number_property(mach_port_t item, const char* property) {
    uint32_t value = 0;
    if (CFTypeRef ref = IORegistryEntryCreateCFProperty(item, from_cstr(property), kCFAllocatorDefault, kIOHIDOptionsTypeNone)) {
        if (CFGetTypeID(ref) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt32Type, &value);
        CFRelease(ref);
    }
    return value;
}

inline CFStringRef get_device_name(IOHIDDeviceRef device) {
    return (CFStringRef) IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
}

template<typename... Args>
inline void release_strings(Args... strings) {
    ((strings ? CFRelease(strings) : void()), ...);
}

inline bool isSubstring(CFStringRef subString, CFStringRef mainString) {
    if (!subString || !mainString) return false;
    return CFStringFind(mainString, subString, kCFCompareCaseInsensitive).location != kCFNotFound;
}

inline std::string CFStringToStdString(CFStringRef cfString) {
    if (cfString == nullptr)  return std::string();
    CFIndex length  = CFStringGetLength(cfString);
    CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string utf8String(maxSize, '\0');
    if (CFStringGetCString(cfString, &utf8String[0], maxSize, kCFStringEncodingUTF8)) {
        utf8String.resize(strlen( utf8String.c_str()));
        return utf8String;
    }
    return std::string();
}

extern "C" {
    int grab();
    int send_key(struct DKEvent* e);
    int wait_key(struct DKEvent* e);
    void release();

    void list_keyboards();
    void list_keyboards_with_ids();
    bool device_matches(const char* product);
    bool driver_activated();
    bool register_device(const char* product_key);
    bool register_device_hash(uint64_t device_hash) {
        return consume_devices([device_hash](mach_port_t current_device) {
            // Don't open karabiner
            if( isSubstring(from_cstr("Karabiner"), get_property(current_device, kIOHIDProductKey) ) )
                return false;
            if ( hash_device(current_device) == device_hash ) {
                registered_devices_hashes.insert(device_hash);
                return true;
            }
            return false;
        });
    }
    const DeviceData* get_device_list(size_t* array_length);
}
