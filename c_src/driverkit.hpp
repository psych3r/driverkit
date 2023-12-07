#include <map>
#include <unistd.h>
#include <errno.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <mach/mach_error.h>
#include <AvailabilityMacros.h>
#include <filesystem> // Include this before virtual_hid_device_service.hpp to avoid compile error
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include "virtual_hid_device_driver.hpp"
#include "virtual_hid_device_service.hpp"

/* The name was changed from "Master" to "Main" in Apple SDK 12.0 (Monterey) */
#if (MAC_OS_X_VERSION_MIN_REQUIRED < 120000) // Before macOS 12 Monterey
    #define kIOMainPortDefault kIOMasterPortDefault
#endif

pqrs::karabiner::driverkit::virtual_hid_device_service::client* client;
pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::keyboard_input keyboard;
pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::apple_vendor_top_case_input top_case;
pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::apple_vendor_keyboard_input apple_keyboard;
pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::consumer_input consumer;

IONotificationPortRef notification_port = IONotificationPortCreate(kIOMainPortDefault);
std::thread thread;
CFRunLoopRef listener_loop;
std::map<io_service_t, IOHIDDeviceRef> source_devices;
int fd[2];
CFMutableDictionaryRef matching_dictionary = NULL;
std::mutex mtx;
std::condition_variable cv;
bool listener_initialized = false;
bool ready_to_loop = false;

using callback_type = void(*)(void*, io_iterator_t);

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

void init_listener();
void monitoring_loop();
void start_monitoring();
void fire_thread_once();
void block_till_listener_init();
void close_registered_devices();
void notify_start_loop();
int  init_sink();
void exit_sink();

void print_iokit_error(const char* fname, int freturn = 0);
void input_callback(void* context, IOReturn result, void* sender, IOHIDValueRef value);
void matched_callback(void* context, io_iterator_t iter);
void terminated_callback(void* context, io_iterator_t iter);
void subscribe_to_notification(const char* notification_type, void* cb_arg, callback_type callback);

bool open_device_if_match(const char* product, mach_port_t device);
bool open_device(mach_port_t keeb);

void init_keyboards_dictionary();
io_iterator_t get_keyboards_iterator();
std::string CFStringToStdString(CFStringRef cfString);
template <typename Func>
bool consume_kb_iter(Func consume);
template<typename... Args>
void release_strings(Args... strings);
CFStringRef from_cstr( const char* str);
CFStringRef get_property(mach_port_t item, const char* property);
bool isSubstring(CFStringRef subString, CFStringRef mainString);

extern "C" {
    int grab();
    int send_key(struct DKEvent* e);
    int wait_key(struct DKEvent* e);
    void release();

    void list_keyboards();
    bool device_matches(const char* product);
    bool driver_activated();
    bool register_device(char* product);
}
