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
use crate::contract::{
    TIMER4_CONTROL_ADDRESS, TIMER4_EOI_ADDRESS, TIMER4_LOAD_ADDRESS,
    TIMER4_PRECONDITION_CLOCK_SOURCE_ADDRESS, TIMER4_PRECONDITION_CLOCK_SOURCE_EXPECTED,
    TIMER4_PRECONDITION_CLOCK_SOURCE_MASK, TIMER4_PRECONDITION_CLOCK_TIMER4_ADDRESS,
    TIMER4_PRECONDITION_CLOCK_TIMER4_EXPECTED, TIMER4_PRECONDITION_CLOCK_TIMER4_MASK,
    TIMER4_PRECONDITION_CLOCK_XTAL_MISC_ADDRESS, TIMER4_PRECONDITION_CLOCK_XTAL_MISC_EXPECTED,
    TIMER4_PRECONDITION_CLOCK_XTAL_MISC_MASK, TIMER4_PRECONDITION_RESET_TIMER_IP_ADDRESS,
    TIMER4_PRECONDITION_RESET_TIMER_IP_EXPECTED, TIMER4_PRECONDITION_RESET_TIMER_IP_MASK,
    TIMER4_PRECONDITION_RESET_TIMER4_ADDRESS, TIMER4_PRECONDITION_RESET_TIMER4_EXPECTED,
    TIMER4_PRECONDITION_RESET_TIMER4_MASK, TIMER4_TEST_PERIOD_TICKS,
    TIMER4_TEST_TIMEOUT_RTOS_TICKS,
};

#[cfg(test)]
use crate::contract::TIMER4_IRQ;

