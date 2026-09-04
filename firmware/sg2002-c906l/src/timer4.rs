//! Opt-in SG2002 Timer4 ownership probe.
//!
//! Timer4 is the SoC's zero-based channel 4, which the DesignWare timer IP
//! documents as its one-based Timer5 register group.  Consequently this
//! module touches only the channel at offset 0x050.  In particular it never
//! reads the aggregate EOI register, because doing so would acknowledge
//! interrupts owned by other cores.

#![cfg_attr(all(test, not(feature = "timer4")), allow(dead_code))]

use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{AtomicBool, Ordering};

use super::{c906l_delay, c906l_ticks, deadline_reached, io_fence};

#[cfg(test)]
pub(crate) const IRQ: u32 = 55;

const TIMER_CLOCK_HZ: u32 = 25_000_000;
const TEST_PERIOD_TICKS: u32 = TIMER_CLOCK_HZ / 10; // 100 ms
const TEST_TIMEOUT_RTOS_TICKS: u32 = 100; // 500 ms at the 200 Hz RTOS tick.

const CONTROL_ENABLE: u32 = 1 << 0;
const CONTROL_USER_DEFINED: u32 = 1 << 1;
const CONTROL_INTERRUPT_MASK: u32 = 1 << 2;
const CONTROL_STOPPED_MASKED: u32 = CONTROL_INTERRUPT_MASK;
const CONTROL_RUNNING_UNMASKED: u32 = CONTROL_ENABLE | CONTROL_USER_DEFINED;

const CLK_XTAL_MISC_GATE: u32 = 1 << 14;
const CLK_TIMER4_GATE: u32 = 1 << 13;
const RESET_TIMER_IP_DEASSERTED: u32 = 1 << 13;
const RESET_TIMER4_DEASSERTED: u32 = 1 << 18;
const TIMER4_XTAL_SOURCE_SELECT: u32 = 1 << 4;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Register {
    ClockXtalMisc,
    ClockTimer4,
    ResetTimer,
    TimerSource,
    Timer4Load,
    Timer4Control,
    Timer4Eoi,
}

impl Register {
    const fn address(self) -> usize {
        match self {
            Self::ClockXtalMisc => 0x0300_2000,
            Self::ClockTimer4 => 0x0300_200c,
            Self::ResetTimer => 0x0300_3008,
            Self::TimerSource => 0x0300_01a0,
            Self::Timer4Load => 0x030a_0050,
            Self::Timer4Control => 0x030a_0058,
            Self::Timer4Eoi => 0x030a_005c,
        }
    }
}

/// Typed register access used by both the real driver and host-side fakes.
///
/// Callers cannot construct an arbitrary address or name the timer block's
/// aggregate EOI register through this interface.
pub(crate) trait TimerIo {
    fn read(&mut self, register: Register) -> u32;
    fn write(&mut self, register: Register, value: u32);
}

struct Mmio;

impl TimerIo for Mmio {
    fn read(&mut self, register: Register) -> u32 {
        // SAFETY: Register contains only audited SG2002 MMIO addresses.  The
        // firmware owns Timer4 when this opt-in feature is selected; the four
        // shared clock/reset/source registers are read-only here.
        let value = unsafe { read_volatile(register.address() as *const u32) };
        io_fence();
        value
    }

