#include "driverkit.hpp"

template<typename T>
int send_key(T& keyboard, struct DKEvent* e) {
    if(e->value == 1) keyboard.keys.insert(e->code);
    else if(e->value == 0) keyboard.keys.erase(e->code);
    else return 1;
#if defined(__MAC_10_12) || defined(__MAC_10_13) || defined(__MAC_10_14) || defined(__MAC_10_15)
    return pqrs::karabiner_virtual_hid_device_methods::post_keyboard_input_report(connect, keyboard);
#else
    client->async_post_report(keyboard);
    return 0;
#endif
}

#if defined(__MAC_10_12) || defined(__MAC_10_13) || defined(__MAC_10_14) || defined(__MAC_10_15)
    int init_sink() {
        kern_return_t kr;
        connect = IO_OBJECT_NULL;
        service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceNameMatching(pqrs::karabiner_virtual_hid_device::get_virtual_hid_root_name()));
        if (!service) {
            print_iokit_error("IOServiceGetMatchingService");
            return 1;
        }
        kr = IOServiceOpen(service, mach_task_self(), kIOHIDServerConnectType, &connect);
        if (kr != KERN_SUCCESS) {
            print_iokit_error("IOServiceOpen", kr);
            return kr;
        }
        //std::this_thread::sleep_for(std::chrono::milliseconds(10000));
        //setuid(501);
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
                    if (ready) {
                        break;
                    }
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
#else
    int init_sink() {
        pqrs::dispatcher::extra::initialize_shared_dispatcher();

        client = new pqrs::karabiner::driverkit::virtual_hid_device_service::client();
        auto copy = client;

        client->connected.connect([copy] {
            std::cout << "connected" << std::endl;
            copy->async_virtual_hid_keyboard_initialize(pqrs::hid::country_code::us);
        });

        client->closed.connect([] { std::cout << "closed" << std::endl; });

        client->connect_failed.connect([](auto&& error_code) { std::cout << "connect_failed " << error_code << std::endl; });

        client->error_occurred.connect([](auto&& error_code) { std::cout << "error_occurred " << error_code << std::endl; });

        client->driver_activated.connect([](auto&& driver_activated) {
            static std::optional<bool> previous_value;
            if (previous_value != driver_activated) {
                std::cout << "driver_activated " << driver_activated << std::endl;
                previous_value = driver_activated;
            }
        });

        client->driver_connected.connect([](auto&& driver_connected) {
            static std::optional<bool> previous_value;
            if (previous_value != driver_connected) {
                std::cout << "driver_connected " << driver_connected << std::endl;
                previous_value = driver_connected;
            }
        });

        client->driver_version_mismatched.connect([](auto&& driver_version_mismatched) {
            static std::optional<bool> previous_value;
            if (previous_value != driver_version_mismatched) {
                std::cout << "driver_version_mismatched " << driver_version_mismatched << std::endl;
                previous_value = driver_version_mismatched;
            }
        });

        client->async_start();
        return 0;
    }
#endif

void init_listener() {
    std::lock_guard<std::mutex> lock(mtx);
    listener_loop = CFRunLoopGetCurrent();
    listener_initialized = true;
    cv.notify_one();
}

void monitoring_loop() {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [] { return ready_to_loop; });
    CFRunLoopRun();
}

void fire_thread_once() { if (!thread.joinable()) thread = std::thread{start_monitoring}; }

void block_till_listener_init() {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [] { return listener_initialized; });
}

void notify_start_loop() {
    std::lock_guard<std::mutex> lock(mtx);
    ready_to_loop = true;
    cv.notify_one();
}

#if defined(__MAC_10_12) || defined(__MAC_10_13) || defined(__MAC_10_14) || defined(__MAC_10_15)
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
        }
        if (service) {
            kr = IOObjectRelease(service);
            if(kr != KERN_SUCCESS) {
                print_iokit_error("IOObjectRelease", kr);
                retval = 1;
            }
        }
        return retval;
    }
#else
    void exit_sink() {
        free(client);
        pqrs::dispatcher::extra::terminate_shared_dispatcher();
    }
#endif

void print_iokit_error(const char* fname, int freturn) {
    std::cerr << fname << " error: " << ( freturn ? mach_error_string(freturn) : "" ) << std::endl;
}

void input_callback(void* context, IOReturn result, void* sender, IOHIDValueRef value) {
    struct DKEvent e;
    IOHIDElementRef element = IOHIDValueGetElement(value);
    e.value = IOHIDValueGetIntegerValue(value);
    e.page = IOHIDElementGetUsagePage(element);
    e.code = IOHIDElementGetUsage(element);
    write(fd[1], &e, sizeof(struct DKEvent));
}

