//! SG2002 DW I2C standard-speed, seven-bit polled master driver.
//!
//! Supports write, read and write/repeated-start/read as one bounded operation.
//! At most one read command is outstanding, so RX FIFO depth is not assumed.
//! No DMA, slave mode, arbitrary address probe or bus recovery is provided.
//! IC_ENABLE bit 1 is reserved in SG2002 TRM v1.02: no generic DW ABORT write.

use core::{num::NonZeroU16, ptr::NonNull};
use safe_mmio::{
    UniqueMmioPointer, field, field_shared,
    fields::{ReadOnly, ReadPure, ReadPureWrite, ReadWrite},
};

#[repr(C)]
struct Registers {
    control: ReadPureWrite<u32>,
    target: ReadPureWrite<u32>,
    slave: ReadPureWrite<u32>,
    reserved0: u32,
    data: ReadWrite<u32>,
    high: ReadPureWrite<u32>,
    low: ReadPureWrite<u32>,
    fast_high: ReadPureWrite<u32>,
    fast_low: ReadPureWrite<u32>,
    reserved1: [u32; 2],
    interrupt: ReadPure<u32>,
    mask: ReadPureWrite<u32>,
    raw_interrupt: ReadPure<u32>,
    rx_threshold: ReadPureWrite<u32>,
    tx_threshold: ReadPureWrite<u32>,
    clear_all: ReadOnly<u32>,
    clear_rx_under: ReadOnly<u32>,
    clear_rx_over: ReadOnly<u32>,
    clear_tx_over: ReadOnly<u32>,
    clear_request: ReadOnly<u32>,
    clear_abort: ReadOnly<u32>,
    clear_rx_done: ReadOnly<u32>,
    clear_activity: ReadOnly<u32>,
    clear_stop: ReadOnly<u32>,
    clear_start: ReadOnly<u32>,
    clear_general: ReadOnly<u32>,
    enable: ReadPureWrite<u32>,
    status: ReadPure<u32>,
    tx_level: ReadPure<u32>,
    rx_level: ReadPure<u32>,
    hold: ReadPureWrite<u32>,
    abort_source: ReadPure<u32>,
    nack: ReadPureWrite<u32>,
    dma: ReadPureWrite<u32>,
    dma_tx: ReadPureWrite<u32>,
    dma_rx: ReadPureWrite<u32>,
    setup: ReadPureWrite<u32>,
    general: ReadPureWrite<u32>,
    enable_status: ReadPure<u32>,
}
const _: [(); 0x10] = [(); core::mem::offset_of!(Registers, data)];
const _: [(); 0x34] = [(); core::mem::offset_of!(Registers, raw_interrupt)];
const _: [(); 0x40] = [(); core::mem::offset_of!(Registers, clear_all)];
const _: [(); 0x6c] = [(); core::mem::offset_of!(Registers, enable)];
const _: [(); 0x80] = [(); core::mem::offset_of!(Registers, abort_source)];
const _: [(); 0x88] = [(); core::mem::offset_of!(Registers, dma)];
const _: [(); 0x9c] = [(); core::mem::offset_of!(Registers, enable_status)];
const _: [(); 0xa0] = [(); core::mem::size_of::<Registers>()];

/// An ordinary seven-bit slave address, excluding reserved/general-call ranges.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Address(u8);
impl Address {
    pub const fn new(value: u8) -> Option<Self> {
        if value >= 8 && value <= 0x77 {
            Some(Self(value))
        } else {
            None
        }
    }
}
/// Board-validated standard-speed timing counts. Clock frequency, rise/fall
/// times and the <=100 kHz bus rate must be checked by the board integration.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Timing {
    pub high: NonZeroU16,
    pub low: NonZeroU16,
    pub hold: u16,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    Unconfigured,
    TooLong,
    Timeout,
    DisableTimeout,
    NotIdle,
    Abort(u32),
    Fifo(u32),
}

