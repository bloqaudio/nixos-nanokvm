//! SG2002 DW SPI, bounded polled master transactions on hardware CS0.
//!
//! TRM v1.02 specifies eight entries in each FIFO and only SER bit 0. Entire
//! transactions are preloaded before selecting CS, avoiding FIFO starvation
//! and the resulting mid-transaction CS deassertion. Longer transfers need a
//! separate reviewed streaming/DMA or GPIO-CS implementation.

use core::ptr::NonNull;
use embedded_hal::spi::{Mode, Phase, Polarity};
use safe_mmio::{
    UniqueMmioPointer, field, field_shared,
    fields::{ReadOnly, ReadPure, ReadPureWrite, ReadWrite},
};

#[repr(C)]
struct Registers {
    control: ReadPureWrite<u32>,
    count: ReadPureWrite<u32>,
    enable: ReadPureWrite<u32>,
    microwire: ReadPureWrite<u32>,
    select: ReadPureWrite<u32>,
    baud: ReadPureWrite<u32>,
    tx_threshold: ReadPureWrite<u32>,
    rx_threshold: ReadPureWrite<u32>,
    tx_level: ReadPure<u32>,
    rx_level: ReadPure<u32>,
    status: ReadPure<u32>,
    mask: ReadPureWrite<u32>,
    interrupt: ReadPure<u32>,
    raw_interrupt: ReadPure<u32>,
    clear_tx: ReadOnly<u32>,
    clear_rx_over: ReadOnly<u32>,
    clear_rx_under: ReadOnly<u32>,
    clear_master: ReadOnly<u32>,
    clear_all: ReadOnly<u32>,
    dma: ReadPureWrite<u32>,
    dma_tx: ReadPureWrite<u32>,
    dma_rx: ReadPureWrite<u32>,
    reserved: [u32; 2],
    data: ReadWrite<u32>,
}
const _: [(); 0x10] = [(); core::mem::offset_of!(Registers, select)];
const _: [(); 0x28] = [(); core::mem::offset_of!(Registers, status)];
const _: [(); 0x34] = [(); core::mem::offset_of!(Registers, raw_interrupt)];
const _: [(); 0x48] = [(); core::mem::offset_of!(Registers, clear_all)];
const _: [(); 0x4c] = [(); core::mem::offset_of!(Registers, dma)];
const _: [(); 0x60] = [(); core::mem::offset_of!(Registers, data)];
const _: [(); 0x64] = [(); core::mem::size_of::<Registers>()];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Divider(u16);
impl Divider {
    /// SPI clock is the board-provided controller clock divided by this value.
    pub const fn new(value: u16) -> Option<Self> {
        if value >= 2 && value & 1 == 0 {
            Some(Self(value))
        } else {
            None
        }
    }
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    Unconfigured,
    TooLong,
    Timeout,
    FifoNotEmpty,
    FifoFull,
    Hardware(u32),
}

trait Io {
    fn enable(&mut self, value: u32);
    fn select(&mut self, value: u32);
    fn control(&mut self, value: u32);
    fn baud(&mut self, value: u32);
    fn mask(&mut self, value: u32);
    fn dma(&mut self, value: u32);
    fn clear(&mut self);
    fn status(&self) -> u32;
    fn errors(&self) -> u32;
    fn write(&mut self, byte: u8);
    fn read(&mut self) -> u8;
}
struct Mmio<'a>(UniqueMmioPointer<'a, Registers>);
impl Io for Mmio<'_> {
    fn enable(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, enable).write(v));
    }
    fn select(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, select).write(v));
    }
    fn control(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, control).write(v));
    }
    fn baud(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, baud).write(v));
    }
    fn mask(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, mask).write(v));
    }
    fn dma(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, dma).write(v));
    }
    fn clear(&mut self) {
        let _ = crate::ordered_read(|| field!(self.0, clear_all).read());
    }
    fn status(&self) -> u32 {
        crate::ordered_read(|| field_shared!(self.0, status).read())
    }
    fn errors(&self) -> u32 {
        crate::ordered_read(|| field_shared!(self.0, raw_interrupt).read()) & 0x2e
    }
    fn write(&mut self, byte: u8) {
        crate::ordered_write(|| field!(self.0, data).write(u32::from(byte)));
    }
    fn read(&mut self) -> u8 {
        crate::ordered_read(|| field!(self.0, data).read()) as u8
    }
}

