#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <unistd.h>
#include <errno.h>
#include <thread>
#include <map>
#include <iostream>
#include <mach/mach_error.h>
#include <AvailabilityMacros.h>
#include <filesystem> // Include this before virtual_hid_device_service.hpp to avoid compile error
#include "virtual_hid_device_driver.hpp"
#include "virtual_hid_device_service.hpp"

/* The name was changed from "Master" to "Main" in Apple SDK 12.0 (Monterey) */
#if (MAC_OS_X_VERSION_MIN_REQUIRED < 120000) // Before macOS 12 Monterey
    #define kIOMainPortDefault kIOMasterPortDefault
#endif

static pqrs::karabiner::driverkit::virtual_hid_device_service::client* client;
static pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::keyboard_input keyboard;
static pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::apple_vendor_top_case_input top_case;
static pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::apple_vendor_keyboard_input apple_keyboard;
static pqrs::karabiner::driverkit::virtual_hid_device_driver::hid_report::consumer_input consumer;

static std::thread thread;
static CFRunLoopRef listener_loop;
static std::map<io_service_t, IOHIDDeviceRef> source_devices;
static int fd[2];
static char* prod = nullptr;
static CFMutableDictionaryRef matching_dictionary = NULL;

/*
 * Key event information that's shared between C++ and Rust
 * value: represents key up or key down
 * page: represents IOKit usage page
 * code: represents IOKit usage
 */
struct DKEvent
{
    uint64_t value;
    uint32_t page;
    uint32_t code;
};

int init_sink(void);
int exit_sink(void);
int start_loop(char* product);
void print_iokit_error(const char* fname, int freturn = 0);
void input_callback(void* context, IOReturn result, void* sender, IOHIDValueRef value);
void matched_callback(void* context, io_iterator_t iter);
void terminated_callback(void* context, io_iterator_t iter);
void monitor_kb(char* product);

void close_registered_devices();
void open_device(mach_port_t keeb);
void init_keyboards_dictionary();
io_iterator_t get_keyboards_iterator();
std::string CFStringToStdString(CFStringRef cfString);

template <typename Func>
int get_consume_kb_iter(Func consume);
CFStringRef from_cstr( const char* str);
CFStringRef get_property(mach_port_t item, const char* property);

extern "C" {
    int grab_kb(char* product);
    int send_key(struct DKEvent* e);
    int wait_key(struct DKEvent* e);
    int release_kb();

    void register_device(char* product);
    void list_keyboards();
    bool device_matches(const char* product);
    bool driver_activated(void);
    void open_matching_devices(char* product, io_iterator_t iter);
}