const CONTROL_ENABLE: u32 = 1 << 0;
const CONTROL_USER_DEFINED: u32 = 1 << 1;
const CONTROL_INTERRUPT_MASK: u32 = 1 << 2;
const CONTROL_STOPPED_MASKED: u32 = CONTROL_INTERRUPT_MASK;
const CONTROL_RUNNING_UNMASKED: u32 = CONTROL_ENABLE | CONTROL_USER_DEFINED;

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
            Self::ClockXtalMisc => TIMER4_PRECONDITION_CLOCK_XTAL_MISC_ADDRESS,
            Self::ClockTimer4 => TIMER4_PRECONDITION_CLOCK_TIMER4_ADDRESS,
            Self::ResetTimer => TIMER4_PRECONDITION_RESET_TIMER_IP_ADDRESS,
            Self::TimerSource => TIMER4_PRECONDITION_CLOCK_SOURCE_ADDRESS,
            Self::Timer4Load => TIMER4_LOAD_ADDRESS,
            Self::Timer4Control => TIMER4_CONTROL_ADDRESS,
            Self::Timer4Eoi => TIMER4_EOI_ADDRESS,
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
        if self.io.read(Register::ClockXtalMisc) & TIMER4_PRECONDITION_CLOCK_XTAL_MISC_MASK
            != TIMER4_PRECONDITION_CLOCK_XTAL_MISC_EXPECTED
        {
            return Err(SelfTestError::ClockXtalMiscDisabled);
        }
        if self.io.read(Register::ClockTimer4) & TIMER4_PRECONDITION_CLOCK_TIMER4_MASK
            != TIMER4_PRECONDITION_CLOCK_TIMER4_EXPECTED
        {
            return Err(SelfTestError::ClockTimer4Disabled);
        }

        let reset = self.io.read(Register::ResetTimer);
        if reset & TIMER4_PRECONDITION_RESET_TIMER_IP_MASK
            != TIMER4_PRECONDITION_RESET_TIMER_IP_EXPECTED
        {
            return Err(SelfTestError::TimerResetAsserted);
        }
        debug_assert_eq!(
            TIMER4_PRECONDITION_RESET_TIMER4_ADDRESS,
            TIMER4_PRECONDITION_RESET_TIMER_IP_ADDRESS
        );
        if reset & TIMER4_PRECONDITION_RESET_TIMER4_MASK
            != TIMER4_PRECONDITION_RESET_TIMER4_EXPECTED
        {
            return Err(SelfTestError::Timer4ResetAsserted);
        }
        if self.io.read(Register::TimerSource) & TIMER4_PRECONDITION_CLOCK_SOURCE_MASK
            != TIMER4_PRECONDITION_CLOCK_SOURCE_EXPECTED
        {
            return Err(SelfTestError::WrongClockSource);
        }
        Ok(())
    }

    fn prepare(&mut self) {
        self.io
            .write(Register::Timer4Control, CONTROL_STOPPED_MASKED);
        let _ = self.io.read(Register::Timer4Eoi);
        self.io
            .write(Register::Timer4Load, TIMER4_TEST_PERIOD_TICKS);
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

trait TimerRuntime {
    fn reset_fired(&mut self);
    fn fired(&mut self) -> bool;
    fn irq_install(&mut self) -> i32;
    fn irq_disable(&mut self);
    fn ticks(&mut self) -> u32;
    fn delay(&mut self, ticks: u32);
}

struct PlatformRuntime;

impl TimerRuntime for PlatformRuntime {
    fn reset_fired(&mut self) {
        TIMER4_FIRED.store(false, Ordering::Release);
    }

    fn fired(&mut self) -> bool {
        TIMER4_FIRED.load(Ordering::Acquire)
    }

    fn irq_install(&mut self) -> i32 {
        // SAFETY: the C trampoline has static lifetime and installs the one
        // generated-contract IRQ routed to C906L.
        unsafe { c906l_timer4_irq_install() }
    }

    fn irq_disable(&mut self) {
        // SAFETY: called only after irq_install succeeded for this fixed IRQ.
        unsafe { c906l_timer4_irq_disable() };
    }

    fn ticks(&mut self) -> u32 {
        // SAFETY: the scheduler is running in task context.
        unsafe { c906l_ticks() }
    }

    fn delay(&mut self, ticks: u32) {
        // SAFETY: the scheduler is running in task context.
        unsafe { c906l_delay(ticks) };
    }
}

fn self_test_with<I: TimerIo, R: TimerRuntime>(
    timer: &mut Timer4<I>,
    runtime: &mut R,
) -> Result<(), SelfTestError> {
    // This is deliberately the first operation.  On validation failure no
    // Timer register, PLIC register, or shared clock/reset register is written.
    timer.validate_platform()?;
    runtime.reset_fired();
    timer.prepare();

    if runtime.irq_install() != 0 {
        timer.stop_and_clear();
        return Err(SelfTestError::InterruptRegistration);
    }

    timer.arm();
    let deadline = runtime.ticks().wrapping_add(TIMER4_TEST_TIMEOUT_RTOS_TICKS);
    let mut remaining_polls = TIMER4_TEST_TIMEOUT_RTOS_TICKS.saturating_add(1);
    let passed = loop {
        if runtime.fired() {
            break true;
        }
        // The deadline is the normal bound.  The poll budget is an independent
        // fail-safe if a broken platform tick ever stops advancing.
        if deadline_reached(runtime.ticks(), deadline) || remaining_polls == 0 {
            break false;
        }
        runtime.delay(1);
        remaining_polls -= 1;
    };

    // Close the peripheral source before masking its PLIC input.  If the ISR
    // already ran, this is an idempotent second stop/clear of Timer4 only.
    timer.stop_and_clear();
    runtime.irq_disable();

    if passed {
        Ok(())
    } else {
        Err(SelfTestError::Timeout)
    }
}

pub(crate) fn self_test() -> Result<(), SelfTestError> {
    self_test_with(&mut Timer4::new(Mmio), &mut PlatformRuntime)
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
                xtal_gate: TIMER4_PRECONDITION_CLOCK_XTAL_MISC_EXPECTED,
                timer_gate: TIMER4_PRECONDITION_CLOCK_TIMER4_EXPECTED,
                reset: TIMER4_PRECONDITION_RESET_TIMER_IP_EXPECTED
                    | TIMER4_PRECONDITION_RESET_TIMER4_EXPECTED,
                source: TIMER4_PRECONDITION_CLOCK_SOURCE_EXPECTED,
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
        assert_eq!(TIMER4_IRQ, 55);
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
                Operation::Write(Register::Timer4Load, TIMER4_TEST_PERIOD_TICKS),
            ]
        );
    }

    #[test]
    fn failed_validation_performs_no_write() {
        for fault in 0..5 {
            let mut io = FakeIo::ready();
            match fault {
                0 => io.xtal_gate ^= TIMER4_PRECONDITION_CLOCK_XTAL_MISC_MASK,
                1 => io.timer_gate ^= TIMER4_PRECONDITION_CLOCK_TIMER4_MASK,
                2 => io.reset ^= TIMER4_PRECONDITION_RESET_TIMER_IP_MASK,
                3 => io.reset ^= TIMER4_PRECONDITION_RESET_TIMER4_MASK,
                4 => io.source ^= TIMER4_PRECONDITION_CLOCK_SOURCE_MASK,
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

    #[derive(Default)]
    struct FakeRuntime {
        reset_fired_calls: usize,
        fired_calls: usize,
        install_calls: usize,
        disable_calls: usize,
        delay_calls: usize,
        tick: u32,
        install_result: i32,
        fire_after_delays: Option<usize>,
    }

    impl TimerRuntime for FakeRuntime {
        fn reset_fired(&mut self) {
            self.reset_fired_calls += 1;
        }

        fn fired(&mut self) -> bool {
            self.fired_calls += 1;
            self.fire_after_delays
                .is_some_and(|threshold| self.delay_calls >= threshold)
        }

        fn irq_install(&mut self) -> i32 {
            self.install_calls += 1;
            self.install_result
        }

        fn irq_disable(&mut self) {
            self.disable_calls += 1;
        }

        fn ticks(&mut self) -> u32 {
            self.tick
        }

        fn delay(&mut self, ticks: u32) {
            self.delay_calls += 1;
            self.tick = self.tick.wrapping_add(ticks);
        }
    }

    #[test]
    fn failed_precondition_never_touches_plic_or_irq_runtime() {
        let mut io = FakeIo::ready();
        io.xtal_gate ^= TIMER4_PRECONDITION_CLOCK_XTAL_MISC_MASK;
        let mut timer = Timer4::new(io);
        let mut runtime = FakeRuntime::default();
        assert_eq!(
            self_test_with(&mut timer, &mut runtime),
            Err(SelfTestError::ClockXtalMiscDisabled)
        );
        assert_eq!(runtime.reset_fired_calls, 0);
        assert_eq!(runtime.install_calls, 0);
        assert_eq!(runtime.disable_calls, 0);
        assert_eq!(runtime.delay_calls, 0);
        assert!(
            timer
                .io
                .operations
                .iter()
                .all(|operation| matches!(operation, Operation::Read(_)))
        );
    }

    #[test]
    fn self_test_success_has_bounded_mmio_and_balanced_irq_lifetime() {
        let mut timer = Timer4::new(FakeIo::ready());
        let mut runtime = FakeRuntime {
            fire_after_delays: Some(3),
            ..FakeRuntime::default()
        };
        assert_eq!(self_test_with(&mut timer, &mut runtime), Ok(()));
        assert_eq!(runtime.reset_fired_calls, 1);
        assert_eq!(runtime.install_calls, 1);
        assert_eq!(runtime.disable_calls, 1);
        assert_eq!(runtime.delay_calls, 3);
        assert!(timer.io.operations.len() < 16);
    }

    #[test]
    fn self_test_timeout_is_independently_poll_bounded() {
        let mut timer = Timer4::new(FakeIo::ready());
        let mut runtime = FakeRuntime::default();
        assert_eq!(
            self_test_with(&mut timer, &mut runtime),
            Err(SelfTestError::Timeout)
        );
        assert_eq!(runtime.install_calls, 1);
        assert_eq!(runtime.disable_calls, 1);
        assert!(runtime.delay_calls <= TIMER4_TEST_TIMEOUT_RTOS_TICKS as usize + 1);
    }

    #[test]
    fn failed_irq_install_stops_timer_without_disabling_unowned_irq() {
        let mut timer = Timer4::new(FakeIo::ready());
        let mut runtime = FakeRuntime {
            install_result: -1,
            ..FakeRuntime::default()
        };
        assert_eq!(
            self_test_with(&mut timer, &mut runtime),
            Err(SelfTestError::InterruptRegistration)
        );
        assert_eq!(runtime.install_calls, 1);
        assert_eq!(runtime.disable_calls, 0);
        assert_eq!(runtime.delay_calls, 0);
        assert_eq!(
            timer.io.operations.last(),
            Some(&Operation::Read(Register::Timer4Eoi))
        );
    }
}
