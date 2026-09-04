//! Opt-in SG2002 DesignWare APB timer ownership probes.
//!
//! Every leased channel is represented by one generated [`TimerConfig`] and
//! one typed `sg2002-pac` channel slice. The PAC deliberately cannot name the
//! timer bank's aggregate EOI register, because reading it would acknowledge
//! interrupts owned by other cores.

use core::ptr::{NonNull, read_volatile};
use core::sync::atomic::{AtomicU32, Ordering};

use sg2002_pac::timer::{CHANNEL_SIZE, TimerChannel};

use super::{c906l_delay, c906l_ticks, deadline_reached, io_fence};
use crate::contract::{
    TIMER4_BANK_ADDRESS, TIMER4_BANK_SIZE, TIMER4_CONTROL_ADDRESS, TIMER4_EOI_ADDRESS, TIMER4_IRQ,
    TIMER4_LOAD_ADDRESS, TIMER4_PRECONDITION_CLOCK_SOURCE_ADDRESS,
    TIMER4_PRECONDITION_CLOCK_SOURCE_EXPECTED, TIMER4_PRECONDITION_CLOCK_SOURCE_MASK,
    TIMER4_PRECONDITION_CLOCK_TIMER4_ADDRESS, TIMER4_PRECONDITION_CLOCK_TIMER4_EXPECTED,
    TIMER4_PRECONDITION_CLOCK_TIMER4_MASK, TIMER4_PRECONDITION_CLOCK_XTAL_MISC_ADDRESS,
    TIMER4_PRECONDITION_CLOCK_XTAL_MISC_EXPECTED, TIMER4_PRECONDITION_CLOCK_XTAL_MISC_MASK,
    TIMER4_PRECONDITION_RESET_TIMER_IP_ADDRESS, TIMER4_PRECONDITION_RESET_TIMER_IP_EXPECTED,
    TIMER4_PRECONDITION_RESET_TIMER_IP_MASK, TIMER4_PRECONDITION_RESET_TIMER4_ADDRESS,
    TIMER4_PRECONDITION_RESET_TIMER4_EXPECTED, TIMER4_PRECONDITION_RESET_TIMER4_MASK,
    TIMER4_TEST_PERIOD_TICKS, TIMER4_TEST_TIMEOUT_RTOS_TICKS,
};

