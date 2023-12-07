pub use interface::DKEvent;
use std::ffi::CString;

mod interface {
    use std::fmt;
    use std::os::raw::c_char;

    extern "C" {
        pub fn grab() -> i32;
        pub fn release();
        pub fn send_key(e: *mut DKEvent) -> i32;
        pub fn wait_key(e: *mut DKEvent) -> i32;
        pub fn list_keyboards();
        pub fn driver_activated() -> bool;
        pub fn device_matches(product: *mut c_char) -> bool;
        pub fn register_device(product: *mut c_char) -> bool;
    }

    #[repr(C)]
    #[derive(Debug)]
    pub struct DKEvent {
        pub value: u64,
        pub page: u32,
        pub code: u32,
    }

    impl fmt::Display for DKEvent {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            // let keycode = 0x0000FFFF & (self.page << 8 | self.code);
            // write!( f, "Event: {{ type: {:#x}, code: {:#0006x}  }}", self.value, keycode)
            write!(
                f,
                "Event: {{ type: {:#x}, page: {:#x}, code: {:#x} }}",
                self.value, self.page, self.code
            )
        }
    }
}

/// Sends a keyevent to the OS via the Karabiner-VirtualHIDDevice driver
pub fn send_key(e: *mut interface::DKEvent) -> i32 {
    unsafe { interface::send_key(e) }
}

/// Reads a new key event, blocks until a new event is ready.
pub fn wait_key(e: *mut interface::DKEvent) -> i32 {
    unsafe { interface::wait_key(e) }
}

/// Relinquishs control of all registered devices
pub fn release() {
    unsafe { interface::release() }
}

/// Lists the valid keyboard names to be registered with register_device()
pub fn list_keyboards() {
    unsafe { interface::list_keyboards() }
}

/// Registers a device for IO control
/// has to be called before grab()
/// when called with an empty string
/// registers all valid devices
pub fn register_device(product: &str) -> bool {
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

// checking for driver_activated and device_matches is done on kanata's side 
// pub enum RegisterError {
//     DriverInactive,
//     DeviceMismatch,
//     RegisterFailed,
// }
//
// pub fn register_device(product: &str) -> Result<(), RegisterError> {
//     if !driver_activated() {
//         Err(RegisterError::DriverInactive)
//     } else if !device_matches(product) {
//         Err(RegisterError::DeviceMismatch)
//     } else if register(product) {
//         Ok(())
//     } else {
//         Err(RegisterError::RegisterFailed)
//     }
// }

/// Grabs control of registered devices and starts the monitoring loop
/// at least on successful call to register_device has to be done before
/// calling grab() 
pub fn grab() -> bool {
    unsafe { interface::grab() == 0 }
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
