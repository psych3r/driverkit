
fn main() {

   let keeb = "Karabiner DriverKit VirtualHIDKeyboard 1.6.0";
    // let keeb = "Apple Internal Keyboard / Trackpad";
    grab_kb(keeb);

    //unsafe { damn::grab_kb(std::ptr::null_mut()) };
    let mut event = KeyEvent { r#type: 0, page: 0, usage: 0, };

    loop {
        wait_key(&mut event);
        if event.usage !=  0xffffffff && event.usage != 0x1 {
            println!("{}", event);
        } else {
            continue;
        }

        if event.page == 0xff && event.usage == 0x3 {
            let mut ev =  KeyEvent{ r#type: event.r#type, page: 0x07, usage: 0xe0};
            //println!("caught! {}", ev);
            send_key(&mut ev);
            continue;
        }

        send_key(&mut event);
    }

    // let mut ev: KeyEvent = KeyEvent{ r#type: 10, page: 10, usage: 10 };
    // send_key(&mut ev);
    // wait_key(&mut ev);

    // if g != 0 {
    //     println!("grab_kb failed with error code: {}", g);
    // } else {
    //     println!("grab_kb succeeded!");
    // }
    //
    // release_kb();

}