void matched_callback(void* context, io_iterator_t iter) {
    // std::cout << "match cb called" << std::endl;
    char* product = (char*)context;
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter))
        open_device_if_match(product, curr);
}

void terminated_callback(void* context, io_iterator_t iter) {
    // std::cout << "term cb called" << std::endl;
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter))
        source_devices.erase(curr);
}

void start_monitoring() {
    init_listener();
    monitoring_loop();
    close_registered_devices();
}

void close_registered_devices() {
    for(std::pair<const io_service_t, IOHIDDeviceRef> p : source_devices) {
        kern_return_t kr = IOHIDDeviceClose(p.second, kIOHIDOptionsTypeSeizeDevice);
        if(kr != KERN_SUCCESS) { print_iokit_error("IOHIDDeviceClose", kr); return; }
    }
}

bool open_device(mach_port_t keeb) {
    IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, keeb);
    source_devices[keeb] = dev;
    IOHIDDeviceRegisterInputValueCallback(dev, input_callback, NULL);
    kern_return_t kr = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeSeizeDevice);
    if(kr != kIOReturnSuccess) {
        print_iokit_error("IOHIDDeviceOpen", kr);
        return false;
    }
    IOHIDDeviceScheduleWithRunLoop(dev, listener_loop, kCFRunLoopDefaultMode);
    return true;
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
bool consume_kb_iter(Func consume) {
    init_keyboards_dictionary();
    io_iterator_t iter = get_keyboards_iterator();
    if(iter == IO_OBJECT_NULL) return false;
    bool result = false;
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter))
        result = result || consume(curr);
    IOObjectRelease(iter);
    return result;
}

CFStringRef from_cstr( const char* str) {
    return CFStringCreateWithCString(kCFAllocatorDefault, str, CFStringGetSystemEncoding());
}

CFStringRef get_property(mach_port_t item, const char* property) {
    return (CFStringRef) IORegistryEntryCreateCFProperty(item, from_cstr(property), kCFAllocatorDefault, kIOHIDOptionsTypeNone);
}

template<typename... Args>
void release_strings(Args... strings) {
    (CFRelease(strings), ...);
}

bool isSubstring(CFStringRef subString, CFStringRef mainString) {
    return CFStringFind(mainString, subString, kCFCompareCaseInsensitive).location != kCFNotFound;
}

/*  * device is karabiner => return, don't open it no matter what
    * product is null     => open the device
    * product specified   => open the device if it matches product (device key requested) */
bool open_device_if_match(const char* product, mach_port_t device) {
    CFStringRef product_key = product ? from_cstr(product) : from_cstr("");
    CFStringRef karabiner   = from_cstr("Karabiner"); //Karabiner DriverKit VirtualHIDKeyboard 1.7.0
    CFStringRef device_key  = get_property(device, kIOHIDProductKey);

    if( !device_key || isSubstring(karabiner, device_key) ) {
        release_strings(karabiner, device_key, product_key);
        return false;
    }

    bool opened = false;
    if( !product || (CFStringCompare(device_key, product_key, 0) == kCFCompareEqualTo))
        opened = open_device(device);

    release_strings(karabiner, device_key, product_key);
    return opened;
}

using callback_type = void(*)(void*, io_iterator_t);
void subscribe_to_notification(const char* notification_type, void* cb_arg, callback_type callback) {
    io_iterator_t iter = IO_OBJECT_NULL;
    CFRetain(matching_dictionary);
    kern_return_t kr = IOServiceAddMatchingNotification(notification_port, notification_type,
                       matching_dictionary, callback, cb_arg, &iter);
    if (kr != KERN_SUCCESS) { print_iokit_error(notification_type, kr); return; }
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter)) {} // mystery, doesn't work without this line!!!
}

bool register_device(char* product) {
    fire_thread_once();
    block_till_listener_init();
    bool opened = consume_kb_iter([product](mach_port_t c) { return open_device_if_match(product, c); });
    CFRunLoopAddSource(listener_loop, IONotificationPortGetRunLoopSource(notification_port), kCFRunLoopDefaultMode);
    subscribe_to_notification(kIOMatchedNotification, product, matched_callback);
    subscribe_to_notification(kIOTerminatedNotification, NULL, terminated_callback);
    return opened;
}

