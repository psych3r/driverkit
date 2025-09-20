fn main() {
    // Call the new API; output is written to stdout by the C layer
    #[allow(unused_unsafe)]
    {
        karabiner_driverkit::list_keyboards_with_ids();
    }
}

