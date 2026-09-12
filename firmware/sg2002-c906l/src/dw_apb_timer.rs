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
    current_address: usize,
    control_address: usize,
    eoi_address: usize,
    status_address: usize,
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
        let Some(channel_offset) = (self.channel as usize).checked_mul(CHANNEL_SIZE) else {
            return false;
        };
        let Some(expected_load_address) = self.bank_address.checked_add(channel_offset) else {
            return false;
        };

        self.channel >= 4
            && self.channel <= 7
            && self.irq == self.channel + 51
            && self.bank_size >= CHANNEL_SIZE
            && self.load_address != 0
            && self.load_address % core::mem::align_of::<u32>() == 0
            && self.load_address >= self.bank_address
            && self.load_address == expected_load_address
            && channel_end <= bank_end
            && self.current_address == self.load_address + 0x04
            && self.control_address == self.load_address + 0x08
            && self.eoi_address == self.load_address + 0x0c
            && self.status_address == self.load_address + 0x10
            && self.reset_timer_ip.address == self.reset_channel.address
            && self.clock_xtal_misc.address % core::mem::align_of::<u32>() == 0
            && self.clock_channel.address % core::mem::align_of::<u32>() == 0
            && self.reset_timer_ip.address % core::mem::align_of::<u32>() == 0
            && self.clock_source.address % core::mem::align_of::<u32>() == 0
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

macro_rules! timer_config {
    (
        channel: $channel:literal,
        irq: $irq:ident,
        bank: ($bank_address:ident, $bank_size:ident),
        registers: ($load:ident, $current:ident, $control:ident, $eoi:ident, $status:ident),
        clock_xtal_misc: ($xtal_address:ident, $xtal_mask:ident, $xtal_expected:ident),
        clock_channel: ($clock_address:ident, $clock_mask:ident, $clock_expected:ident),
        reset_timer_ip: ($ip_reset_address:ident, $ip_reset_mask:ident, $ip_reset_expected:ident),
        reset_channel: ($channel_reset_address:ident, $channel_reset_mask:ident, $channel_reset_expected:ident),
        clock_source: ($source_address:ident, $source_mask:ident, $source_expected:ident),
        self_test: ($period_ticks:ident, $timeout_ticks:ident) $(,)?
    ) => {
        TimerConfig {
            channel: $channel,
            irq: crate::contract::$irq,
            bank_address: crate::contract::$bank_address,
            bank_size: crate::contract::$bank_size,
            load_address: crate::contract::$load,
            current_address: crate::contract::$current,
            control_address: crate::contract::$control,
            eoi_address: crate::contract::$eoi,
            status_address: crate::contract::$status,
            clock_xtal_misc: Precondition {
                address: crate::contract::$xtal_address,
                mask: crate::contract::$xtal_mask,
                expected: crate::contract::$xtal_expected,
            },
            clock_channel: Precondition {
                address: crate::contract::$clock_address,
                mask: crate::contract::$clock_mask,
                expected: crate::contract::$clock_expected,
            },
            reset_timer_ip: Precondition {
                address: crate::contract::$ip_reset_address,
                mask: crate::contract::$ip_reset_mask,
                expected: crate::contract::$ip_reset_expected,
            },
            reset_channel: Precondition {
                address: crate::contract::$channel_reset_address,
                mask: crate::contract::$channel_reset_mask,
                expected: crate::contract::$channel_reset_expected,
            },
            clock_source: Precondition {
                address: crate::contract::$source_address,
                mask: crate::contract::$source_mask,
                expected: crate::contract::$source_expected,
            },
            period_ticks: crate::contract::$period_ticks,
            timeout_rtos_ticks: crate::contract::$timeout_ticks,
        }
    };
}

