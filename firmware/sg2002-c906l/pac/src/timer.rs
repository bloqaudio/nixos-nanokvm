//! Synopsys DesignWare APB timer channel registers.
//!
//! SG2002 exposes eight channels in one timer bank.  Each channel occupies an
//! independent 0x14-byte slice.  This module deliberately models one slice,
//! ending at its per-channel interrupt-status register; the bank-wide EOI and
//! status registers therefore cannot be named through [`TimerChannel`].

use core::ptr::NonNull;

use safe_mmio::{
    UniqueMmioPointer, field, field_shared,
    fields::{ReadOnly, ReadPure, ReadPureWrite},
};

/// Size in bytes of one DesignWare APB timer channel register slice.
pub const CHANNEL_SIZE: usize = 0x14;

#[repr(C)]
struct TimerChannelRegisters {
    load: ReadPureWrite<u32>,
    current: ReadPure<u32>,
    control: ReadPureWrite<u32>,
    eoi: ReadOnly<u32>,
    interrupt_status: ReadPure<u32>,
}

const _: [(); 0x00] = [(); core::mem::offset_of!(TimerChannelRegisters, load)];
const _: [(); 0x04] = [(); core::mem::offset_of!(TimerChannelRegisters, current)];
const _: [(); 0x08] = [(); core::mem::offset_of!(TimerChannelRegisters, control)];
const _: [(); 0x0c] = [(); core::mem::offset_of!(TimerChannelRegisters, eoi)];
const _: [(); 0x10] = [(); core::mem::offset_of!(TimerChannelRegisters, interrupt_status)];
const _: [(); CHANNEL_SIZE] = [(); core::mem::size_of::<TimerChannelRegisters>()];
const _: [(); 4] = [(); core::mem::align_of::<TimerChannelRegisters>()];

/// Unique access to one SG2002 DesignWare APB timer channel.
///
/// This handle covers exactly one 0x14-byte channel slice.  It provides no raw
/// register pointer and no operation for the timer bank's aggregate EOI.
pub struct TimerChannel<'a> {
    registers: UniqueMmioPointer<'a, TimerChannelRegisters>,
}

impl TimerChannel<'static> {
    /// Creates a handle for a statically mapped timer-channel register slice.
    ///
    /// # Safety
    ///
    /// `base` must be four-byte aligned and point to the first byte of one
    /// complete, statically mapped 0x14-byte DesignWare APB timer channel.  The
    /// mapping must permit volatile 32-bit accesses for the lifetime of the
    /// firmware.  No other code or processor may access that channel's slice
    /// while the returned unique handle exists, except through synchronization
    /// which preserves that uniqueness guarantee.  The caller must separately
    /// establish the SG2002 lease, clock, and reset prerequisites.
    pub const unsafe fn from_base(base: NonNull<u8>) -> Self {
        let registers = base.cast::<TimerChannelRegisters>();
        Self {
            // SAFETY: the caller establishes alignment, validity, lifetime,
            // access semantics, and uniqueness for the exact channel slice.
            registers: unsafe { UniqueMmioPointer::new(registers) },
        }
    }
}

impl<'a> TimerChannel<'a> {
    #[cfg(test)]
    fn from_registers(registers: &'a mut TimerChannelRegisters) -> Self {
        Self {
            registers: UniqueMmioPointer::from(registers),
        }
    }

    /// Reads the channel's reload value.
    pub fn load(&self) -> u32 {
        field_shared!(self.registers, load).read()
    }

    /// Sets the value reloaded into the channel counter.
    pub fn set_load(&mut self, value: u32) {
        field!(self.registers, load).write(value);
    }

    /// Reads the channel's current counter value.
    pub fn current(&self) -> u32 {
        field_shared!(self.registers, current).read()
    }

    /// Reads the channel control register.
    pub fn control(&self) -> u32 {
        field_shared!(self.registers, control).read()
    }

    /// Writes the channel control register.
    pub fn set_control(&mut self, value: u32) {
        field!(self.registers, control).write(value);
    }

    /// Reads and acknowledges this channel's interrupt.
    ///
    /// On the DesignWare timer, reading this register clears only the current
    /// channel's interrupt.  The mutable receiver reflects that side effect.
    pub fn read_eoi(&mut self) -> u32 {
        field!(self.registers, eoi).read()
    }

    /// Reads this channel's raw interrupt status without acknowledging it.
    pub fn interrupt_status(&self) -> u32 {
        field_shared!(self.registers, interrupt_status).read()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn registers() -> TimerChannelRegisters {
        TimerChannelRegisters {
            load: ReadPureWrite(0x1111_1111),
            current: ReadPure(0x2222_2222),
            control: ReadPureWrite(0x3333_3333),
            eoi: ReadOnly(0x4444_4444),
            interrupt_status: ReadPure(0x5555_5555),
        }
    }

    #[test]
    fn reads_every_channel_register_from_ram() {
        let mut registers = registers();
        let mut channel = TimerChannel::from_registers(&mut registers);

        assert_eq!(channel.load(), 0x1111_1111);
        assert_eq!(channel.current(), 0x2222_2222);
        assert_eq!(channel.control(), 0x3333_3333);
        assert_eq!(channel.read_eoi(), 0x4444_4444);
        assert_eq!(channel.interrupt_status(), 0x5555_5555);
    }

    #[test]
    fn writes_only_load_and_control_in_ram() {
        let mut registers = registers();
        {
            let mut channel = TimerChannel::from_registers(&mut registers);
            channel.set_load(0xa5a5_5a5a);
            channel.set_control(0x0000_0007);
        }

        assert_eq!(registers.load.0, 0xa5a5_5a5a);
        assert_eq!(registers.current.0, 0x2222_2222);
        assert_eq!(registers.control.0, 0x0000_0007);
        assert_eq!(registers.eoi.0, 0x4444_4444);
        assert_eq!(registers.interrupt_status.0, 0x5555_5555);
    }
}
