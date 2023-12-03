#include "driverkit.hpp"


int exit_sink()
{
    free(client);
    pqrs::dispatcher::extra::terminate_shared_dispatcher();
    return 0;
}

/*
 * This gets us some code reuse (see the send_key overload below)
 */
template<typename T>
int send_key(T& keyboard, struct DKEvent* e)
{
    if(e->value == 1) keyboard.keys.insert(e->code);
    else if(e->value == 0) keyboard.keys.erase(e->code);
    else return 1;
    client->async_post_report(keyboard);
    return 0;
}

int init_sink()
{
    pqrs::dispatcher::extra::initialize_shared_dispatcher();
    client = new pqrs::karabiner::driverkit::virtual_hid_device_service::client();
    auto copy = client;

    client->connected.connect([copy]
    {
        std::cout << "connected" << std::endl;
        copy->async_virtual_hid_keyboard_initialize(pqrs::hid::country_code::us);
    });

    client->connect_failed.connect([](auto&& error_code)
    {
        std::cout << "connect_failed " << error_code << std::endl;
    });

    client->closed.connect([] { std::cout << "closed" << std::endl; });

    client->error_occurred.connect([](auto&& error_code)
    {
        std::cout << "error_occurred " << error_code << std::endl;
    });

    client->driver_activated.connect([](auto&& driver_activated)
    {
        static std::optional<bool> previous_value;
        if (previous_value != driver_activated)
        {
            std::cout << "driver_activated " << driver_activated << std::endl;
            previous_value = driver_activated;
        }
    });

    client->driver_connected.connect([](auto&& driver_connected)
    {
        static std::optional<bool> previous_value;
        if (previous_value != driver_connected)
        {
            std::cout << "driver_connected " << driver_connected << std::endl;
            previous_value = driver_connected;
        }
    });

    client->driver_version_mismatched.connect([](auto&& driver_version_mismatched)
    {
        static std::optional<bool> previous_value;
        if (previous_value != driver_version_mismatched)
        {
            std::cout << "driver_version_mismatched " << driver_version_mismatched << std::endl;
            previous_value = driver_version_mismatched;
        }
    });

    client->async_start();
    return 0;
}

void print_iokit_error(const char* fname, int freturn)
{
    std::cerr << fname << " error";
    if(freturn)
    {
        //std::cerr << " " << std::hex << freturn;
        std::cerr << ": ";
        std::cerr << mach_error_string(freturn);
    }
    std::cerr << std::endl;
}

void input_callback(void* context, IOReturn result, void* sender, IOHIDValueRef value)
{
    struct DKEvent e;
    IOHIDElementRef element = IOHIDValueGetElement(value);
    e.value = IOHIDValueGetIntegerValue(value);
    e.page = IOHIDElementGetUsagePage(element);
    e.code = IOHIDElementGetUsage(element);
    write(fd[1], &e, sizeof(struct DKEvent));
}

void matched_callback(void* context, io_iterator_t iter)
{
    char* product = (char*)context;
    open_matching_devices(product, iter);
}

/*
 * We'll register this callback to run whenever an IOHIDDevice
 * (representing a keyboard) is disconnected from the OS
 */
void terminated_callback(void* context, io_iterator_t iter)
{
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter))
        source_devices.erase(curr);
}


int start_loop(char* product) {
    if (source_devices.empty()) return 1; // no registered devices!
    CFRunLoopRun();
    close_registered_devices();
    return 0;
}

void close_registered_devices()
{
    for(std::pair<const io_service_t, IOHIDDeviceRef> p : source_devices)
    {
        kern_return_t kr = IOHIDDeviceClose(p.second, kIOHIDOptionsTypeSeizeDevice);
        if(kr != KERN_SUCCESS)
        {
            print_iokit_error("IOServiceAddMatchingNotification", kr);
            return;
        }
    }
}

void open_device(mach_port_t keeb)
{
    IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, keeb);
    source_devices[keeb] = dev;
    IOHIDDeviceRegisterInputValueCallback(dev, input_callback, NULL);
    kern_return_t kr = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeSeizeDevice);
    if(kr != kIOReturnSuccess)
        print_iokit_error("IOHIDDeviceOpen", kr);
    IOHIDDeviceScheduleWithRunLoop(dev, listener_loop, kCFRunLoopDefaultMode);
}

void init_keyboards_dictionary()
{
    if( matching_dictionary ) return;
    matching_dictionary      = IOServiceMatching(kIOHIDDeviceKey);
    UInt32 generic_desktop   = kHIDPage_GenericDesktop;
    UInt32 gd_keyboard       = kHIDUsage_GD_Keyboard;
    CFNumberRef page_number  = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &generic_desktop );
    CFNumberRef usage_number = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &gd_keyboard );
    CFDictionarySetValue(matching_dictionary, CFSTR(kIOHIDDeviceUsagePageKey), page_number);
    CFDictionarySetValue(matching_dictionary, CFSTR(kIOHIDDeviceUsageKey),     usage_number);
    CFRelease(page_number);
    CFRelease(usage_number);
}

io_iterator_t get_keyboards_iterator()
{
    io_iterator_t iter = IO_OBJECT_NULL;
    CFRetain(matching_dictionary);
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching_dictionary, &iter);
    return iter;
}

template <typename Func>
int get_consume_kb_iter(Func consume)
{
    init_keyboards_dictionary();
    io_iterator_t iter = get_keyboards_iterator();
    if(iter == IO_OBJECT_NULL) return 1;
    for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter))
        consume(curr);
    return 0;
}