#[cfg(feature = "timer4")]
pub(crate) const TIMER4: TimerConfig = timer_config!(
    channel: 4,
    irq: TIMER4_IRQ,
    bank: (TIMER4_BANK_ADDRESS, TIMER4_BANK_SIZE),
    registers: (
        TIMER4_LOAD_ADDRESS,
        TIMER4_CURRENT_ADDRESS,
        TIMER4_CONTROL_ADDRESS,
        TIMER4_EOI_ADDRESS,
        TIMER4_STATUS_ADDRESS
    ),
    clock_xtal_misc: (
        TIMER4_PRECONDITION_CLOCK_XTAL_MISC_ADDRESS,
        TIMER4_PRECONDITION_CLOCK_XTAL_MISC_MASK,
        TIMER4_PRECONDITION_CLOCK_XTAL_MISC_EXPECTED
    ),
    clock_channel: (
        TIMER4_PRECONDITION_CLOCK_TIMER4_ADDRESS,
        TIMER4_PRECONDITION_CLOCK_TIMER4_MASK,
        TIMER4_PRECONDITION_CLOCK_TIMER4_EXPECTED
    ),
    reset_timer_ip: (
        TIMER4_PRECONDITION_RESET_TIMER_IP_ADDRESS,
        TIMER4_PRECONDITION_RESET_TIMER_IP_MASK,
        TIMER4_PRECONDITION_RESET_TIMER_IP_EXPECTED
    ),
    reset_channel: (
        TIMER4_PRECONDITION_RESET_TIMER4_ADDRESS,
        TIMER4_PRECONDITION_RESET_TIMER4_MASK,
        TIMER4_PRECONDITION_RESET_TIMER4_EXPECTED
    ),
    clock_source: (
        TIMER4_PRECONDITION_CLOCK_SOURCE_ADDRESS,
        TIMER4_PRECONDITION_CLOCK_SOURCE_MASK,
        TIMER4_PRECONDITION_CLOCK_SOURCE_EXPECTED
    ),
    self_test: (TIMER4_TEST_PERIOD_TICKS, TIMER4_TEST_TIMEOUT_RTOS_TICKS),
);

#[cfg(feature = "timer5")]
pub(crate) const TIMER5: TimerConfig = timer_config!(
    channel: 5,
    irq: TIMER5_IRQ,
    bank: (TIMER5_BANK_ADDRESS, TIMER5_BANK_SIZE),
    registers: (
        TIMER5_LOAD_ADDRESS,
        TIMER5_CURRENT_ADDRESS,
        TIMER5_CONTROL_ADDRESS,
        TIMER5_EOI_ADDRESS,
        TIMER5_STATUS_ADDRESS
    ),
    clock_xtal_misc: (
        TIMER5_PRECONDITION_CLOCK_XTAL_MISC_ADDRESS,
        TIMER5_PRECONDITION_CLOCK_XTAL_MISC_MASK,
        TIMER5_PRECONDITION_CLOCK_XTAL_MISC_EXPECTED
    ),
    clock_channel: (
        TIMER5_PRECONDITION_CLOCK_TIMER5_ADDRESS,
        TIMER5_PRECONDITION_CLOCK_TIMER5_MASK,
        TIMER5_PRECONDITION_CLOCK_TIMER5_EXPECTED
    ),
    reset_timer_ip: (
        TIMER5_PRECONDITION_RESET_TIMER_IP_ADDRESS,
        TIMER5_PRECONDITION_RESET_TIMER_IP_MASK,
        TIMER5_PRECONDITION_RESET_TIMER_IP_EXPECTED
    ),
    reset_channel: (
        TIMER5_PRECONDITION_RESET_TIMER5_ADDRESS,
        TIMER5_PRECONDITION_RESET_TIMER5_MASK,
        TIMER5_PRECONDITION_RESET_TIMER5_EXPECTED
    ),
    clock_source: (
        TIMER5_PRECONDITION_CLOCK_SOURCE_ADDRESS,
        TIMER5_PRECONDITION_CLOCK_SOURCE_MASK,
        TIMER5_PRECONDITION_CLOCK_SOURCE_EXPECTED
    ),
    self_test: (TIMER5_TEST_PERIOD_TICKS, TIMER5_TEST_TIMEOUT_RTOS_TICKS),
);

