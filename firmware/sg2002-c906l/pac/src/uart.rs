//! Bounded polled SG2002 DW UART driver, based on TRM v1.02.
//!
//! Only 8N1, non-DMA operation is supported. The TRM labels MCR bit 4
//! reserved, so the usual 16550 loopback bit is deliberately not exposed.
//! There is no clock, reset, pinmux or PLIC access here.

use core::{num::NonZeroU16, ptr::NonNull};
use safe_mmio::{
    UniqueMmioPointer, field, field_shared,
    fields::{ReadOnly, ReadPureWrite, ReadWrite, WriteOnly},
};

#[repr(C)]
struct Registers {
    data_divisor: ReadWrite<u32>,
    interrupt_divisor: ReadPureWrite<u32>,
    fifo: WriteOnly<u32>,
    line: ReadPureWrite<u32>,
    modem: ReadPureWrite<u32>,
    line_status: ReadOnly<u32>,
    reserved: [u32; 25],
    status: ReadOnly<u32>,
}
const _: [(); 0x0c] = [(); core::mem::offset_of!(Registers, line)];
const _: [(); 0x14] = [(); core::mem::offset_of!(Registers, line_status)];
const _: [(); 0x7c] = [(); core::mem::offset_of!(Registers, status)];
const _: [(); 0x80] = [(); core::mem::size_of::<Registers>()];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    Unconfigured,
    Timeout,
    ConfigurationRejected,
    /// LSR error bits 1 through 4. The affected FIFO head, if present, was
    /// drained. Bit 7 is not an error for the current head: it can refer to a
    /// later FIFO entry and is evaluated when that entry reaches the head.
    Receive(u8),
}

// Semantic I/O operations allow host tests to model side-effecting register
// aliases (DLL/THR/RBR and DLH/IER), not pretend ordinary RAM is a UART FIFO.
trait Io {
    fn status(&mut self) -> u32;
    fn line_status(&mut self) -> u32;
    fn line(&self) -> u32;
    fn set_line(&mut self, value: u32);
    fn set_interrupt_divisor(&mut self, value: u32);
    fn set_data_divisor(&mut self, value: u32);
    fn data(&mut self) -> u32;
    fn set_fifo(&mut self, value: u32);
    fn set_modem(&mut self, value: u32);
}

struct Mmio<'a>(UniqueMmioPointer<'a, Registers>);
impl Io for Mmio<'_> {
    fn status(&mut self) -> u32 {
        crate::ordered_read(|| field!(self.0, status).read())
    }
    fn line_status(&mut self) -> u32 {
        crate::ordered_read(|| field!(self.0, line_status).read())
    }
    fn line(&self) -> u32 {
        crate::ordered_read(|| field_shared!(self.0, line).read())
    }
    fn set_line(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, line).write(v));
    }
    fn set_interrupt_divisor(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, interrupt_divisor).write(v));
    }
    fn set_data_divisor(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, data_divisor).write(v));
    }
    fn data(&mut self) -> u32 {
        crate::ordered_read(|| field!(self.0, data_divisor).read())
    }
    fn set_fifo(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, fifo).write(v));
    }
    fn set_modem(&mut self, v: u32) {
        crate::ordered_write(|| field!(self.0, modem).write(v));
    }
}

