//! Typed SG2002 peripheral register access.
//!
//! Register layouts in this crate are hand-audited against the SG2002 TRM.
//! SoC instance addresses and ownership policy belong to the generated board
//! contract rather than this crate.

#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]

// Volatile access prevents compiler elision but is not a RISC-V device-ordering
// barrier. Keep register sequencing explicit even if a platform's PMAs happen
// to make its current peripheral mapping strongly ordered.
fn io_fence() {
    #[cfg(target_arch = "riscv64")]
    unsafe {
        core::arch::asm!("fence iorw, iorw", options(nostack));
    }
    #[cfg(not(target_arch = "riscv64"))]
    core::sync::atomic::compiler_fence(core::sync::atomic::Ordering::SeqCst);
}

fn ordered_read<T>(read: impl FnOnce() -> T) -> T {
    io_fence();
    let value = read();
    io_fence();
    value
}

fn ordered_write(write: impl FnOnce()) {
    io_fence();
    write();
    io_fence();
}

pub mod gpio;
pub mod i2c;
pub mod spi;
pub mod timer;
pub mod uart;
