//! Exclusive whole-bank SG2002 GPIO driver (TRM v1.02, GPIO chapter).
//!
//! Pin handles borrow the bank exclusively: safe code cannot race whole-bank
//! read/modify/write operations. Pinmux, clocks, Linux handoff and IRQ routing
//! are prerequisites, not powers granted by this driver.

use core::{convert::Infallible, ptr::NonNull};
use embedded_hal::digital::{ErrorType, InputPin, OutputPin, StatefulOutputPin};
use safe_mmio::{
    UniqueMmioPointer, field, field_shared,
    fields::{ReadPure, ReadPureWrite, WriteOnly},
};

#[repr(C)]
struct Registers {
    data: ReadPureWrite<u32>,
    direction: ReadPureWrite<u32>,
    reserved0: [u32; 10],
    enable: ReadPureWrite<u32>,
    mask: ReadPureWrite<u32>,
    edge: ReadPureWrite<u32>,
    polarity: ReadPureWrite<u32>,
    status: ReadPure<u32>,
    raw_status: ReadPure<u32>,
    debounce: ReadPureWrite<u32>,
    eoi: WriteOnly<u32>,
    input: ReadPure<u32>,
    reserved1: [u32; 3],
    sync: ReadPureWrite<u32>,
}
const _: [(); 0x30] = [(); core::mem::offset_of!(Registers, enable)];
const _: [(); 0x4c] = [(); core::mem::offset_of!(Registers, eoi)];
const _: [(); 0x50] = [(); core::mem::offset_of!(Registers, input)];
const _: [(); 0x60] = [(); core::mem::offset_of!(Registers, sync)];
const _: [(); 0x64] = [(); core::mem::size_of::<Registers>()];

/// A checked bank-local pin number. Board pinmux validity is a separate check.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Pin(u8);
impl Pin {
    pub const fn new(index: u8) -> Option<Self> {
        if index < 32 { Some(Self(index)) } else { None }
    }
    const fn mask(self) -> u32 {
        1 << self.0
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Trigger {
    Low,
    High,
    Falling,
    Rising,
}

/// Unique ownership of a complete bank, including its interrupt registers.
///
/// ```compile_fail
/// use sg2002_pac::gpio::{Bank, Pin};
/// fn conflicting(bank: &mut Bank<'_>) {
///     let mut first = bank.output(Pin::new(0).unwrap(), false);
///     let second = bank.output(Pin::new(1).unwrap(), false);
///     embedded_hal::digital::OutputPin::set_high(&mut first).unwrap();
/// }
/// ```
pub struct Bank<'a> {
    registers: UniqueMmioPointer<'a, Registers>,
}

impl Bank<'static> {
    /// # Safety
    /// `base` must map a complete, aligned SG2002 GPIO register bank for the
    /// firmware lifetime. The caller must have an activated exclusive lease
    /// for the **whole bank and its pins**, with Linux, other cores, IRQ/DMA
    /// handlers and other aliases excluded. Clocks/reset/pinmux must already
    /// be established. This does not acquire a lease or change pinmux.
    pub unsafe fn from_base(base: NonNull<u8>) -> Self {
        Self {
            registers: unsafe { UniqueMmioPointer::new(base.cast()) },
        }
    }
}