struct Driver<I> {
    io: I,
    configured: bool,
}
impl<I: Io> Driver<I> {
    fn configure(&mut self, divisor: NonZeroU16, polls: u32) -> Result<(), Error> {
        self.configured = false;
        let mut idle = false;
        for _ in 0..polls {
            if self.io.status() & 1 == 0 {
                idle = true;
                break;
            }
        }
        if !idle {
            return Err(Error::Timeout);
        }
        // DLAB aliases both data and interrupt registers. Verify it before
        // each alias change; DesignWare can reject LCR writes while busy.
        self.io.set_line(3);
        if self.io.line() != 3 {
            return Err(Error::ConfigurationRejected);
        }
        self.io.set_interrupt_divisor(0);
        self.io.set_line(0x83);
        if self.io.line() != 0x83 {
            return Err(Error::ConfigurationRejected);
        }
        self.io.set_data_divisor(u32::from(divisor.get() & 0xff));
        self.io.set_interrupt_divisor(u32::from(divisor.get() >> 8));
        self.io.set_line(3);
        if self.io.line() != 3 {
            return Err(Error::ConfigurationRejected);
        }
        self.io.set_modem(0); // No automatic flow control or reserved bits.
        self.io.set_fifo(7); // FIFO enable; clear TX and RX; DMA mode zero.
        self.configured = true;
        Ok(())
    }
    fn ready(&self) -> Result<(), Error> {
        if self.configured {
            Ok(())
        } else {
            Err(Error::Unconfigured)
        }
    }
    fn try_write(&mut self, byte: u8) -> Result<bool, Error> {
        self.ready()?;
        if self.io.status() & 2 == 0 {
            return Ok(false);
        }
        self.io.set_data_divisor(u32::from(byte));
        Ok(true)
    }
    fn try_read(&mut self) -> Result<Option<u8>, Error> {
        self.ready()?;
        let status = self.io.line_status();
        let data = if status & 1 != 0 {
            Some(self.io.data() as u8)
        } else {
            None
        };
        if status & 0x1e != 0 {
            return Err(Error::Receive((status & 0x1e) as u8));
        }
        Ok(data)
    }
    fn write(&mut self, byte: u8, polls: u32) -> Result<(), Error> {
        self.ready()?;
        for _ in 0..polls {
            if self.try_write(byte)? {
                return Ok(());
            }
        }
        Err(Error::Timeout)
    }
    fn read(&mut self, polls: u32) -> Result<u8, Error> {
        self.ready()?;
        for _ in 0..polls {
            if let Some(byte) = self.try_read()? {
                return Ok(byte);
            }
        }
        Err(Error::Timeout)
    }
    fn flush(&mut self, polls: u32) -> Result<(), Error> {
        self.ready()?;
        // USR.TFE alone does not prove that the shift register is empty.
        // USR.BUSY also includes RX activity: conservatively wait for both.
        // Avoid reading LSR here, which would discard RX error indications.
        for _ in 0..polls {
            if self.io.status() & 5 == 4 {
                return Ok(());
            }
        }
        Err(Error::Timeout)
    }
}