#[cfg(feature = "timer6")]
pub(crate) const TIMER6: TimerConfig = timer_config!(
    channel: 6,
    irq: TIMER6_IRQ,
    bank: (TIMER6_BANK_ADDRESS, TIMER6_BANK_SIZE),
    registers: (
        TIMER6_LOAD_ADDRESS,
        TIMER6_CURRENT_ADDRESS,
        TIMER6_CONTROL_ADDRESS,
        TIMER6_EOI_ADDRESS,
        TIMER6_STATUS_ADDRESS
    ),
    clock_xtal_misc: (
        TIMER6_PRECONDITION_CLOCK_XTAL_MISC_ADDRESS,
        TIMER6_PRECONDITION_CLOCK_XTAL_MISC_MASK,
        TIMER6_PRECONDITION_CLOCK_XTAL_MISC_EXPECTED
    ),
    clock_channel: (
        TIMER6_PRECONDITION_CLOCK_TIMER6_ADDRESS,
        TIMER6_PRECONDITION_CLOCK_TIMER6_MASK,
        TIMER6_PRECONDITION_CLOCK_TIMER6_EXPECTED
    ),
    reset_timer_ip: (
        TIMER6_PRECONDITION_RESET_TIMER_IP_ADDRESS,
        TIMER6_PRECONDITION_RESET_TIMER_IP_MASK,
        TIMER6_PRECONDITION_RESET_TIMER_IP_EXPECTED
    ),
    reset_channel: (
        TIMER6_PRECONDITION_RESET_TIMER6_ADDRESS,
        TIMER6_PRECONDITION_RESET_TIMER6_MASK,
        TIMER6_PRECONDITION_RESET_TIMER6_EXPECTED
    ),
    clock_source: (
        TIMER6_PRECONDITION_CLOCK_SOURCE_ADDRESS,
        TIMER6_PRECONDITION_CLOCK_SOURCE_MASK,
        TIMER6_PRECONDITION_CLOCK_SOURCE_EXPECTED
    ),
    self_test: (TIMER6_TEST_PERIOD_TICKS, TIMER6_TEST_TIMEOUT_RTOS_TICKS),
);