struct Driver<I> {
    io: I,
    configured: bool,
}
impl<I: Io> Driver<I> {
    fn configure(&mut self, divider: Divider, mode: Mode) {
        self.io.enable(0);
        self.io.select(0);
        self.io.mask(0);
        self.io.dma(0);
        self.io.baud(u32::from(divider.0));
        let cpol = if mode.polarity == Polarity::IdleHigh {
            1 << 7
        } else {
            0
        };
        let cpha = if mode.phase == Phase::CaptureOnSecondTransition {
            1 << 6
        } else {
            0
        };
        self.io.control(7 | cpol | cpha); // 8 bits, Motorola full duplex.
        self.io.clear();
        self.configured = true;
    }
    fn transfer(&mut self, bytes: &mut [u8], polls: u32) -> Result<(), Error> {
        if !self.configured {
            return Err(Error::Unconfigured);
        }
        if bytes.len() > 8 {
            return Err(Error::TooLong);
        }
        if bytes.is_empty() {
            return Ok(());
        }
        if polls == 0 {
            return Err(Error::Timeout);
        }
        self.io.enable(1);
        let result = self.exchange(bytes, polls);
        // SPIENR=0 immediately halts transfers and clears both FIFOs. A failed
        // transaction may have clocked a prefix: never silently retry it.
        self.io.enable(0);
        self.io.select(0);
        self.io.clear();
        result
    }
    fn exchange(&mut self, bytes: &mut [u8], polls: u32) -> Result<(), Error> {
        if self.io.status() & 0x0d != 4 {
            return Err(Error::FifoNotEmpty);
        }
        for &byte in bytes.iter() {
            if self.io.status() & 2 == 0 {
                return Err(Error::FifoFull);
            }
            self.io.write(byte);
        }
        self.io.select(1);
        let mut received = 0;
        for _ in 0..polls {
            let errors = self.io.errors();
            if errors != 0 {
                return Err(Error::Hardware(errors));
            }
            let status = self.io.status();
            if received < bytes.len() && status & 8 != 0 {
                bytes[received] = self.io.read();
                received += 1;
            }
            // A fresh status read after the final RBR read is not required:
            // observing idle+TX-empty already proves all clocks completed.
            if received == bytes.len() && status & 5 == 4 {
                return Ok(());
            }
        }
        Err(Error::Timeout)
    }
}