/// Exclusive UART controller. Failed configuration leaves data operations
/// disabled; timeout never authorizes Linux to take the controller back.
pub struct Uart<'a> {
    driver: Driver<Mmio<'a>>,
}
impl Uart<'static> {
    /// # Safety
    /// `base` must be an aligned, permanently mapped SG2002 UART register
    /// aperture of at least 0x80 bytes. The caller must hold an activated
    /// exclusive whole-controller/pin lease and exclude every other core,
    /// IRQ/DMA handler and alias. Clock/reset/pinmux and absence of DMA users
    /// must already be proved. Do not use this for the Linux rescue console.
    pub unsafe fn from_base(base: NonNull<u8>) -> Self {
        Self {
            driver: Driver {
                io: Mmio(unsafe { UniqueMmioPointer::new(base.cast()) }),
                configured: false,
            },
        }
    }
}
impl Uart<'_> {
    /// Configure 8N1 using the board-validated divisor `clock / (16 * baud)`.
    /// Clears both FIFOs; at most `polls` reads are spent waiting for idle.
    /// The board must keep RX quiescent throughout configuration.
    pub fn configure(&mut self, divisor: NonZeroU16, polls: u32) -> Result<(), Error> {
        self.driver.configure(divisor, polls)
    }
    pub fn try_write(&mut self, byte: u8) -> Result<bool, Error> {
        self.driver.try_write(byte)
    }
    pub fn try_read(&mut self) -> Result<Option<u8>, Error> {
        self.driver.try_read()
    }
    /// Wait for at most `polls` status samples; this is not a wall-clock limit.
    pub fn write(&mut self, byte: u8, polls: u32) -> Result<(), Error> {
        self.driver.write(byte, polls)
    }
    pub fn read(&mut self, polls: u32) -> Result<u8, Error> {
        self.driver.read(polls)
    }
    /// Wait for transmitter idle. Concurrent receive traffic can cause a
    /// conservative timeout; receive-error status is never consumed here.
    pub fn flush(&mut self, polls: u32) -> Result<(), Error> {
        self.driver.flush(polls)
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::{collections::VecDeque, vec, vec::Vec};
    #[derive(Default)]
    struct Fake {
        status: u32,
        lsr: u32,
        lcr: u32,
        reject_line: Option<u32>,
        samples: u32,
        writes: Vec<(&'static str, u32)>,
        received: VecDeque<u8>,
    }
    impl Io for Fake {
        fn status(&mut self) -> u32 {
            self.samples += 1;
            self.status
        }
        fn line_status(&mut self) -> u32 {
            self.samples += 1;
            self.lsr
        }
        fn line(&self) -> u32 {
            self.lcr
        }
        fn set_line(&mut self, v: u32) {
            self.writes.push(("lcr", v));
            if self.reject_line != Some(v) {
                self.lcr = v;
            }
        }
        fn set_interrupt_divisor(&mut self, v: u32) {
            self.writes
                .push((if self.lcr & 0x80 != 0 { "dlh" } else { "ier" }, v));
        }
        fn set_data_divisor(&mut self, v: u32) {
            self.writes
                .push((if self.lcr & 0x80 != 0 { "dll" } else { "thr" }, v));
        }
        fn data(&mut self) -> u32 {
            u32::from(
                self.received
                    .pop_front()
                    .expect("RBR read only with data ready"),
            )
        }
        fn set_fifo(&mut self, v: u32) {
            self.writes.push(("fcr", v));
        }
        fn set_modem(&mut self, v: u32) {
            self.writes.push(("mcr", v));
        }
    }
    fn driver() -> Driver<Fake> {
        Driver {
            io: Fake::default(),
            configured: false,
        }
    }
    #[test]
    fn no_register_access_before_configuration() {
        let mut d = driver();
        assert_eq!(d.read(10), Err(Error::Unconfigured));
        assert_eq!(d.write(1, 10), Err(Error::Unconfigured));
        assert_eq!(d.flush(10), Err(Error::Unconfigured));
        assert_eq!(d.io.samples, 0);
        assert!(d.io.writes.is_empty());
    }
    #[test]
    fn configuration_uses_correct_aliases_and_never_loopback() {
        let mut d = driver();
        d.configure(NonZeroU16::new(0x1234).unwrap(), 1).unwrap();
        assert_eq!(
            d.io.writes,
            vec![
                ("lcr", 3),
                ("ier", 0),
                ("lcr", 0x83),
                ("dll", 0x34),
                ("dlh", 0x12),
                ("lcr", 3),
                ("mcr", 0),
                ("fcr", 7)
            ]
        );
    }
    #[test]
    fn busy_and_zero_budget_never_write_registers() {
        let mut d = driver();
        d.io.status = 1;
        assert_eq!(d.configure(NonZeroU16::MIN, 7), Err(Error::Timeout));
        assert_eq!(d.io.samples, 7);
        assert!(d.io.writes.is_empty());
        assert_eq!(d.configure(NonZeroU16::MIN, 0), Err(Error::Timeout));
        assert_eq!(d.io.samples, 7);
    }
    #[test]
    fn rejected_dlab_never_writes_data_or_interrupt_aliases() {
        let mut d = driver();
        d.io.reject_line = Some(3);
        assert_eq!(
            d.configure(NonZeroU16::MIN, 1),
            Err(Error::ConfigurationRejected)
        );
        assert_eq!(d.io.writes, vec![("lcr", 3)]);
        assert_eq!(d.try_write(1), Err(Error::Unconfigured));
    }
    #[test]
    fn rejected_dlab_set_never_transmits_divisor_as_data() {
        let mut d = driver();
        d.io.reject_line = Some(0x83);
        assert_eq!(
            d.configure(NonZeroU16::MIN, 1),
            Err(Error::ConfigurationRejected)
        );
        assert_eq!(d.io.writes, vec![("lcr", 3), ("ier", 0), ("lcr", 0x83)]);
        assert_eq!(d.try_write(1), Err(Error::Unconfigured));
    }
    #[test]
    fn bounded_transmit_and_receive_handle_backpressure_and_errors() {
        let mut d = driver();
        d.configured = true;
        assert_eq!(d.write(0x42, 4), Err(Error::Timeout));
        assert_eq!(d.io.samples, 4);
        assert!(d.io.writes.is_empty());
        d.io.status = 2;
        d.write(0x42, 1).unwrap();
        assert_eq!(d.io.writes, vec![("thr", 0x42)]);
        assert_eq!(d.read(3), Err(Error::Timeout));
        d.io.lsr = 9;
        d.io.received.push_back(0x55);
        assert_eq!(d.read(1), Err(Error::Receive(8)));
        assert!(d.io.received.is_empty());
        d.io.lsr = 0x81;
        d.io.received.push_back(0x56);
        assert_eq!(d.read(1), Ok(0x56));
    }
    #[test]
    fn flush_waits_for_idle_without_consuming_receive_errors() {
        let mut d = driver();
        d.configured = true;
        d.io.status = 5;
        assert_eq!(d.flush(2), Err(Error::Timeout));
        d.io.status = 4;
        d.io.lsr = 0x0c;
        assert_eq!(d.flush(1), Ok(()));
        assert_eq!(d.try_read(), Err(Error::Receive(0x0c)));
    }
}