    fn write(&mut self, register: Register, value: u32) {
        debug_assert!(matches!(
            register,
            Register::Timer4Load | Register::Timer4Control
        ));
        // SAFETY: the assertion above documents the only writable register
        // variants, both belonging exclusively to the leased Timer4 channel.
        unsafe { write_volatile(register.address() as *mut u32, value) };
        io_fence();
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum SelfTestError {
    ClockXtalMiscDisabled,
    ClockTimer4Disabled,
    TimerResetAsserted,
    Timer4ResetAsserted,
    WrongClockSource,
    InterruptRegistration,
    Timeout,
}

struct Timer4<I> {
    io: I,
}

impl<I: TimerIo> Timer4<I> {
    const fn new(io: I) -> Self {
        Self { io }
    }

    /// Check every shared clock/reset/source prerequisite without modifying
    /// any shared register.  The caller must not perform Timer MMIO writes
    /// until this method succeeds.
    fn validate_platform(&mut self) -> Result<(), SelfTestError> {
        if self.io.read(Register::ClockXtalMisc) & CLK_XTAL_MISC_GATE == 0 {
            return Err(SelfTestError::ClockXtalMiscDisabled);
        }
        if self.io.read(Register::ClockTimer4) & CLK_TIMER4_GATE == 0 {
            return Err(SelfTestError::ClockTimer4Disabled);
        }

        let reset = self.io.read(Register::ResetTimer);
        if reset & RESET_TIMER_IP_DEASSERTED == 0 {
            return Err(SelfTestError::TimerResetAsserted);
        }
        if reset & RESET_TIMER4_DEASSERTED == 0 {
            return Err(SelfTestError::Timer4ResetAsserted);
        }
        if self.io.read(Register::TimerSource) & TIMER4_XTAL_SOURCE_SELECT != 0 {
            return Err(SelfTestError::WrongClockSource);
        }
        Ok(())
    }

    fn prepare(&mut self) {
        self.io
            .write(Register::Timer4Control, CONTROL_STOPPED_MASKED);
        let _ = self.io.read(Register::Timer4Eoi);
        self.io.write(Register::Timer4Load, TEST_PERIOD_TICKS);
    }

    fn arm(&mut self) {
        self.io
            .write(Register::Timer4Control, CONTROL_RUNNING_UNMASKED);
    }

    /// The peripheral-level ISR sequence.  PLIC completion happens in the
    /// vendor dispatcher only after the C trampoline returns, so both these
    /// operations necessarily precede completion.
    fn acknowledge_and_stop(&mut self) {
        let _ = self.io.read(Register::Timer4Eoi);
        self.io
            .write(Register::Timer4Control, CONTROL_STOPPED_MASKED);
    }

    fn stop_and_clear(&mut self) {
        self.io
            .write(Register::Timer4Control, CONTROL_STOPPED_MASKED);
        let _ = self.io.read(Register::Timer4Eoi);
    }
}

unsafe extern "C" {
    fn c906l_timer4_irq_install() -> i32;
    fn c906l_timer4_irq_disable();
}

static TIMER4_FIRED: AtomicBool = AtomicBool::new(false);

pub(crate) fn self_test() -> Result<(), SelfTestError> {
    let mut timer = Timer4::new(Mmio);

    // This is deliberately the first operation.  On validation failure no
    // Timer register, PLIC register, or shared clock/reset register is written.
    timer.validate_platform()?;
    TIMER4_FIRED.store(false, Ordering::Release);
    timer.prepare();

    // SAFETY: the C trampoline has a static lifetime, installs exactly IRQ55,
    // and calls c906l_timer4_interrupt with the vendor ISR ABI.
    if unsafe { c906l_timer4_irq_install() } != 0 {
        timer.stop_and_clear();
        return Err(SelfTestError::InterruptRegistration);
    }

    timer.arm();
    // SAFETY: the scheduler is running; both wrappers are valid in task context.
    let deadline = unsafe { c906l_ticks() }.wrapping_add(TEST_TIMEOUT_RTOS_TICKS);
    let passed = loop {
        if TIMER4_FIRED.load(Ordering::Acquire) {
            break true;
        }
        // SAFETY: see above.  Wrapping comparison keeps the deadline valid
        // across a FreeRTOS tick-count wrap.
        if deadline_reached(unsafe { c906l_ticks() }, deadline) {
            break false;
        }
        // SAFETY: one tick is finite and keeps the control task schedulable.
        unsafe { c906l_delay(1) };
    };

    // Close the peripheral source before masking its PLIC input.  If the ISR
    // already ran, this is an idempotent second stop/clear of Timer4 only.
    timer.stop_and_clear();
    // SAFETY: install succeeded above and IRQ is the same fixed IRQ55.
    unsafe { c906l_timer4_irq_disable() };

    if passed {
        Ok(())
    } else {
        Err(SelfTestError::Timeout)
    }
}

/// Called by the C IRQ55 trampoline.  The vendor dispatcher writes the PLIC
/// claim/complete register only after this function and its trampoline return.
#[unsafe(no_mangle)]
pub extern "C" fn c906l_timer4_interrupt() -> i32 {
    let mut timer = Timer4::new(Mmio);
    timer.acknowledge_and_stop();
    TIMER4_FIRED.store(true, Ordering::Release);
    0
}

#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use std::vec::Vec;

    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    enum Operation {
        Read(Register),
        Write(Register, u32),
    }

    struct FakeIo {
        xtal_gate: u32,
        timer_gate: u32,
        reset: u32,
        source: u32,
        operations: Vec<Operation>,
    }

    impl FakeIo {
        fn ready() -> Self {
            Self {
                xtal_gate: CLK_XTAL_MISC_GATE,
                timer_gate: CLK_TIMER4_GATE,
                reset: RESET_TIMER_IP_DEASSERTED | RESET_TIMER4_DEASSERTED,
                source: 0,
                operations: Vec::new(),
            }
        }
    }

    impl TimerIo for FakeIo {
        fn read(&mut self, register: Register) -> u32 {
            self.operations.push(Operation::Read(register));
            match register {
                Register::ClockXtalMisc => self.xtal_gate,
                Register::ClockTimer4 => self.timer_gate,
                Register::ResetTimer => self.reset,
                Register::TimerSource => self.source,
                Register::Timer4Eoi => 1,
                Register::Timer4Load | Register::Timer4Control => 0,
            }
        }

        fn write(&mut self, register: Register, value: u32) {
            self.operations.push(Operation::Write(register, value));
        }
    }

    #[test]
    fn timer4_registers_and_irq_are_exact() {
        assert_eq!(IRQ, 55);
        assert_eq!(Register::ClockXtalMisc.address(), 0x0300_2000);
        assert_eq!(Register::ClockTimer4.address(), 0x0300_200c);
        assert_eq!(Register::ResetTimer.address(), 0x0300_3008);
        assert_eq!(Register::TimerSource.address(), 0x0300_01a0);
        assert_eq!(Register::Timer4Load.address(), 0x030a_0050);
        assert_eq!(Register::Timer4Control.address(), 0x030a_0058);
        assert_eq!(Register::Timer4Eoi.address(), 0x030a_005c);
    }

    #[test]
    fn validates_all_shared_prerequisites_before_first_timer_write() {
        let mut timer = Timer4::new(FakeIo::ready());
        assert_eq!(timer.validate_platform(), Ok(()));
        timer.prepare();

        assert_eq!(
            timer.io.operations,
            [
                Operation::Read(Register::ClockXtalMisc),
                Operation::Read(Register::ClockTimer4),
                Operation::Read(Register::ResetTimer),
                Operation::Read(Register::TimerSource),
                Operation::Write(Register::Timer4Control, CONTROL_STOPPED_MASKED),
                Operation::Read(Register::Timer4Eoi),
                Operation::Write(Register::Timer4Load, 2_500_000),
            ]
        );
    }

    #[test]
    fn failed_validation_performs_no_write() {
        for fault in 0..5 {
            let mut io = FakeIo::ready();
            match fault {
                0 => io.xtal_gate = 0,
                1 => io.timer_gate = 0,
                2 => io.reset &= !RESET_TIMER_IP_DEASSERTED,
                3 => io.reset &= !RESET_TIMER4_DEASSERTED,
                4 => io.source = TIMER4_XTAL_SOURCE_SELECT,
                _ => unreachable!(),
            }
            let mut timer = Timer4::new(io);
            assert!(timer.validate_platform().is_err());
            assert!(
                timer
                    .io
                    .operations
                    .iter()
                    .all(|operation| matches!(operation, Operation::Read(_)))
            );
        }
    }

    #[test]
    fn isr_reads_only_channel_eoi_then_masks_and_disables() {
        let mut timer = Timer4::new(FakeIo::ready());
        timer.acknowledge_and_stop();
        assert_eq!(
            timer.io.operations,
            [
                Operation::Read(Register::Timer4Eoi),
                Operation::Write(Register::Timer4Control, CONTROL_STOPPED_MASKED),
            ]
        );
    }
}