std::string CFStringToStdString(CFStringRef cfString)
{
    if (cfString == nullptr)  return std::string();
    CFIndex length = CFStringGetLength(cfString);
    CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string utf8String(maxSize, '\0');
    if (CFStringGetCString(cfString, &utf8String[0], maxSize, kCFStringEncodingUTF8))
    {
        utf8String.resize(strlen( utf8String.c_str()));
        return utf8String;
    }
    return std::string();
}

CFStringRef from_cstr( const char* str)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, str, CFStringGetSystemEncoding());
}

CFStringRef get_property(mach_port_t item, const char* property)
{
    return (CFStringRef) IORegistryEntryCreateCFProperty(item, from_cstr(property), kCFAllocatorDefault, kIOHIDOptionsTypeNone);
}

extern "C" {

    void list_keyboards()
    {
        get_consume_kb_iter([](mach_port_t c) { std::cout << CFStringToStdString( get_property(c, kIOHIDProductKey) ) << std::endl; });
    }

    bool driver_activated(void)
    {
        std::string service_name("org_pqrs_Karabiner_DriverKit_VirtualHIDDeviceRoot");
        auto service = IOServiceGetMatchingService(type_safe::get(pqrs::osx::iokit_mach_port::null),
                IOServiceNameMatching(service_name.c_str()));
        if (!service) return false;

        IOObjectRelease(service);
        return true;
    }

    // Reads a new key event from the pipe, blocking until a new event is ready.
    int wait_key(struct DKEvent* e)
    {
        return read(fd[0], e, sizeof(struct DKEvent)) == sizeof(struct DKEvent);
    }

    bool device_matches(const char* product)
    {
        init_keyboards_dictionary();
        io_iterator_t iter = get_keyboards_iterator();

        CFStringRef device = from_cstr(product);
        if (!device) return true; // will match all ???

        for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter))
        {
            CFStringRef current_device = get_property(curr, kIOHIDProductKey);
            if(current_device == NULL) continue;
            bool match = (CFStringCompare(current_device, device, 0) == kCFCompareEqualTo);
            CFRelease(current_device);

            if( !match ) continue;
            else
            {
                CFRelease(device);
                return true;
            }
        }

        CFRelease(device);
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
    int grab_kb(char* product)
    {
        // Source
        if (pipe(fd) == -1)
        {
            std::cerr << "pipe error: " << errno << std::endl;
            return errno;
        }
        if(product)
        {
            prod = (char*)malloc(strlen(product) + 1);
            strcpy(prod, product);
        }
        thread = std::thread{start_loop, prod};
        // Sink
        return init_sink();
    }

    /*
     * Releases the resources needed to receive key events from and send
     * key events to the OS.
     */
    int release_kb()
    {
        int retval = 0;
        // Source
        if(thread.joinable())
        {
            CFRunLoopStop(listener_loop);
            thread.join();
        }
        else std::cerr << "No thread was running!" << std::endl;

        if(prod) free(prod);

        if (close(fd[0]) == -1)
        {
            std::cerr << "close error: " << errno << std::endl;
            retval = 1;
        }

        if (close(fd[1]) == -1)
        {
            std::cerr << "close error: " << errno << std::endl;
            retval = 1;
        }

        // Sink
        if(exit_sink()) retval = 1;
        return retval;
    }

    void register_device(char* product)
    {
        init_keyboards_dictionary();
        io_iterator_t iter = get_keyboards_iterator();
        listener_loop = CFRunLoopGetCurrent();
        open_matching_devices(product, iter);

        IONotificationPortRef notification_port = IONotificationPortCreate(kIOMainPortDefault);
        CFRunLoopSourceRef notification_source  = IONotificationPortGetRunLoopSource(notification_port);
        CFRunLoopAddSource(listener_loop, notification_source, kCFRunLoopDefaultMode);

        CFRetain(matching_dictionary);
        kern_return_t kr = IOServiceAddMatchingNotification(notification_port, kIOMatchedNotification,
                matching_dictionary, matched_callback, product, &iter);

        if (kr != KERN_SUCCESS) { print_iokit_error("IOServiceAddMatchingNotification", kr); return; }

        for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter)) {}

        kr = IOServiceAddMatchingNotification(notification_port, kIOTerminatedNotification,
                matching_dictionary, terminated_callback, NULL, &iter);

        if (kr != KERN_SUCCESS) { print_iokit_error("IOServiceAddMatchingNotification", kr); return; }

        for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter)) {}

        IOObjectRelease(iter);
    }

    /*
     * all this does is:
     * 1. register all devices if product is null or ""
     * 2. register only one device with exact name match
     * 3. never ever ever register karabiner
     * */
    void open_matching_devices(char* product, io_iterator_t iter)
    {
        CFStringRef device    = product ? from_cstr(product) : NULL;
        CFStringRef karabiner = from_cstr("Karabiner VirtualHIDKeyboard");

        for(mach_port_t curr = IOIteratorNext(iter); curr; curr = IOIteratorNext(iter))
        {
            CFStringRef current_device = get_property(curr, kIOHIDProductKey);

            // continue if the current device is null or karabiner
            if( !current_device || CFStringCompare(current_device, karabiner, 0) == kCFCompareEqualTo )
            {
                CFRelease(current_device);
                continue;
            }

            // register all if no product specified or only specified product
            if( !product || (CFStringCompare(current_device, device, 0) == kCFCompareEqualTo))
                open_device(curr);
            CFRelease(current_device);
        }

        if(product) CFRelease(device);
        CFRelease(karabiner);
    }

    /*
     * Rust calls this with a new key event to send back to the OS. It
     * posts the information to the karabiner kernel extension (which
     * represents a virtual keyboard).
     */
    int send_key(struct DKEvent* e)
    {
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
