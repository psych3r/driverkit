#include "driverkit.hpp"

template<typename T>
int send_key(T& keyboard, struct DKEvent* e) {
    if(e->value == 1) keyboard.keys.insert(e->code);
    else if(e->value == 0) keyboard.keys.erase(e->code);
    else return 1;
    client->async_post_report(keyboard);
    return 0;
}

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

void exit_sink() {
    free(client);
    pqrs::dispatcher::extra::terminate_shared_dispatcher();
}

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

void open_device(mach_port_t keeb) {
    IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, keeb);
    source_devices[keeb] = dev;
    IOHIDDeviceRegisterInputValueCallback(dev, input_callback, NULL);
    kern_return_t kr = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeSeizeDevice);
    if(kr != kIOReturnSuccess) print_iokit_error("IOHIDDeviceOpen", kr);
    IOHIDDeviceScheduleWithRunLoop(dev, listener_loop, kCFRunLoopDefaultMode);
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
int consume_kb_iter(Func consume) {
    init_keyboards_dictionary();
    io_iterator_t iter = get_keyboards_iterator();
    if(iter == IO_OBJECT_NULL) return 1;
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter))
        consume(curr);
    IOObjectRelease(iter);
    return 0;
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

/*  * device is karabiner => return, don't open it no matter what
    * product is null     => open the device
    * product specified   => open the device if it matches product (device key requested) */
void open_device_if_match(const char* product, mach_port_t device) {
    CFStringRef product_key = product ? from_cstr(product) : from_cstr("");
    CFStringRef karabiner   = from_cstr("Karabiner VirtualHIDKeyboard");
    CFStringRef device_key  = get_property(device, kIOHIDProductKey);

    if( !device_key || CFStringCompare(device_key, karabiner, 0) == kCFCompareEqualTo )
        return release_strings(karabiner, device_key, product_key);

    if( !product || (CFStringCompare(device_key, product_key, 0) == kCFCompareEqualTo))
        open_device(device);

    release_strings(karabiner, device_key, product_key);
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

void register_device(char* product) {
    fire_thread_once();
    block_till_listener_init();
    consume_kb_iter([product](mach_port_t c) { open_device_if_match(product, c); });
    CFRunLoopAddSource(listener_loop, IONotificationPortGetRunLoopSource(notification_port), kCFRunLoopDefaultMode);
    subscribe_to_notification(kIOMatchedNotification, product, matched_callback);
    subscribe_to_notification(kIOTerminatedNotification, NULL, terminated_callback);
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
        consume_kb_iter([](mach_port_t c) { std::cout << CFStringToStdString( get_property(c, kIOHIDProductKey) ) << std::endl; });
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
        close(fd[0]); close(fd[1]);
        exit_sink();
    }

    /*
     * Rust calls this with a new key event to send back to the OS. It
     * posts the information to the karabiner kernel extension (which
     * represents a virtual keyboard).
     */
    int send_key(struct DKEvent* e) {
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
    }
}

// main function is just for testing
// build as binary command:
// g++ c_src/driverkit.cpp -Ic_src/Karabiner-DriverKit-VirtualHIDDevice/include/pqrs/karabiner/driverkit -Ic_src/Karabiner-DriverKit-VirtualHIDDevice/src/Client/vendor/include -std=c++2a -framework IOKit -framework CoreFoundation -o driverkit
int main() {
    list_keyboards();
    std::cout << device_matches(NULL) << " " << device_matches("Apple Internal Keyboard / Trackpad") <<
              " " << device_matches("nano") << std::endl;
    const char* keeb = "Apple Internal Keyboard / Trackpad";
    register_device("kbd67mkiirgb v3");
    //register_device(NULL);
    //register_device(keeb);
    notify_start_loop();
    thread.join();
}
