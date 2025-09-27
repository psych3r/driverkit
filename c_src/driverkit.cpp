#include "driverkit.hpp"
#include <exception>

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

        client->closed.connect([] { std::cout << "closed" << std::endl; });

        client->connect_failed.connect([](auto&& error_code) { std::cout << "connect_failed " << error_code << std::endl; });

        client->error_occurred.connect([](auto&& error_code) { std::cout << "error_occurred " << error_code << std::endl; });

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
    kern_return_t kr = IOHIDDeviceOpen(device_ref, kIOHIDOptionsTypeSeizeDevice);
    if(kr != kIOReturnSuccess) {
        print_iokit_error("IOHIDDeviceOpen", kr, CFStringToStdString(get_device_name(device_ref)));
        return false;
    }
    IOHIDDeviceRegisterInputValueCallback(device_ref, input_callback, NULL);
    IOHIDDeviceScheduleWithRunLoop(device_ref, listener_loop, kCFRunLoopDefaultMode);
    return true;
}

bool capture_registered_devices() {
    // Register the notification port to the run loop, essential for receiving re-connect events so we can re-capture devices
    CFRunLoopAddSource(listener_loop, IONotificationPortGetRunLoopSource(notification_port), kCFRunLoopDefaultMode);
    return consume_devices([](mach_port_t c) {
        uint64_t device_hash = hash_device(c);
        if ( registered_devices_hashes.find(device_hash) != registered_devices_hashes.end() ) {
            bool captured = capture_device(IOHIDDeviceCreate(kCFAllocatorDefault, c));
            if ( captured ) {
                void* dev_hash = reinterpret_cast<void*>(static_cast<uintptr_t>(device_hash));
                subscribe_to_notification(kIOMatchedNotification, dev_hash, device_connected_callback);
            }
            return captured;
        } else return false;
    });
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

    /*
     * current device is karabiner => return, shouldn't be registered nor captured, avoid at all costs!!!
     * product_kye is null         => register all devices
     * product_key specified       => register the device that matches product_key  */
    bool register_device(const char* product_key) {
        return consume_devices([product_key](mach_port_t current_device) {
            CFStringRef product_key_cfstring = product_key ? from_cstr(product_key) : from_cstr("");
            CFStringRef karabiner            = from_cstr("Karabiner"); //Karabiner DriverKit VirtualHIDKeyboard 1.7.0
            CFStringRef current_product_key  = get_property(current_device, kIOHIDProductKey);
            // Don't open karabiner devices or devices without a name
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

    void list_keyboards() { // CFStringGetCStringPtr(cfString, kCFStringEncodingUTF8)
        consume_devices([](mach_port_t c) { std::cout << CFStringToStdString( get_property(c, kIOHIDProductKey) ) << std::endl; return true;});
    }

    void list_keyboards_with_ids() {
        consume_devices([](mach_port_t current_device) {
            // TODO: filter out duplicates (same vendor_id, product_id, name)
            // Also, print as decimal instad of hex?
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
        // FIXME: should we have anything here?
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
        if (!registered_devices_hashes.size() ) {
            std::cout << "At least one device has to be registered via register_device()" << std::endl;
            return 1;
        }
        if (pipe(fd) == -1) { std::cerr << "pipe error: " << errno << std::endl; return errno; }
        fire_listener_thread();
        return init_sink();
    }

    /*
     * Releases the resources needed to receive key events from and send
     * key events to the OS.
     */
    void release() {
        std::cout << "release called" << std::endl;
        if(listener_thread.joinable()) { CFRunLoopStop(listener_loop); listener_thread.join(); }
        close_registered_devices();
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
        static std::vector<std::string> products;   // to own the strings
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

}

// main function is just for testing
// build as binary command:
// g++ c_src/driverkit.cpp -DBUILD_AS_BINARY -Ic_src/Karabiner-DriverKit-VirtualHIDDevice/include/pqrs/karabiner/driverkit -Ic_src/Karabiner-DriverKit-VirtualHIDDevice/src/Client/vendor/include -Ic_src/Karabiner-DriverKit-VirtualHIDDevice/vendor/vendor/include -std=c++2a -framework IOKit -framework CoreFoundation -o driverkit -g -O0
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


    // register_device(keeb);
    // register_device(nullptr);
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
