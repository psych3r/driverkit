pub use interface::DKEvent;
use std::ffi::CString;
use std::ffi::CStr;
use std::fmt;

mod interface {
    use std::fmt;
    use std::os::raw::c_char;

    extern "C" {
        pub fn grab() -> i32;
        pub fn release();
        pub fn send_key(e: *mut DKEvent) -> i32;
        pub fn wait_key(e: *mut DKEvent) -> i32;
        pub fn list_keyboards();
        pub fn list_keyboards_with_ids();
        pub fn driver_activated() -> bool;
        pub fn device_matches(product: *mut c_char) -> bool;
        pub fn register_device(product: *mut c_char) -> bool;
        pub fn register_device_hash(hash: u64) -> bool;
        pub fn get_device_list(array_length: *mut usize) -> *const DeviceData;
    }

    #[repr(C)]
    #[derive(Debug)]
    pub struct DKEvent {
        pub value: u64,
        pub page: u32,
        pub code: u32,
    }

    #[repr(C)]
    #[derive(Debug)]
    pub struct DeviceData {
        pub product_key: *const c_char,
        pub vendor_id:   u32,
        pub product_id:  u32,
    }

    impl fmt::Display for DKEvent {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            let keycode = 0x0000FFFF & (self.page << 8 | self.code);
            write!(
                f,
                "Event: {{ type: {:#x}, code: {:#0006x}  }}",
                self.value, keycode
            )
            // write!( f, "Event: {{ type: {:#x}, page: {:#x}, code: {:#x} }}", self.value, self.page, self.code)
        }
    }
}

pub struct DeviceData {
    pub product_key: String,
    pub vendor_id:   u32,
    pub product_id:  u32,
    pub hash:        u64,
}

impl PartialEq<str> for DeviceData {
    fn eq(&self, other: &str) -> bool {
        if let Some(hash) = parse_register_request(other) {
            self.hash == hash
        } else {
            self.product_key.trim() == other.trim()
        }
    }
}

impl fmt::Display for DeviceData {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            // "{:<20} {:<10} {:<10} {}",
            "0x{:<18X} {:<10} {:<10} {}",
            self.hash,
            self.vendor_id,
            self.product_id,
            self.product_key
        )
    }
}

fn fnv1a_64(data: &str) -> u64 {
    const FNV_OFFSET: u64 = 14695981039346656037;
    const FNV_PRIME:  u64 = 1099511628211;
    let mut hash = FNV_OFFSET;
    for b in data.as_bytes() {
        hash ^= *b as u64;
        hash = hash.wrapping_mul(FNV_PRIME);
    }
    hash
}

/// Sends a keyevent to the OS via the Karabiner-VirtualHIDDevice driver
pub fn send_key(e: *mut interface::DKEvent) -> i32 {
    unsafe { interface::send_key(e) }
}

/// Reads a new key event, blocks until a new event is ready.
pub fn wait_key(e: *mut interface::DKEvent) -> i32 {
    unsafe { interface::wait_key(e) }
    // unsafe {
    //     let r = interface::wait_key(e);
    //     // don't know why macos emmits such events with every press/release
    //     // they can be ommited or not, kanata will work either way
    //     if (*e).value > 1 || (*e).code == 0xffffffff || (*e).code == 0x1 {
    //         wait_key(e);
    //     }
    //     r
    // }
}

/// Relinquishs control of all registered devices
pub fn release() {
    unsafe { interface::release() }
}

/// Lists the valid keyboard names to be registered with register_device()
pub fn list_keyboards() {
    unsafe { interface::list_keyboards() }
}

/// Lists keyboard names with vendor/product IDs (tab-separated, decimal) and exits.
pub fn list_keyboards_with_ids() {
    unsafe { interface::list_keyboards_with_ids() }
}

/// Registers a device for IO control
/// has to be called before grab()
/// when called with an empty string
/// registers all valid devices
fn register_device_str(product: &str) -> bool {
    let c_str_ptr = if product.is_empty() {
        std::ptr::null_mut()
    } else {
        CString::new(product)
            .expect("CString::new failed")
            .into_raw()
    };

    let registered = unsafe { interface::register_device(c_str_ptr) };

    // Convert the raw pointer back to CString to free the memory
    if !product.is_empty() {
        unsafe {
            let _ = CString::from_raw(c_str_ptr);
        }
    }

    registered
}

fn parse_register_request(input: &str) -> Option<u64> {
    // println!("Parsing register request: {}", input);
    let s = input.strip_prefix("0x")
        .or_else(|| input.strip_prefix("0X"))
        .unwrap_or(input);
    // println!("Parsing register request: {}", s);
    u64::from_str_radix(s, 16).ok()
}

pub fn register_device(input: &str) -> bool {
    // println!("Registering device: {}", input);
    if let Some(hash) = parse_register_request(input) {
        // println!("Registering device with hash: {}", hash);
        unsafe { interface::register_device_hash(hash) }
    } else {
        // println!("Registering device with product key: {}", input);
        register_device_str(input)
    }
}

/// Grabs control of registered devices and starts the monitoring loop
/// at least on successful call to register_device has to be done before
/// calling grab()
pub fn grab() -> bool {
    unsafe { interface::grab() == 0 }
}

pub fn fetch_devices() -> Vec<DeviceData> {
    unsafe {
        let mut len: usize = 0;
        let ptr = interface::get_device_list(&mut len as *mut usize);
        if ptr.is_null() || len == 0 {
            return Vec::new();
        }

        let slice = std::slice::from_raw_parts(ptr, len);

        slice
            .iter()
            .map(|d| {
                // Convert once and reuse
                let product_str = CStr::from_ptr(d.product_key)
                    .to_string_lossy()
                    .into_owned();

                let key = format!( "{}:{}:{}", d.vendor_id, d.product_id, product_str);

                DeviceData {
                    product_key: product_str,
                    vendor_id: d.vendor_id,
                    product_id: d.product_id,
                    hash: fnv1a_64(&key),
                }
            })
        .collect()
    }
}

/// Checks if Karabiner-VirtualHIDDevice-Manager is activated
/// it has to be avtivated before registering devices
pub fn driver_activated() -> bool {
    unsafe { interface::driver_activated() }
}

/// Checks if a device is valid and connected so it can be registered
pub fn device_matches(product: &str) -> bool {
    if product.is_empty() {
        // will match all devices in this case
        true
    } 
    else if let Some(hash) = parse_register_request(product) {
        let devices = fetch_devices();
        devices.iter().any(|d| d.hash == hash)
    } else {
        let c_str_ptr = CString::new(product)
            .expect("CString::new failed")
            .into_raw();
        let ret = unsafe { interface::device_matches(c_str_ptr) };
        unsafe {
            let _ = CString::from_raw(c_str_ptr);
        }
        ret
    }
}