impl<'mmio> Bank<'mmio> {
    /// Consume the exclusive bank into a fixed set of output lines. Preloads
    /// their latches before setting direction and preserves every other bit.
    /// The group retains ownership of the whole bank, not just `mask`.
    pub fn outputs(mut self, mask: u32, initial: u32) -> OutputGroup<'mmio> {
        let enabled = crate::ordered_read(|| field_shared!(self.registers, enable).read()) & !mask;
        crate::ordered_write(|| field!(self.registers, enable).write(enabled));
        let data = crate::ordered_read(|| field_shared!(self.registers, data).read());
        crate::ordered_write(|| {
            field!(self.registers, data).write((data & !mask) | (initial & mask))
        });
        let direction =
            crate::ordered_read(|| field_shared!(self.registers, direction).read()) | mask;
        crate::ordered_write(|| field!(self.registers, direction).write(direction));
        OutputGroup { bank: self, mask }
    }
    /// Disable and mask all bank interrupts before installing a polled service.
    /// Does not alter any pin direction, output latch or pending status.
    pub fn mask_interrupts(&mut self) {
        crate::ordered_write(|| field!(self.registers, mask).write(u32::MAX));
        crate::ordered_write(|| field!(self.registers, enable).write(0));
    }
    pub fn input(&mut self, pin: Pin) -> Input<'_, 'mmio> {
        let value =
            crate::ordered_read(|| field_shared!(self.registers, direction).read()) & !pin.mask();
        crate::ordered_write(|| field!(self.registers, direction).write(value));
        Input { bank: self, pin }
    }
    /// Preload the output latch before enabling output to avoid an initial
    /// pulse. Only the selected pin's latch, direction and IRQ enable change.
    pub fn output(&mut self, pin: Pin, high: bool) -> Output<'_, 'mmio> {
        let enabled =
            crate::ordered_read(|| field_shared!(self.registers, enable).read()) & !pin.mask();
        crate::ordered_write(|| field!(self.registers, enable).write(enabled));
        self.set(pin, high);
        let direction =
            crate::ordered_read(|| field_shared!(self.registers, direction).read()) | pin.mask();
        crate::ordered_write(|| field!(self.registers, direction).write(direction));
        Output { bank: self, pin }
    }
    fn set(&mut self, pin: Pin, high: bool) {
        let old = crate::ordered_read(|| field_shared!(self.registers, data).read());
        crate::ordered_write(|| {
            field!(self.registers, data).write(if high {
                old | pin.mask()
            } else {
                old & !pin.mask()
            })
        });
    }
    pub fn pending(&self) -> u32 {
        crate::ordered_read(|| field_shared!(self.registers, status).read())
    }
    /// Write-one-to-clear exactly these events. Never reads the EOI register.
    pub fn acknowledge(&mut self, pins: u32) {
        crate::ordered_write(|| field!(self.registers, eoi).write(pins));
    }
}

/// Multiple outputs with one owner of the whole bank. No split handles or
/// shared read/modify/write access are created.
pub struct OutputGroup<'mmio> {
    bank: Bank<'mmio>,
    mask: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OutsideOutputGroup;

impl OutputGroup<'_> {
    /// Read the output latch of group-owned lines. This does not claim that an
    /// external device has reached its powered/ready state.
    pub fn get(&self, mask: u32) -> Result<u32, OutsideOutputGroup> {
        if mask & !self.mask != 0 {
            return Err(OutsideOutputGroup);
        }
        Ok(crate::ordered_read(|| field_shared!(self.bank.registers, data).read()) & mask)
    }

    /// Change only selected group lines. Rejects an invalid mask before MMIO;
    /// bits in `values` outside `mask` do not affect the output latch.
    pub fn set(&mut self, mask: u32, values: u32) -> Result<(), OutsideOutputGroup> {
        if mask & !self.mask != 0 {
            return Err(OutsideOutputGroup);
        }
        if mask == 0 {
            return Ok(());
        }
        let old = crate::ordered_read(|| field_shared!(self.bank.registers, data).read());
        crate::ordered_write(|| {
            field!(self.bank.registers, data).write((old & !mask) | (values & mask))
        });
        Ok(())
    }
}

pub struct Input<'bank, 'mmio> {
    bank: &'bank mut Bank<'mmio>,
    pin: Pin,
}
impl Input<'_, '_> {
    /// Configure this input's interrupt while masked, acknowledge its stale
    /// edge, then enable it. The board must separately install its PLIC handler.
    pub fn enable_interrupt(&mut self, trigger: Trigger, debounce: bool) {
        let registers = &mut self.bank.registers;
        let bit = self.pin.mask();
        let mask = crate::ordered_read(|| field_shared!(*registers, mask).read());
        crate::ordered_write(|| field!(*registers, mask).write(mask | bit));
        let edge = crate::ordered_read(|| field_shared!(*registers, edge).read()) & !bit;
        crate::ordered_write(|| {
            field!(*registers, edge).write(
                edge | if matches!(trigger, Trigger::Rising | Trigger::Falling) {
                    bit
                } else {
                    0
                },
            )
        });
        let polarity = crate::ordered_read(|| field_shared!(*registers, polarity).read()) & !bit;
        crate::ordered_write(|| {
            field!(*registers, polarity).write(
                polarity
                    | if matches!(trigger, Trigger::High | Trigger::Rising) {
                        bit
                    } else {
                        0
                    },
            )
        });
        let old = crate::ordered_read(|| field_shared!(*registers, debounce).read()) & !bit;
        crate::ordered_write(|| {
            field!(*registers, debounce).write(old | if debounce { bit } else { 0 })
        });
        crate::ordered_write(|| field!(*registers, eoi).write(bit));
        let enabled = crate::ordered_read(|| field_shared!(*registers, enable).read()) | bit;
        crate::ordered_write(|| field!(*registers, enable).write(enabled));
        crate::ordered_write(|| field!(*registers, mask).write(mask & !bit));
    }
    pub fn disable_interrupt(&mut self) {
        let enabled = crate::ordered_read(|| field_shared!(self.bank.registers, enable).read())
            & !self.pin.mask();
        crate::ordered_write(|| field!(self.bank.registers, enable).write(enabled));
    }
}
impl ErrorType for Input<'_, '_> {
    type Error = Infallible;
}
impl InputPin for Input<'_, '_> {
    fn is_high(&mut self) -> Result<bool, Infallible> {
        Ok(
            crate::ordered_read(|| field_shared!(self.bank.registers, input).read())
                & self.pin.mask()
                != 0,
        )
    }
    fn is_low(&mut self) -> Result<bool, Infallible> {
        self.is_high().map(|high| !high)
    }
}