trait Io {
    fn enable(&mut self, value: u32);
    fn enabled(&self) -> bool;
    fn configure(&mut self, timing: Timing);
    fn target(&mut self, address: Address);
    fn clear(&mut self);
    fn interrupt(&self) -> u32;
    fn abort_source(&self) -> u32;
    fn status(&self) -> u32;
    fn command(&mut self, value: u32);
    fn read(&mut self) -> u8;
}
struct Mmio<'a>(UniqueMmioPointer<'a, Registers>);
impl Io for Mmio<'_> {
    fn enable(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, enable).write(v));
    }
    fn enabled(&self) -> bool {
        crate::ordered_read(|| field_shared!(self.0, enable_status).read()) & 1 != 0
    }
    fn configure(&mut self, timing: Timing) {
        crate::ordered_write(|| field!(self.0, mask).write(0));
        crate::ordered_write(|| field!(self.0, dma).write(0));
        crate::ordered_write(|| field!(self.0, control).write(0x63)); // Slave disabled, restart, standard speed, master.
        crate::ordered_write(|| field!(self.0, high).write(u32::from(timing.high.get())));
        crate::ordered_write(|| field!(self.0, low).write(u32::from(timing.low.get())));
        crate::ordered_write(|| field!(self.0, hold).write(u32::from(timing.hold)));
        crate::ordered_write(|| field!(self.0, rx_threshold).write(0));
        crate::ordered_write(|| field!(self.0, tx_threshold).write(0));
    }
    fn target(&mut self, address: Address) {
        crate::ordered_write(|| field!(self.0, target).write(u32::from(address.0)));
    }
    fn clear(&mut self) {
        let _ = crate::ordered_read(|| field!(self.0, clear_all).read());
    }
    fn interrupt(&self) -> u32 {
        crate::ordered_read(|| field_shared!(self.0, raw_interrupt).read())
    }
    fn abort_source(&self) -> u32 {
        crate::ordered_read(|| field_shared!(self.0, abort_source).read())
    }
    fn status(&self) -> u32 {
        crate::ordered_read(|| field_shared!(self.0, status).read())
    }
    fn command(&mut self, value: u32) {
        crate::ordered_write(|| field!(self.0, data).write(value));
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
    fn disable(&mut self, polls: u32) -> Result<(), Error> {
        self.io.enable(0);
        for _ in 0..polls {
            if !self.io.enabled() {
                return Ok(());
            }
        }
        Err(Error::DisableTimeout)
    }
    fn configure(&mut self, timing: Timing, polls: u32) -> Result<(), Error> {
        self.configured = false;
        if polls == 0 {
            return Err(Error::Timeout);
        }
        self.disable(polls)?;
        if self.io.status() & 1 != 0 {
            return Err(Error::NotIdle);
        }
        self.io.configure(timing);
        self.io.clear();
        self.configured = true;
        Ok(())
    }
    fn sample(&self, budget: &mut u32) -> Result<(u32, u32), Error> {
        if *budget == 0 {
            return Err(Error::Timeout);
        }
        *budget -= 1;
        let interrupt = self.io.interrupt();
        if interrupt & 0x40 != 0 {
            return Err(Error::Abort(self.io.abort_source()));
        }
        if interrupt & 0x0b != 0 {
            return Err(Error::Fifo(interrupt & 0x0b));
        }
        Ok((self.io.status(), interrupt))
    }
    fn wait_status(&self, mask: u32, budget: &mut u32) -> Result<(), Error> {
        loop {
            if self.sample(budget)?.0 & mask == mask {
                return Ok(());
            }
        }
    }
    fn transfer(
        &mut self,
        address: Address,
        write: &[u8],
        read: &mut [u8],
        polls: u32,
    ) -> Result<(), Error> {
        if !self.configured {
            return Err(Error::Unconfigured);
        }
        if write.len() > 256 || read.len() > 256 {
            return Err(Error::TooLong);
        }
        if write.is_empty() && read.is_empty() {
            return Ok(());
        }
        if polls == 0 {
            return Err(Error::Timeout);
        }
        if self.io.enabled() || self.io.status() & 9 != 0 {
            self.configured = false;
            return Err(Error::NotIdle);
        }
        self.io.clear();
        self.io.target(address);
        self.io.enable(1);
        let result = self.exchange(write, read, polls);
        // Disable can itself time out on a stuck external bus. Do not pretend
        // it recovered, reset shared hardware, or automatically retry writes.
        let cleanup = self.disable(polls);
        if result.is_err() || cleanup.is_err() {
            self.configured = false;
        }
        cleanup?;
        self.io.clear();
        result
    }
    fn exchange(&mut self, write: &[u8], read: &mut [u8], mut budget: u32) -> Result<(), Error> {
        loop {
            if budget == 0 {
                return Err(Error::Timeout);
            }
            budget -= 1;
            if self.io.enabled() {
                break;
            }
        }
        for (index, &byte) in write.iter().enumerate() {
            self.wait_status(2, &mut budget)?;
            let stop = if index + 1 == write.len() && read.is_empty() {
                1 << 9
            } else {
                0
            };
            self.io.command(u32::from(byte) | stop);
        }
        let length = read.len();
        for (index, byte) in read.iter_mut().enumerate() {
            self.wait_status(2, &mut budget)?;
            let restart = if index == 0 && !write.is_empty() {
                1 << 10
            } else {
                0
            };
            let stop = if index + 1 == length { 1 << 9 } else { 0 };
            self.io.command((1 << 8) | restart | stop);
            self.wait_status(8, &mut budget)?;
            *byte = self.io.read();
        }
        loop {
            let (status, interrupt) = self.sample(&mut budget)?;
            if interrupt & (1 << 9) != 0 && status & 5 == 4 {
                return Ok(());
            }
        }
    }
}