const CONTROL_ENABLE: u32 = 1 << 0;
const CONTROL_USER_DEFINED: u32 = 1 << 1;
const CONTROL_INTERRUPT_MASK: u32 = 1 << 2;
const CONTROL_STOPPED_MASKED: u32 = CONTROL_INTERRUPT_MASK;
const CONTROL_RUNNING_UNMASKED: u32 = CONTROL_ENABLE | CONTROL_USER_DEFINED;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Precondition {
    address: usize,
    mask: u32,
    expected: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct TimerConfig {
    channel: u32,
    irq: u32,
    bank_address: usize,
    bank_size: usize,
    load_address: usize,
    control_address: usize,
    eoi_address: usize,
    clock_xtal_misc: Precondition,
    clock_channel: Precondition,
    reset_timer_ip: Precondition,
    reset_channel: Precondition,
    clock_source: Precondition,
    period_ticks: u32,
    timeout_rtos_ticks: u32,
}

impl TimerConfig {
    const fn fired_mask(self) -> u32 {
        1 << self.channel
    }

    const fn valid(self) -> bool {
        let Some(bank_end) = self.bank_address.checked_add(self.bank_size) else {
            return false;
        };
        let Some(channel_end) = self.load_address.checked_add(CHANNEL_SIZE) else {
            return false;
        };

        self.channel >= 4
            && self.channel <= 7
            && self.irq != 0
            && self.bank_size >= CHANNEL_SIZE
            && self.load_address != 0
            && self.load_address % core::mem::align_of::<u32>() == 0
            && self.load_address >= self.bank_address
            && channel_end <= bank_end
            && self.control_address == self.load_address + 0x08
            && self.eoi_address == self.load_address + 0x0c
            && self.reset_timer_ip.address == self.reset_channel.address
            && self.clock_xtal_misc.mask != 0
            && self.clock_xtal_misc.expected & !self.clock_xtal_misc.mask == 0
            && self.clock_channel.mask != 0
            && self.clock_channel.expected & !self.clock_channel.mask == 0
            && self.reset_timer_ip.mask != 0
            && self.reset_timer_ip.expected & !self.reset_timer_ip.mask == 0
            && self.reset_channel.mask != 0
            && self.reset_channel.expected & !self.reset_channel.mask == 0
            && self.clock_source.mask != 0
            && self.clock_source.expected & !self.clock_source.mask == 0
            && self.period_ticks != 0
            && self.timeout_rtos_ticks != 0
    }
}

pub(crate) const TIMER4: TimerConfig = TimerConfig {
    channel: 4,
    irq: TIMER4_IRQ,
    bank_address: TIMER4_BANK_ADDRESS,
    bank_size: TIMER4_BANK_SIZE,
    load_address: TIMER4_LOAD_ADDRESS,
    control_address: TIMER4_CONTROL_ADDRESS,
    eoi_address: TIMER4_EOI_ADDRESS,
    clock_xtal_misc: Precondition {
        address: TIMER4_PRECONDITION_CLOCK_XTAL_MISC_ADDRESS,
        mask: TIMER4_PRECONDITION_CLOCK_XTAL_MISC_MASK,
        expected: TIMER4_PRECONDITION_CLOCK_XTAL_MISC_EXPECTED,
    },
    clock_channel: Precondition {
        address: TIMER4_PRECONDITION_CLOCK_TIMER4_ADDRESS,
        mask: TIMER4_PRECONDITION_CLOCK_TIMER4_MASK,
        expected: TIMER4_PRECONDITION_CLOCK_TIMER4_EXPECTED,
    },
    reset_timer_ip: Precondition {
        address: TIMER4_PRECONDITION_RESET_TIMER_IP_ADDRESS,
        mask: TIMER4_PRECONDITION_RESET_TIMER_IP_MASK,
        expected: TIMER4_PRECONDITION_RESET_TIMER_IP_EXPECTED,
    },
    reset_channel: Precondition {
        address: TIMER4_PRECONDITION_RESET_TIMER4_ADDRESS,
        mask: TIMER4_PRECONDITION_RESET_TIMER4_MASK,
        expected: TIMER4_PRECONDITION_RESET_TIMER4_EXPECTED,
    },
    clock_source: Precondition {
        address: TIMER4_PRECONDITION_CLOCK_SOURCE_ADDRESS,
        mask: TIMER4_PRECONDITION_CLOCK_SOURCE_MASK,
        expected: TIMER4_PRECONDITION_CLOCK_SOURCE_EXPECTED,
    },
    period_ticks: TIMER4_TEST_PERIOD_TICKS,
    timeout_rtos_ticks: TIMER4_TEST_TIMEOUT_RTOS_TICKS,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Register {
    ClockXtalMisc,
    ClockChannel,
    ResetTimer,
    TimerSource,
    Load,
    Control,
    Eoi,
}

impl Register {
    const fn address(self, config: TimerConfig) -> usize {
        match self {
            Self::ClockXtalMisc => config.clock_xtal_misc.address,
            Self::ClockChannel => config.clock_channel.address,
            Self::ResetTimer => config.reset_timer_ip.address,
            Self::TimerSource => config.clock_source.address,
            Self::Load => config.load_address,
            Self::Control => config.control_address,
            Self::Eoi => config.eoi_address,
        }
    }
}

/// Register access used by both the real driver and host-side fakes.
///
/// Callers cannot construct an arbitrary address or name the timer block's
/// aggregate EOI register through this interface.
trait TimerIo {
    fn read(&mut self, register: Register) -> u32;
    fn write(&mut self, register: Register, value: u32);
}

unsafe extern "C" {
    fn c906l_local_irq_save() -> u8;
    fn c906l_local_irq_restore(restore_irqs: u8);
    fn c906l_timer_irq_install(channel: u32, irq: u32) -> i32;
    fn c906l_timer_irq_disable(channel: u32, irq: u32);
}

struct LocalIrqGuard {
    restore_irqs: u8,
}

impl LocalIrqGuard {
    fn acquire() -> Self {
        // SAFETY: this only clears the local C906L M-mode interrupt-enable bit
        // and returns whether it was previously set.
        Self {
            restore_irqs: unsafe { c906l_local_irq_save() },
        }
    }
}

impl Drop for LocalIrqGuard {
    fn drop(&mut self) {
        // SAFETY: restore exactly the state returned by the paired save call.
        unsafe { c906l_local_irq_restore(self.restore_irqs) };
    }
}

struct Mmio {
    config: TimerConfig,
}

impl Mmio {
    const fn new(config: TimerConfig) -> Self {
        Self { config }
    }

    fn with_channel<T>(&mut self, access: impl FnOnce(&mut TimerChannel<'static>) -> T) -> T {
        // A single channel is accessed from both task and interrupt context.
        // Masking local interrupts makes each short-lived UniqueMmioPointer
        // exclusive on C906L; the exact Linux lease excludes the other core.
        let guard = LocalIrqGuard::acquire();
        // SAFETY: valid() checked this non-zero, aligned, in-bank channel base.
        // The local interrupt guard and Linux lease provide uniqueness until
        // the handle is explicitly dropped below.
        let base = unsafe { NonNull::new_unchecked(self.config.load_address as *mut u8) };
        // SAFETY: the conditions documented above satisfy from_base().
        let mut channel = unsafe { TimerChannel::from_base(base) };
        let result = access(&mut channel);
        io_fence();
        drop(channel);
        drop(guard);
        result
    }

    fn read_shared(&self, register: Register) -> u32 {
        debug_assert!(matches!(
            register,
            Register::ClockXtalMisc
                | Register::ClockChannel
                | Register::ResetTimer
                | Register::TimerSource
        ));
        // SAFETY: these generated, aligned shared prerequisite registers are
        // read-only for C906L. No ownership is claimed and no RMW is performed.
        let value = unsafe { read_volatile(register.address(self.config) as *const u32) };
        io_fence();
        value
    }
}

impl TimerIo for Mmio {
    fn read(&mut self, register: Register) -> u32 {
        match register {
            Register::ClockXtalMisc
            | Register::ClockChannel
            | Register::ResetTimer
            | Register::TimerSource => self.read_shared(register),
            Register::Load => self.with_channel(|channel| channel.load()),
            Register::Control => self.with_channel(|channel| channel.control()),
            Register::Eoi => self.with_channel(TimerChannel::read_eoi),
        }
    }

    fn write(&mut self, register: Register, value: u32) {
        match register {
            Register::Load => self.with_channel(|channel| channel.set_load(value)),
            Register::Control => self.with_channel(|channel| channel.set_control(value)),
            Register::ClockXtalMisc
            | Register::ClockChannel
            | Register::ResetTimer
            | Register::TimerSource
            | Register::Eoi => panic!("attempted write outside leased timer registers"),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum SelfTestError {
    InvalidContract,
    ClockXtalMiscDisabled,
    ClockChannelDisabled,
    TimerResetAsserted,
    ChannelResetAsserted,
    WrongClockSource,
    InterruptRegistration,
    Timeout,
}

struct Timer<I> {
    config: TimerConfig,
    io: I,
}

impl<I: TimerIo> Timer<I> {
    const fn new(config: TimerConfig, io: I) -> Self {
        Self { config, io }
    }

    fn precondition_met(&mut self, register: Register, condition: Precondition) -> bool {
        self.io.read(register) & condition.mask == condition.expected
    }

    /// Check the generated channel shape and every shared prerequisite without
    /// modifying any register. No timer or PLIC write occurs before success.
    fn validate_platform(&mut self) -> Result<(), SelfTestError> {
        if !self.config.valid() {
            return Err(SelfTestError::InvalidContract);
        }
        if !self.precondition_met(Register::ClockXtalMisc, self.config.clock_xtal_misc) {
            return Err(SelfTestError::ClockXtalMiscDisabled);
        }
        if !self.precondition_met(Register::ClockChannel, self.config.clock_channel) {
            return Err(SelfTestError::ClockChannelDisabled);
        }

        let reset = self.io.read(Register::ResetTimer);
        if reset & self.config.reset_timer_ip.mask != self.config.reset_timer_ip.expected {
            return Err(SelfTestError::TimerResetAsserted);
        }
        if reset & self.config.reset_channel.mask != self.config.reset_channel.expected {
            return Err(SelfTestError::ChannelResetAsserted);
        }
        if !self.precondition_met(Register::TimerSource, self.config.clock_source) {
            return Err(SelfTestError::WrongClockSource);
        }
        Ok(())
    }

    fn prepare(&mut self) {
        self.io.write(Register::Control, CONTROL_STOPPED_MASKED);
        let _ = self.io.read(Register::Eoi);
        self.io.write(Register::Load, self.config.period_ticks);
    }

    fn arm(&mut self) {
        self.io.write(Register::Control, CONTROL_RUNNING_UNMASKED);
    }

    /// PLIC completion happens in the vendor dispatcher only after the C
    /// trampoline returns, so both operations precede completion.
    fn acknowledge_and_stop(&mut self) {
        let _ = self.io.read(Register::Eoi);
        self.io.write(Register::Control, CONTROL_STOPPED_MASKED);
    }

    fn stop_and_clear(&mut self) {
        self.io.write(Register::Control, CONTROL_STOPPED_MASKED);
        let _ = self.io.read(Register::Eoi);
    }
}

static TIMERS_FIRED: AtomicU32 = AtomicU32::new(0);

trait TimerRuntime {
    fn reset_fired(&mut self);
    fn fired(&mut self) -> bool;
    fn irq_install(&mut self) -> i32;
    fn irq_disable(&mut self);
    fn ticks(&mut self) -> u32;
    fn delay(&mut self, ticks: u32);
}

struct PlatformRuntime {
    config: TimerConfig,
}

impl PlatformRuntime {
    const fn new(config: TimerConfig) -> Self {
        Self { config }
    }
}

impl TimerRuntime for PlatformRuntime {
    fn reset_fired(&mut self) {
        TIMERS_FIRED.fetch_and(!self.config.fired_mask(), Ordering::Release);
    }

    fn fired(&mut self) -> bool {
        TIMERS_FIRED.load(Ordering::Acquire) & self.config.fired_mask() != 0
    }

    fn irq_install(&mut self) -> i32 {
        // SAFETY: C validates the generated channel/IRQ pair before installing
        // its static trampoline.
        unsafe { c906l_timer_irq_install(self.config.channel, self.config.irq) }
    }

    fn irq_disable(&mut self) {
        // SAFETY: called only after irq_install succeeded for this exact pair.
        unsafe { c906l_timer_irq_disable(self.config.channel, self.config.irq) };
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
    timer: &mut Timer<I>,
    runtime: &mut R,
) -> Result<(), SelfTestError> {
    timer.validate_platform()?;
    runtime.reset_fired();
    timer.prepare();

    if runtime.irq_install() != 0 {
        timer.stop_and_clear();
        return Err(SelfTestError::InterruptRegistration);
    }

    timer.arm();
    let deadline = runtime
        .ticks()
        .wrapping_add(timer.config.timeout_rtos_ticks);
    let mut remaining_polls = timer.config.timeout_rtos_ticks.saturating_add(1);
    let passed = loop {
        if runtime.fired() {
            break true;
        }
        // The deadline is the normal bound. The poll budget independently
        // bounds failure if a broken platform tick stops advancing.
        if deadline_reached(runtime.ticks(), deadline) || remaining_polls == 0 {
            break false;
        }
        runtime.delay(1);
        remaining_polls -= 1;
    };

    // Close the peripheral source before masking its PLIC input. If the ISR
    // ran, this is an idempotent second stop/clear of the same channel only.
    timer.stop_and_clear();
    runtime.irq_disable();

    if passed {
        Ok(())
    } else {
        Err(SelfTestError::Timeout)
    }
}

pub(crate) fn self_test(config: TimerConfig) -> Result<(), SelfTestError> {
    if !config.valid() {
        return Err(SelfTestError::InvalidContract);
    }
    self_test_with(
        &mut Timer::new(config, Mmio::new(config)),
        &mut PlatformRuntime::new(config),
    )
}

fn interrupt_config(channel: u32, irq: u32) -> Option<TimerConfig> {
    if channel == TIMER4.channel && irq == TIMER4.irq {
        Some(TIMER4)
    } else {
        None
    }
}

/// Called by a generated-contract C trampoline. Invalid or unselected pairs
/// are rejected before any timer MMIO is constructed or accessed.
#[unsafe(no_mangle)]
pub extern "C" fn c906l_timer_interrupt(channel: u32, irq: u32) -> i32 {
    let Some(config) = interrupt_config(channel, irq) else {
        return -1;
    };
    if !config.valid() {
        return -1;
    }
    let mut timer = Timer::new(config, Mmio::new(config));
    timer.acknowledge_and_stop();
    TIMERS_FIRED.fetch_or(config.fired_mask(), Ordering::Release);
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
        config: TimerConfig,
        xtal_gate: u32,
        timer_gate: u32,
        reset: u32,
        source: u32,
        operations: Vec<Operation>,
    }

    impl FakeIo {
        fn ready(config: TimerConfig) -> Self {
            Self {
                config,
                xtal_gate: config.clock_xtal_misc.expected,
                timer_gate: config.clock_channel.expected,
                reset: config.reset_timer_ip.expected | config.reset_channel.expected,
                source: config.clock_source.expected,
                operations: Vec::new(),
            }
        }
    }

    impl TimerIo for FakeIo {
        fn read(&mut self, register: Register) -> u32 {
            self.operations.push(Operation::Read(register));
            match register {
                Register::ClockXtalMisc => self.xtal_gate,
                Register::ClockChannel => self.timer_gate,
                Register::ResetTimer => self.reset,
                Register::TimerSource => self.source,
                Register::Eoi => 1,
                Register::Load | Register::Control => 0,
            }
        }

        fn write(&mut self, register: Register, value: u32) {
            assert!(matches!(register, Register::Load | Register::Control));
            assert!(register.address(self.config) >= self.config.load_address);
            self.operations.push(Operation::Write(register, value));
        }
    }

    fn timer4() -> Timer<FakeIo> {
        Timer::new(TIMER4, FakeIo::ready(TIMER4))
    }

    #[test]
    fn timer4_registers_irq_and_channel_slice_are_exact() {
        assert!(TIMER4.valid());
        assert_eq!(TIMER4.channel, 4);
        assert_eq!(TIMER4.irq, 55);
        assert_eq!(Register::ClockXtalMisc.address(TIMER4), 0x0300_2000);
        assert_eq!(Register::ClockChannel.address(TIMER4), 0x0300_200c);
        assert_eq!(Register::ResetTimer.address(TIMER4), 0x0300_3008);
        assert_eq!(Register::TimerSource.address(TIMER4), 0x0300_01a0);
        assert_eq!(Register::Load.address(TIMER4), 0x030a_0050);
        assert_eq!(Register::Control.address(TIMER4), 0x030a_0058);
        assert_eq!(Register::Eoi.address(TIMER4), 0x030a_005c);
        assert_eq!(TIMER4.load_address + CHANNEL_SIZE, 0x030a_0064);
        assert_ne!(Register::Eoi.address(TIMER4), 0x030a_00a4);
    }

    #[test]
    fn invalid_contract_is_rejected_before_mmio() {
        let invalid = TimerConfig {
            eoi_address: TIMER4.eoi_address + 4,
            ..TIMER4
        };
        let mut timer = Timer::new(invalid, FakeIo::ready(invalid));
        let mut runtime = FakeRuntime::default();
        assert_eq!(
            self_test_with(&mut timer, &mut runtime),
            Err(SelfTestError::InvalidContract)
        );
        assert!(timer.io.operations.is_empty());
        assert_eq!(runtime.install_calls, 0);
    }

    #[test]
    fn validates_all_shared_prerequisites_before_first_timer_write() {
        let mut timer = timer4();
        assert_eq!(timer.validate_platform(), Ok(()));
        timer.prepare();

        assert_eq!(
            timer.io.operations,
            [
                Operation::Read(Register::ClockXtalMisc),
                Operation::Read(Register::ClockChannel),
                Operation::Read(Register::ResetTimer),
                Operation::Read(Register::TimerSource),
                Operation::Write(Register::Control, CONTROL_STOPPED_MASKED),
                Operation::Read(Register::Eoi),
                Operation::Write(Register::Load, TIMER4_TEST_PERIOD_TICKS),
            ]
        );
    }

    #[test]
    fn failed_validation_performs_no_write() {
        for fault in 0..5 {
            let mut io = FakeIo::ready(TIMER4);
            match fault {
                0 => io.xtal_gate ^= TIMER4.clock_xtal_misc.mask,
                1 => io.timer_gate ^= TIMER4.clock_channel.mask,
                2 => io.reset ^= TIMER4.reset_timer_ip.mask,
                3 => io.reset ^= TIMER4.reset_channel.mask,
                4 => io.source ^= TIMER4.clock_source.mask,
                _ => unreachable!(),
            }
            let mut timer = Timer::new(TIMER4, io);
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
    fn isr_reads_only_channel_eoi_then_masks() {
        let mut timer = timer4();
        timer.acknowledge_and_stop();
        assert_eq!(
            timer.io.operations,
            [
                Operation::Read(Register::Eoi),
                Operation::Write(Register::Control, CONTROL_STOPPED_MASKED),
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
        let mut io = FakeIo::ready(TIMER4);
        io.xtal_gate ^= TIMER4.clock_xtal_misc.mask;
        let mut timer = Timer::new(TIMER4, io);
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
        let mut timer = timer4();
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
        let mut timer = timer4();
        let mut runtime = FakeRuntime::default();
        assert_eq!(
            self_test_with(&mut timer, &mut runtime),
            Err(SelfTestError::Timeout)
        );
        assert_eq!(runtime.install_calls, 1);
        assert_eq!(runtime.disable_calls, 1);
        assert!(runtime.delay_calls <= TIMER4.timeout_rtos_ticks as usize + 1);
    }

    #[test]
    fn failed_irq_install_stops_timer_without_disabling_unowned_irq() {
        let mut timer = timer4();
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
            Some(&Operation::Read(Register::Eoi))
        );
    }

    #[test]
    fn invalid_or_unselected_interrupt_pair_is_rejected() {
        assert_eq!(interrupt_config(5, 56), None);
        assert_eq!(interrupt_config(TIMER4.channel, TIMER4.irq + 1), None);
    }
}
