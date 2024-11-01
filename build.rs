fn main() {
    let mut build = cc::Build::new();

    build
        .file("c_src/driverkit.cpp")
        .cpp(true)
        .std("c++2a")
        .flag("-w")
        .shared_flag(true)
        .flag("-fPIC");

    if let os_info::Version::Semantic(major, minor, patch) = os_info::get().version() {
        if major <= &10 {
            println!("macOS version {major}.{minor}.{patch}, using kext...");
            // kext
            build.flag("-D");
            build.flag("USE_KEXT");
            build.include("c_src/Karabiner-VirtualHIDDevice/dist/include");
        } else {
            println!("macOS version {major}.{minor}.{patch}, using dext...");
            // dext
            build.include(
                "c_src/Karabiner-DriverKit-VirtualHIDDevice/include/pqrs/karabiner/driverkit",
            );
            build.include("c_src/Karabiner-DriverKit-VirtualHIDDevice/src/Daemon/vendor/include");
        }
    }

    build.compile("driverkit");

    println!("cargo:rerun-if-changed=c_src/c_src/driverkit.hpp");
    println!("cargo:rerun-if-changed=c_src/c_src/driverkit.cpp");
    println!("cargo:rustc-link-lib=framework=IOKit");
    println!("cargo:rustc-link-lib=framework=CoreFoundation");
}