/// Exclusive SPI controller; never clones or hands out raw register pointers.
pub struct Spi<'a> {
    driver: Driver<Mmio<'a>>,
}
impl Spi<'static> {
    /// # Safety
    /// `base` must map an aligned SG2002 SPI register aperture, at least 0x64
    /// bytes, for the firmware lifetime. An activated lease must exclude Linux
    /// and all other CPU/IRQ/DMA users of the whole controller, all pins and
    /// children. Clock/reset/pinmux must be established and the bus quiescent.
    /// Physical SPI0/1 are named SPI1/2 in the C906L interrupt table.
    pub unsafe fn from_base(base: NonNull<u8>) -> Self {
        Self {
            driver: Driver {
                io: Mmio(unsafe { UniqueMmioPointer::new(base.cast()) }),
                configured: false,
            },
        }
    }
}
impl Spi<'_> {
    /// Set 8-bit Motorola full-duplex mode, with DMA and interrupts disabled.
    /// Must only be called on a quiescent bus; this clears both FIFOs.
    pub fn configure(&mut self, divider: Divider, mode: Mode) {
        self.driver.configure(divider, mode);
    }
    /// Transfer up to eight bytes under one hardware-CS0 assertion. `polls`
    /// bounds total receive/completion iterations, not time. Failure can leave
    /// a prefix in `bytes` and on the wire; controller ends disabled every time.
    pub fn transfer_in_place(&mut self, bytes: &mut [u8], polls: u32) -> Result<(), Error> {
        self.driver.transfer(bytes, polls)
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::{collections::VecDeque, vec::Vec};
    #[derive(Default)]
    struct Fake {
        events: Vec<(&'static str, u32)>,
        tx: Vec<u8>,
        rx: VecDeque<u8>,
        selected: bool,
        stuck: bool,
        full: bool,
        errors: u32,
    }
    impl Io for Fake {
        fn enable(&mut self, v: u32) {
            self.events.push(("enable", v));
            if v == 0 {
                self.tx.clear();
                self.rx.clear();
            }
        }
        fn select(&mut self, v: u32) {
            self.events.push(("select", v));
            self.selected = v != 0;
            if self.selected {
                self.rx.extend(self.tx.iter().map(|byte| byte ^ 0xff));
            }
        }
        fn control(&mut self, v: u32) {
            self.events.push(("control", v));
        }
        fn baud(&mut self, v: u32) {
            self.events.push(("baud", v));
        }
        fn mask(&mut self, v: u32) {
            self.events.push(("mask", v));
        }
        fn dma(&mut self, v: u32) {
            self.events.push(("dma", v));
        }
        fn clear(&mut self) {
            self.events.push(("clear", 0));
        }
        fn status(&self) -> u32 {
            if self.stuck && self.selected {
                1
            } else {
                4 | if self.full { 0 } else { 2 } | if self.rx.is_empty() { 0 } else { 8 }
            }
        }
        fn errors(&self) -> u32 {
            self.errors
        }
        fn write(&mut self, byte: u8) {
            assert!(!self.selected);
            self.events.push(("data", u32::from(byte)));
            self.tx.push(byte);
        }
        fn read(&mut self) -> u8 {
            self.rx.pop_front().unwrap()
        }
    }
    fn driver() -> Driver<Fake> {
        Driver {
            io: Fake::default(),
            configured: true,
        }
    }
    fn stopped(d: &Driver<Fake>) {
        assert_eq!(
            &d.io.events[d.io.events.len() - 3..],
            &[("enable", 0), ("select", 0), ("clear", 0)]
        );
    }
    #[test]
    fn validates_divider_and_transaction_before_io() {
        assert!(Divider::new(0).is_none());
        assert!(Divider::new(3).is_none());
        assert!(Divider::new(65534).is_some());
        let mut d = driver();
        assert_eq!(d.transfer(&mut [0; 9], 10), Err(Error::TooLong));
        assert_eq!(d.transfer(&mut [0], 0), Err(Error::Timeout));
        assert_eq!(d.transfer(&mut [], 0), Ok(()));
        assert!(d.io.events.is_empty());
        d.configured = false;
        assert_eq!(d.transfer(&mut [0], 10), Err(Error::Unconfigured));
        assert!(d.io.events.is_empty());
    }
    #[test]
    fn preloads_complete_transaction_then_receives_and_stops() {
        let mut d = driver();
        let mut data = [1, 2, 3, 4, 5, 6, 7, 8];
        d.transfer(&mut data, 8).unwrap();
        assert_eq!(data, [254, 253, 252, 251, 250, 249, 248, 247]);
        assert_eq!(d.io.events[9], ("select", 1));
        stopped(&d);
    }
    #[test]
    fn full_fifo_never_selects_slave_and_every_error_stops() {
        let mut d = driver();
        d.io.full = true;
        assert_eq!(d.transfer(&mut [0], 5), Err(Error::FifoFull));
        assert!(!d.io.events.contains(&("select", 1)));
        stopped(&d);
        d.io.full = false;
        d.io.stuck = true;
        assert_eq!(d.transfer(&mut [0], 5), Err(Error::Timeout));
        stopped(&d);
        d.io.errors = 8;
        assert_eq!(d.transfer(&mut [0], 5), Err(Error::Hardware(8)));
        stopped(&d);
    }
    #[test]
    fn configuration_disables_dma_interrupts_and_encodes_mode() {
        let mut d = driver();
        d.configure(Divider::new(4).unwrap(), embedded_hal::spi::MODE_3);
        assert_eq!(d.io.events[0], ("enable", 0));
        assert!(d.io.events.contains(&("control", 0xc7)));
        assert!(d.io.events.contains(&("dma", 0)));
        assert!(d.io.events.contains(&("mask", 0)));
    }
}