pub struct Output<'bank, 'mmio> {
    bank: &'bank mut Bank<'mmio>,
    pin: Pin,
}
impl ErrorType for Output<'_, '_> {
    type Error = Infallible;
}
impl OutputPin for Output<'_, '_> {
    fn set_low(&mut self) -> Result<(), Infallible> {
        self.bank.set(self.pin, false);
        Ok(())
    }
    fn set_high(&mut self) -> Result<(), Infallible> {
        self.bank.set(self.pin, true);
        Ok(())
    }
}
impl StatefulOutputPin for Output<'_, '_> {
    fn is_set_high(&mut self) -> Result<bool, Infallible> {
        Ok(
            crate::ordered_read(|| field_shared!(self.bank.registers, data).read())
                & self.pin.mask()
                != 0,
        )
    }
    fn is_set_low(&mut self) -> Result<bool, Infallible> {
        self.is_set_high().map(|high| !high)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn registers() -> Registers {
        unsafe { core::mem::zeroed() }
    }
    #[test]
    fn output_preserves_neighbours_and_reads_latch_not_input() {
        let mut regs = registers();
        regs.data.0 = 0xa5a5_0000;
        regs.direction.0 = 0x8000_0000;
        regs.enable.0 = u32::MAX;
        {
            let mut bank = Bank {
                registers: UniqueMmioPointer::from(&mut regs),
            };
            let mut pin = bank.output(Pin::new(7).unwrap(), true);
            assert!(pin.is_set_high().unwrap());
            pin.set_low().unwrap();
        }
        assert_eq!(regs.data.0, 0xa5a5_0000);
        assert_eq!(regs.direction.0, 0x8000_0080);
        assert_eq!(regs.enable.0, !0x80);
    }
    #[test]
    fn input_reads_external_port_and_interrupt_configuration_is_local() {
        let mut regs = registers();
        regs.input.0 = 1 << 31;
        regs.direction.0 = u32::MAX;
        regs.mask.0 = u32::MAX;
        regs.edge.0 = 4;
        regs.polarity.0 = 4;
        {
            let mut bank = Bank {
                registers: UniqueMmioPointer::from(&mut regs),
            };
            let mut pin = bank.input(Pin::new(31).unwrap());
            assert!(pin.is_high().unwrap());
            pin.enable_interrupt(Trigger::Rising, true);
        }
        assert_eq!(regs.direction.0, 0x7fff_ffff);
        assert_eq!(regs.edge.0, 0x8000_0004);
        assert_eq!(regs.polarity.0, 0x8000_0004);
        assert_eq!(regs.mask.0, 0x7fff_ffff);
        assert_eq!(regs.enable.0, 0x8000_0000);
        assert_eq!(regs.eoi.0, 0x8000_0000);
    }
    #[test]
    fn pin_numbers_are_checked() {
        assert!(Pin::new(31).is_some());
        assert!(Pin::new(32).is_none());
        assert!(Pin::new(255).is_none());
    }

    #[test]
    fn output_group_preserves_other_lines_and_rejects_outside_mask() {
        let mut regs = registers();
        regs.data.0 = 0xa5a5_0000;
        regs.direction.0 = 0x8000_0000;
        regs.enable.0 = u32::MAX;
        {
            let bank = Bank {
                registers: UniqueMmioPointer::from(&mut regs),
            };
            let mut outputs = bank.outputs(0x81, 0x80);
            assert_eq!(outputs.get(0x81), Ok(0x80));
            outputs.set(1, 1).unwrap();
            assert_eq!(outputs.get(0x81), Ok(0x81));
            assert_eq!(outputs.set(0x100, 0x100), Err(OutsideOutputGroup));
            assert_eq!(outputs.get(0x100), Err(OutsideOutputGroup));
            outputs.set(0x80, 0).unwrap();
        }
        assert_eq!(regs.data.0, 0xa5a5_0001);
        assert_eq!(regs.direction.0, 0x8000_0081);
        assert_eq!(regs.enable.0, !0x81);
    }
}
