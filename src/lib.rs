pub use interface::DKEvent;
use std::ffi::CString;

mod interface {
    use std::fmt;
    use std::os::raw::c_char;

    extern "C" {
        pub fn release_kb() -> i32;
        pub fn grab_kb(product: *mut c_char) -> i32;
        pub fn send_key(e: *mut DKEvent) -> i32;
        pub fn wait_key(e: *mut DKEvent) -> i32;
        pub fn list_keyboards() -> i32;
        pub fn driver_activated() -> bool;
        pub fn device_matches(product: *mut c_char) -> i32;
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
            let keycode = 0x0000FFFF & (self.page << 8 | self.code);
            write!(
                f,
                "Event: {{ type: {:#x}, code: {:#0006x}  }}",
                self.value, keycode
            )
            //write!(f, "Event: {{ type: {:#x}, page: {:#x}, code: {:#x}  }}", self.value, self.page, self.code)
        }
    }
}

pub enum GrabError {
    DeviceMismatch,
    DriverInactive,
    GrabbingFailed,
}

pub fn send_key(e: *mut interface::DKEvent) -> i32 {
    unsafe { interface::send_key(e) }
}
pub fn wait_key(e: *mut interface::DKEvent) -> i32 {
    unsafe { interface::wait_key(e) }
}
pub fn release_kb() -> i32 {
    unsafe { interface::release_kb() }
}
pub fn list_keyboards() -> i32 {
    unsafe { interface::list_keyboards() }
}

pub fn grab_kb(product: &str) -> Result<(), GrabError> {
    if !driver_activated() {
        Err(GrabError::DriverInactive)
    } else if !device_matches(product) {
        Err(GrabError::DeviceMismatch)
    } else {
        match grab(product) {
            0 => Ok(()),
            _ => Err(GrabError::GrabbingFailed),
        }
    }
}

fn grab(product: &str) -> i32 {
    let c_str_ptr = if product.is_empty() {
        std::ptr::null_mut()
    } else {
        CString::new(product)
            .expect("CString::new failed")
            .into_raw()
    };

    let ret = unsafe { interface::grab_kb(c_str_ptr) };

    // Convert the raw pointer back to CString to free the memory
    if !product.is_empty() {
        unsafe {
            let _ = CString::from_raw(c_str_ptr);
        }
    }
    ret
}

pub fn driver_activated() -> bool {
    unsafe { interface::driver_activated() }
}

pub fn device_matches(product: &str) -> bool {
    if product.is_empty() {
        true
    } else {
        let c_str_ptr = CString::new(product)
            .expect("CString::new failed")
            .into_raw();
        let ret = unsafe { interface::device_matches(c_str_ptr) };
        unsafe {
            let _ = CString::from_raw(c_str_ptr);
        }
        ret == 0
    }
}