#[cfg(feature = "timer7")]
pub(crate) const TIMER7: TimerConfig = timer_config!(
    channel: 7,
    irq: TIMER7_IRQ,
    bank: (TIMER7_BANK_ADDRESS, TIMER7_BANK_SIZE),
    registers: (
        TIMER7_LOAD_ADDRESS,
        TIMER7_CURRENT_ADDRESS,
        TIMER7_CONTROL_ADDRESS,
        TIMER7_EOI_ADDRESS,
        TIMER7_STATUS_ADDRESS
    ),
    clock_xtal_misc: (
        TIMER7_PRECONDITION_CLOCK_XTAL_MISC_ADDRESS,
        TIMER7_PRECONDITION_CLOCK_XTAL_MISC_MASK,
        TIMER7_PRECONDITION_CLOCK_XTAL_MISC_EXPECTED
    ),
    clock_channel: (
        TIMER7_PRECONDITION_CLOCK_TIMER7_ADDRESS,
        TIMER7_PRECONDITION_CLOCK_TIMER7_MASK,
        TIMER7_PRECONDITION_CLOCK_TIMER7_EXPECTED
    ),
    reset_timer_ip: (
        TIMER7_PRECONDITION_RESET_TIMER_IP_ADDRESS,
        TIMER7_PRECONDITION_RESET_TIMER_IP_MASK,
        TIMER7_PRECONDITION_RESET_TIMER_IP_EXPECTED
    ),
    reset_channel: (
        TIMER7_PRECONDITION_RESET_TIMER7_ADDRESS,
        TIMER7_PRECONDITION_RESET_TIMER7_MASK,
        TIMER7_PRECONDITION_RESET_TIMER7_EXPECTED
    ),
    clock_source: (
        TIMER7_PRECONDITION_CLOCK_SOURCE_ADDRESS,
        TIMER7_PRECONDITION_CLOCK_SOURCE_MASK,
        TIMER7_PRECONDITION_CLOCK_SOURCE_EXPECTED
    ),
    self_test: (TIMER7_TEST_PERIOD_TICKS, TIMER7_TEST_TIMEOUT_RTOS_TICKS),
);

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
    #[cfg(feature = "timer4")]
    if channel == TIMER4.channel && irq == TIMER4.irq {
        return Some(TIMER4);
    }
    #[cfg(feature = "timer5")]
    if channel == TIMER5.channel && irq == TIMER5.irq {
        return Some(TIMER5);
    }
    #[cfg(feature = "timer6")]
    if channel == TIMER6.channel && irq == TIMER6.irq {
        return Some(TIMER6);
    }
    #[cfg(feature = "timer7")]
    if channel == TIMER7.channel && irq == TIMER7.irq {
        return Some(TIMER7);
    }
    None
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

    const SELECTED_CONFIGS: &[TimerConfig] = &[
        #[cfg(feature = "timer4")]
        TIMER4,
        #[cfg(feature = "timer5")]
        TIMER5,
        #[cfg(feature = "timer6")]
        TIMER6,
        #[cfg(feature = "timer7")]
        TIMER7,
    ];

    fn test_config() -> TimerConfig {
        SELECTED_CONFIGS[0]
    }

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

    fn test_timer() -> Timer<FakeIo> {
        let config = test_config();
        Timer::new(config, FakeIo::ready(config))
    }

    #[test]
    fn selected_registers_irqs_and_channel_slices_are_exact() {
        for config in SELECTED_CONFIGS {
            let offset = (config.channel - 4) as usize;
            let load = 0x030a_0050 + offset * CHANNEL_SIZE;
            assert!(config.valid());
            assert_eq!(config.irq, config.channel + 51);
            assert_eq!(config.bank_address, 0x030a_0000);
            assert_eq!(config.bank_size, 0x0001_0000);
            assert_eq!(Register::ClockXtalMisc.address(*config), 0x0300_2000);
            assert_eq!(config.clock_xtal_misc.mask, 0x0000_4000);
            assert_eq!(Register::ClockChannel.address(*config), 0x0300_200c);
            assert_eq!(config.clock_channel.mask, 0x0000_2000 << offset);
            assert_eq!(Register::ResetTimer.address(*config), 0x0300_3008);
            assert_eq!(config.reset_timer_ip.mask, 0x0000_2000);
            assert_eq!(config.reset_channel.mask, 0x0004_0000 << offset);
            assert_eq!(Register::TimerSource.address(*config), 0x0300_01a0);
            assert_eq!(config.clock_source.mask, 0x10 << offset);
            assert_eq!(Register::Load.address(*config), load);
            assert_eq!(config.current_address, load + 0x04);
            assert_eq!(Register::Control.address(*config), load + 0x08);
            assert_eq!(Register::Eoi.address(*config), load + 0x0c);
            assert_eq!(config.status_address, load + 0x10);
            assert_eq!(config.load_address + CHANNEL_SIZE, load + 0x14);
            assert_ne!(Register::Eoi.address(*config), 0x030a_00a4);
        }
    }

    #[test]
    fn invalid_contract_is_rejected_before_mmio() {
        let config = test_config();
        let invalid = TimerConfig {
            status_address: config.status_address + 4,
            ..config
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
        let mut timer = test_timer();
        let config = timer.config;
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
                Operation::Write(Register::Load, config.period_ticks),
            ]
        );
    }

    #[test]
    fn failed_validation_performs_no_write() {
        let config = test_config();
        for fault in 0..5 {
            let mut io = FakeIo::ready(config);
            match fault {
                0 => io.xtal_gate ^= config.clock_xtal_misc.mask,
                1 => io.timer_gate ^= config.clock_channel.mask,
                2 => io.reset ^= config.reset_timer_ip.mask,
                3 => io.reset ^= config.reset_channel.mask,
                4 => io.source ^= config.clock_source.mask,
                _ => unreachable!(),
            }
            let mut timer = Timer::new(config, io);
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
        let mut timer = test_timer();
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
        let config = test_config();
        let mut io = FakeIo::ready(config);
        io.xtal_gate ^= config.clock_xtal_misc.mask;
        let mut timer = Timer::new(config, io);
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
        let mut timer = test_timer();
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
        let mut timer = test_timer();
        let timeout_rtos_ticks = timer.config.timeout_rtos_ticks;
        let mut runtime = FakeRuntime::default();
        assert_eq!(
            self_test_with(&mut timer, &mut runtime),
            Err(SelfTestError::Timeout)
        );
        assert_eq!(runtime.install_calls, 1);
        assert_eq!(runtime.disable_calls, 1);
        assert!(runtime.delay_calls <= timeout_rtos_ticks as usize + 1);
    }

    #[test]
    fn failed_irq_install_stops_timer_without_disabling_unowned_irq() {
        let mut timer = test_timer();
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
        assert_eq!(interrupt_config(0, u32::MAX), None);
        for (channel, irq) in [(4, 55), (5, 56), (6, 57), (7, 58)] {
            let expected = SELECTED_CONFIGS
                .iter()
                .copied()
                .find(|config| config.channel == channel);
            assert_eq!(interrupt_config(channel, irq), expected);
            assert_eq!(interrupt_config(channel, irq + 1), None);
        }
    }
}
