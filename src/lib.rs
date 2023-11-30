use std::ffi::CString;

mod interface {

use std::os::raw::c_char;
use std::fmt;

extern "C" {
    // ./c_src/mac/dext.cpp
    pub fn release_kb() -> i32;
    pub fn grab_kb(product: *mut c_char) -> i32;
    pub fn send_key(e: *mut KeyEvent) -> i32;
    pub fn wait_key(e: *mut KeyEvent) -> i32;
}

#[repr(C)]
#[derive(Debug)]
pub struct KeyEvent {
    pub value: u64,
    pub page: u32,
    pub code: u32,
    /*

    IOHIDElementRef element = IOHIDValueGetElement(value);
    e.type = IOHIDValueGetIntegerValue(value);
    0x0 => Release 
    0x1 => Press 
    _   => press


    e.page = IOHIDElementGetUsagePage(element);
    key code space
    0x7 => most common keys
    wheen how ya ragel

    e.code = IOHIDElementGetUsage(element);
    the key code in that space

    * */
}

impl fmt::Display for KeyEvent {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let keycode =  0x0000FFFF & ( self.page << 8 | self.code );
        write!(f, "Event: {{ type: {:#x}, code: {:#0006x}  }}", self.value, keycode)
        //write!(f, "Event: {{ type: {:#x}, page: {:#x}, code: {:#x}  }}", self.value, self.page, self.code)
    }
}


}

pub fn release_kb() -> i32 {
    unsafe { interface::release_kb() }
}

pub fn grab_kb(product: &str) -> i32 {
    // let keeb = "Karabiner DriverKit VirtualHIDKeyboard 1.6.0";
    // let keeb = "Apple Internal Keyboard / Trackpad";

    let c_str_ptr = CString::new(product).expect("CString::new failed").into_raw();
    let ret = unsafe { interface::grab_kb(c_str_ptr) };

    // Convert the raw pointer back to CString to free the memory
    unsafe { let _ = CString::from_raw(c_str_ptr); }
    ret
}

pub fn send_key(e: *mut interface::KeyEvent) -> i32 {
    unsafe { interface::send_key(e) }
}

pub fn wait_key(e: *mut interface::KeyEvent) -> i32 {
    unsafe { interface::wait_key(e) }
}
