#![no_std]

use sg2002_c906l_firmware as firmware;

// Make the C ABI entry point a root of the final static library even with LTO.
#[used]
static C906L_ENTRY: extern "C" fn() -> ! = firmware::c906l_rust_main;
