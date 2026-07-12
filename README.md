# driverkit

driverkit is a minimal wrapper around [Karabiner-DriverKit-VirtualHIDDevice (dext)](https://github.com/pqrs-org/Karabiner-DriverKit-VirtualHIDDevice) and [Karabiner-VirtualHIDDevice (kext)](https://github.com/pqrs-org/Karabiner-VirtualHIDDevice) intended for kanata
macos support.

The DriverKit backend targets Karabiner-DriverKit-VirtualHIDDevice v8.0.0
(client protocol 7). It is not compatible with older protocol-5 daemons such
as VirtualHIDDevice v6.2.0.

## Installation

Update the submodules first

    git submodule update --init --recursive

then

    cargo build