pub struct I2c<'a> {
    driver: Driver<Mmio<'a>>,
}
impl I2c<'static> {
    /// # Safety
    /// `base` must map an aligned SG2002 I2C register aperture of at least 0xa0
    /// bytes for the firmware lifetime. An activated whole-bus lease must
    /// exclude Linux, every child/client, other cores, IRQ/DMA users and aliases.
    /// Clock/reset/pinmux and pull-ups must be established and bus quiescent.
    /// Camera-control buses remain Linux-owned; a disabled DT node is not a lease.
    pub unsafe fn from_base(base: NonNull<u8>) -> Self {
        Self {
            driver: Driver {
                io: Mmio(unsafe { UniqueMmioPointer::new(base.cast()) }),
                configured: false,
            },
        }
    }
}
impl I2c<'_> {
    /// Only configure a quiescent bus. After a transfer failure, board policy
    /// must prove quiescence/recovery before calling this again.
    pub fn configure(&mut self, timing: Timing, polls: u32) -> Result<(), Error> {
        self.driver.configure(timing, polls)
    }
    /// Up to 256 bytes in each direction. One shared `polls` budget bounds the
    /// operation; a second budget of at most `polls` bounds disable cleanup.
    /// Failure may have written a prefix and leaves the driver unconfigured.
    /// DisableTimeout means quiescence is unproved; retain exclusive ownership.
    pub fn write_read(
        &mut self,
        address: Address,
        write: &[u8],
        read: &mut [u8],
        polls: u32,
    ) -> Result<(), Error> {
        self.driver.transfer(address, write, read, polls)
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::{cell::Cell, collections::VecDeque, vec, vec::Vec};
    #[derive(Default)]
    struct Fake {
        enabled: bool,
        refuse_disable: bool,
        stuck: bool,
        abort: u32,
        stop: bool,
        suppress_stop: bool,
        rx: VecDeque<u8>,
        events: Vec<(&'static str, u32)>,
        commands: Vec<u32>,
        samples: Cell<u32>,
    }
    impl Io for Fake {
        fn enable(&mut self, v: u32) {
            self.events.push(("enable", v));
            if v != 0 || !self.refuse_disable {
                self.enabled = v != 0;
            }
        }
        fn enabled(&self) -> bool {
            self.enabled
        }
        fn configure(&mut self, _: Timing) {
            assert!(!self.enabled);
            self.events.push(("configure", 0));
        }
        fn target(&mut self, a: Address) {
            assert!(!self.enabled);
            self.events.push(("target", u32::from(a.0)));
        }
        fn clear(&mut self) {
            self.events.push(("clear", 0));
            self.stop = false;
        }
        fn interrupt(&self) -> u32 {
            self.samples.set(self.samples.get() + 1);
            if self.abort != 0 {
                0x40
            } else if self.stop {
                1 << 9
            } else {
                0
            }
        }
        fn abort_source(&self) -> u32 {
            self.abort
        }
        fn status(&self) -> u32 {
            if self.stuck {
                0
            } else {
                6 | if self.rx.is_empty() { 0 } else { 8 }
            }
        }
        fn command(&mut self, v: u32) {
            assert!(self.enabled);
            self.commands.push(v);
            if v & 0x100 != 0 {
                assert!(self.rx.is_empty(), "one read in flight");
                self.rx.push_back(0xa5);
            }
            self.stop = !self.suppress_stop && v & 0x200 != 0;
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
    fn address() -> Address {
        Address::new(0x50).unwrap()
    }
    #[test]
    fn validates_addresses_lengths_and_empty_operations() {
        assert!(Address::new(0).is_none());
        assert!(Address::new(0x78).is_none());
        assert!(Address::new(0x80).is_none());
        let mut d = driver();
        assert_eq!(d.transfer(address(), &[], &mut [], 0), Ok(()));
        assert_eq!(
            d.transfer(address(), &[0; 257], &mut [], 10),
            Err(Error::TooLong)
        );
        assert_eq!(d.transfer(address(), &[0], &mut [], 0), Err(Error::Timeout));
        assert!(d.io.events.is_empty());
    }
    #[test]
    fn write_read_has_one_restart_and_only_final_stop() {
        let mut d = driver();
        let mut read = [0; 2];
        d.transfer(address(), &[0x12, 0x34], &mut read, 10).unwrap();
        assert_eq!(d.io.commands, vec![0x12, 0x34, 0x500, 0x300]);
        assert_eq!(read, [0xa5; 2]);
        assert!(!d.io.enabled);
        assert!(d.configured);
    }
    #[test]
    fn read_only_and_write_only_stop_without_restart() {
        let mut d = driver();
        d.transfer(address(), &[], &mut [0], 4).unwrap();
        assert_eq!(d.io.commands, vec![0x300]);
        d.io.commands.clear();
        d.transfer(address(), &[0x81], &mut [], 4).unwrap();
        assert_eq!(d.io.commands, vec![0x281]);
    }
    #[test]
    fn timeout_consumes_one_shared_budget_and_faults_driver() {
        let mut d = driver();
        d.io.stuck = true;
        assert_eq!(d.transfer(address(), &[1], &mut [], 5), Err(Error::Timeout));
        // One budget unit for enable acknowledgement, four for bus status.
        assert_eq!(d.io.samples.get(), 4);
        assert!(!d.io.enabled);
        assert!(!d.configured);
        assert_eq!(
            d.transfer(address(), &[1], &mut [], 5),
            Err(Error::Unconfigured)
        );
    }
    #[test]
    fn abort_source_survives_cleanup_and_does_not_retry() {
        let mut d = driver();
        d.io.abort = 0x1000;
        assert_eq!(
            d.transfer(address(), &[1], &mut [], 5),
            Err(Error::Abort(0x1000))
        );
        assert!(d.io.commands.is_empty());
        assert!(!d.io.enabled);
        assert!(!d.configured);
    }
    #[test]
    fn disable_failure_is_terminal_even_after_successful_transfer() {
        let mut d = driver();
        d.io.refuse_disable = true;
        assert_eq!(
            d.transfer(address(), &[1], &mut [], 5),
            Err(Error::DisableTimeout)
        );
        assert!(d.io.enabled);
        assert!(!d.configured);
    }
    #[test]
    fn stale_stop_cannot_complete_a_new_transfer() {
        let mut d = driver();
        d.io.stop = true;
        d.io.suppress_stop = true;
        assert_eq!(d.transfer(address(), &[1], &mut [], 5), Err(Error::Timeout));
        assert!(!d.configured);
    }
    #[test]
    fn configuration_waits_for_disable_before_programming() {
        let timing = Timing {
            high: NonZeroU16::new(400).unwrap(),
            low: NonZeroU16::new(470).unwrap(),
            hold: 10,
        };
        let mut d = driver();
        d.io.enabled = true;
        d.io.refuse_disable = true;
        assert_eq!(d.configure(timing, 3), Err(Error::DisableTimeout));
        assert_eq!(d.io.events, vec![("enable", 0)]);
        assert!(!d.configured);
        d.io.refuse_disable = false;
        d.configure(timing, 3).unwrap();
        assert_eq!(
            &d.io.events[1..],
            &[("enable", 0), ("configure", 0), ("clear", 0)]
        );
        assert!(!d.io.enabled);
        assert!(d.configured);
    }
}