std::string CFStringToStdString(CFStringRef cfString) {
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

    void list_keyboards() { // CFStringGetCStringPtr(cfString, kCFStringEncodingUTF8)
        consume_kb_iter([](mach_port_t c) { std::cout << CFStringToStdString( get_property(c, kIOHIDProductKey) ) << std::endl; return true;});
    }

    bool driver_activated() {
        auto service = IOServiceGetMatchingService(type_safe::get(pqrs::osx::iokit_mach_port::null),
                       IOServiceNameMatching("org_pqrs_Karabiner_DriverKit_VirtualHIDDeviceRoot"));
        if (!service) return false;
        IOObjectRelease(service);
        return true;
    }

    // Reads a new key event from the pipe, blocking until a new event is ready.
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

    /*
     * Opens and seizes input from each keyboard device whose product name
     * matches the parameter (if NULL is received, then it opens all
     * keyboard devices). Spawns a thread to receive asynchronous input
     * and opens a pipe for this thread to send key event data to the main
     * thread.
     *
     * Loads a the karabiner kernel extension that will send key events
     * back to the OS.
     */
    int grab() {
        if (!thread.joinable()) {
            std::cout << "At least one device has to be registered via register_device()" << std::endl;
            return 1;
        }
        if (pipe(fd) == -1) { std::cerr << "pipe error: " << errno << std::endl; return errno; }
        notify_start_loop();
        return init_sink();
    }

    /*
     * Releases the resources needed to receive key events from and send
     * key events to the OS.
     */
    void release() {
        if(thread.joinable()) { CFRunLoopStop(listener_loop); thread.join(); }
        keyboard.keys.clear();
        close(fd[0]); close(fd[1]);
        exit_sink();
    }

    /*
     * Rust calls this with a new key event to send back to the OS. It
     * posts the information to the karabiner kernel extension (which
     * represents a virtual keyboard).
     */
    int send_key(struct DKEvent* e) {
#if defined(__MAC_10_12) || defined(__MAC_10_13) || defined(__MAC_10_14) || defined(__MAC_10_15)
        auto usage_page = pqrs::karabiner_virtual_hid_device::usage_page(e->page);
        if(usage_page == pqrs::karabiner_virtual_hid_device::usage_page::keyboard_or_keypad)
            return send_key(keyboard, e);
        else if(usage_page == pqrs::karabiner_virtual_hid_device::usage_page::apple_vendor_top_case)
            return send_key(top_case, e);
        else if(usage_page == pqrs::karabiner_virtual_hid_device::usage_page::apple_vendor_keyboard)
            return send_key(apple_keyboard, e);
        else if(usage_page == pqrs::karabiner_virtual_hid_device::usage_page::consumer)
            return send_key(consumer, e);
        else
            return 1;
#else
        auto usage_page = pqrs::hid::usage_page::value_t(e->page);
        if(usage_page == pqrs::hid::usage_page::keyboard_or_keypad)
            return send_key(keyboard, e);
        else if(usage_page == pqrs::hid::usage_page::apple_vendor_top_case)
            return send_key(top_case, e);
        else if(usage_page == pqrs::hid::usage_page::apple_vendor_keyboard)
            return send_key(apple_keyboard, e);
        else if(usage_page == pqrs::hid::usage_page::consumer)
            return send_key(consumer, e);
        else return 1;
#endif
    }
}

// main function is just for testing
// build as binary command:
// g++ c_src/driverkit.cpp -Ic_src/Karabiner-DriverKit-VirtualHIDDevice/include/pqrs/karabiner/driverkit -Ic_src/Karabiner-DriverKit-VirtualHIDDevice/src/Client/vendor/include -std=c++2a -framework IOKit -framework CoreFoundation -o driverkit
#ifdef BUILD_AS_BINARY
int main() {
    list_keyboards();
    std::cout << "test device_matches:" << std::boolalpha << std::endl <<
              "device_matches(NULL): " << device_matches(NULL) << std::endl <<
              "device_matches(appl): " << device_matches("Apple Internal Keyboard / Trackpad") << std::endl <<
              "device_matches(____): " << device_matches("nano") << std::noboolalpha << std::endl;
    // std::cout << "test register_device:" << std::boolalpha << std::endl <<
    //     "register_device(NULL): " << register_device(NULL) << std::endl <<
    //     "register_device(appl): " << register_device("Apple Internal Keyboard / Trackpad") << std::endl <<
    //     "register_device(____): " << register_device("nano") << std::noboolalpha << std::endl;
    // const char* keeb = "Apple Internal Keyboard / Trackpad";
    register_device("kbd67mkiirgb v3");
    //register_device(NULL);
    //register_device(keeb);
    notify_start_loop();
    thread.join();
}
#endif
