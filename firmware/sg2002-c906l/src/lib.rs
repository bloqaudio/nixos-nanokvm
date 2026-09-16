#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(target_arch = "riscv64")]
use core::arch::asm;
use core::ffi::{c_int, c_void};
#[cfg(test)]
use core::mem::{align_of, size_of};
use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{Ordering, compiler_fence};
use embedded_hal::delay::DelayNs;

#[allow(dead_code)]
mod contract {
    include!(env!("SG2002_C906L_CONTRACT_RS"));
}

mod activation;
#[cfg(any(
    feature = "timer4",
    feature = "timer5",
    feature = "timer6",
    feature = "timer7"
))]
mod dw_apb_timer;
#[cfg(feature = "picoclaw-lcd")]
mod lcd;
#[cfg(feature = "picoclaw-lcd")]
mod lcd_service;
mod rpmsg;

use contract::{
    ABI_MAJOR, ABI_MINOR, ACTIVATION_REQUIRED, ACTIVATION_RESULT_INTERNAL_FAILURE,
    ACTIVATION_STATE_ACTIVE, ACTIVATION_STATE_DORMANT, ACTIVATION_STATE_INITIALIZING,
    DORMANT_CAPABILITIES, EXPECTED_CAPABILITIES, FLAG_MAILBOX_LOCK_FAILURE, FLAG_REQUEST_DROPPED,
    FLAG_RESPONSE_TIMEOUT, FLAG_UNEXPECTED_MAILBOX_CHANNEL, Message, OP_ACTIVATE_LEASES, OP_ERROR,
    OP_GET_ABI, OP_GET_CAPABILITIES, OP_PING, OP_RESPONSE, SERVICE_CONTROL, SHMEM_MAGIC,
    STATE_BOOTING, STATE_FAULT, STATE_RUNNING, STATUS_REGION_ADDRESS, STATUS_SIZE, Status,
};

#[cfg(test)]
use contract::{CACHE_LINE_SIZE, MESSAGE_SIZE};

const HEARTBEAT_TICKS: u32 = 20; // FreeRTOS runs at 200 Hz.

unsafe extern "C" {
    fn c906l_platform_start(
        control_task: extern "C" fn(*mut c_void),
        rpmsg_task: extern "C" fn(*mut c_void),
    ) -> c_int;
    fn c906l_request_receive(word: *mut u64, timeout_ticks: u32) -> c_int;
    fn c906l_response_send(word: u64) -> c_int;
    fn c906l_request_drops() -> u32;
    fn c906l_mailbox_lock_failures() -> u32;
    fn c906l_unexpected_mailbox_events() -> u32;
    fn c906l_ticks() -> u32;
    fn c906l_cache_clean(address: usize, size: usize);
    fn c906l_cache_invalidate(address: usize, size: usize);
    fn c906l_delay(ticks: u32);
    fn c906l_vq_kick_receive(vqid: *mut u32, timeout_ticks: u32) -> c_int;
    fn c906l_vq_notify(vqid: u32) -> c_int;
}

impl Message {
    const fn decode(word: u64) -> Self {
        Self {
            service: word as u8,
            opcode: (word >> 8) as u8,
            sequence: (word >> 16) as u16,
            value: (word >> 32) as u32,
        }
    }

    const fn encode(self) -> u64 {
        (self.service as u64)
            | ((self.opcode as u64) << 8)
            | ((self.sequence as u64) << 16)
            | ((self.value as u64) << 32)
    }

    fn ordinary_response(self, capabilities: u64) -> Self {
        if self.service != SERVICE_CONTROL {
            return Self {
                opcode: OP_ERROR,
                value: 0,
                ..self
            };
        }

        let value = match self.opcode {
            OP_PING => self.value,
            OP_GET_ABI => ((ABI_MAJOR as u32) << 16) | ABI_MINOR as u32,
            OP_GET_CAPABILITIES => capabilities as u32,
            _ => {
                return Self {
                    opcode: OP_ERROR,
                    value: 0,
                    ..self
                };
            }
        };

        Self {
            opcode: self.opcode | OP_RESPONSE,
            value,
            ..self
        }
    }

    const fn activation_response(self, result: u32) -> Self {
        Self {
            opcode: OP_ACTIVATE_LEASES | OP_RESPONSE,
            value: result,
            ..self
        }
    }
}

fn status_ptr() -> *mut Status {
    STATUS_REGION_ADDRESS as *mut Status
}

pub(crate) fn io_fence() {
    compiler_fence(Ordering::SeqCst);
    // SAFETY: this is a memory-ordering instruction with no operands.
    #[cfg(target_arch = "riscv64")]
    unsafe {
        asm!("fence iorw, iorw", options(nostack, preserves_flags))
    };
}

pub(crate) fn clean(address: usize, size: usize) {
    io_fence();
    // SAFETY: callers provide a range wholly within the reserved C906L/Linux
    // shared-memory carveout.
    unsafe { c906l_cache_clean(address, size) };
    io_fence();
}

pub(crate) fn invalidate(address: usize, size: usize) {
    // SAFETY: callers provide a range wholly within the reserved C906L/Linux
    // shared-memory carveout.
    unsafe { c906l_cache_invalidate(address, size) };
    io_fence();
}

fn publish(status: &Status) {
    // SAFETY: the DT reserves this aligned status cacheline exclusively for
    // C906L/Linux communication for the entire firmware lifetime.
    unsafe { write_volatile(status_ptr(), *status) };
    clean(STATUS_REGION_ADDRESS, STATUS_SIZE);
}

#[cfg(all(not(test), target_os = "none"))]
fn zero_status() -> Status {
    Status {
        magic: 0,
        abi_major: 0,
        abi_minor: 0,
        struct_size: 0,
        state: 0,
        generation: 0,
        flags: 0,
        heartbeat: 0,
        capabilities: 0,
        last_request: 0,
        last_response: 0,
        activation_state: ACTIVATION_STATE_INITIALIZING,
        activation_error: 0,
        activation_attempts: 0,
        activation_request_id: 0,
    }
}

fn initial_status() -> Status {
    invalidate(STATUS_REGION_ADDRESS, STATUS_SIZE);
    // SAFETY: the status cacheline is permanently reserved and aligned.
    let previous = unsafe { read_volatile(status_ptr()) };
    let previous_generation = if previous.magic == SHMEM_MAGIC
        && previous.abi_major == ABI_MAJOR
        && previous.struct_size == STATUS_SIZE as u32
    {
        previous.generation
    } else {
        0
    };
    let mut generation = previous_generation.wrapping_add(1);
    if generation == 0 {
        generation = 1;
    }

    Status {
        magic: SHMEM_MAGIC,
        abi_major: ABI_MAJOR,
        abi_minor: ABI_MINOR,
        struct_size: STATUS_SIZE as u32,
        state: STATE_BOOTING,
        generation,
        flags: 0,
        heartbeat: 0,
        capabilities: DORMANT_CAPABILITIES,
        last_request: 0,
        last_response: 0,
        activation_state: ACTIVATION_STATE_INITIALIZING,
        activation_error: 0,
        activation_attempts: 0,
        activation_request_id: 0,
    }
}

fn load_status() -> Status {
    invalidate(STATUS_REGION_ADDRESS, STATUS_SIZE);
    // SAFETY: the status cacheline is permanently reserved and aligned.
    unsafe { read_volatile(status_ptr()) }
}

const fn deadline_reached(now: u32, deadline: u32) -> bool {
    now.wrapping_sub(deadline) as i32 >= 0
}

const fn mailbox_diagnostic_flags(
    request_drops: u32,
    lock_failures: u32,
    unexpected_events: u32,
) -> u32 {
    (if request_drops != 0 {
        FLAG_REQUEST_DROPPED
    } else {
        0
    }) | (if lock_failures != 0 {
        FLAG_MAILBOX_LOCK_FAILURE
    } else {
        0
    }) | (if unexpected_events != 0 {
        FLAG_UNEXPECTED_MAILBOX_CHANNEL
    } else {
        0
    })
}

struct SharedStatusPublisher;

impl activation::StatusPublisher for SharedStatusPublisher {
    fn publish(&mut self, status: &Status) {
        publish(status);
    }
}

extern "C" fn control_task(_argument: *mut c_void) {
    let mut status = load_status();
    status.state = STATE_RUNNING;
    status.activation_state = if ACTIVATION_REQUIRED {
        ACTIVATION_STATE_DORMANT
    } else {
        status.capabilities = EXPECTED_CAPABILITIES;
        ACTIVATION_STATE_ACTIVE
    };
    publish(&status);

    let mut accepted_request = None;
    let mut request_source = activation::SharedRequestSource;
    let mut leases = activation::HardwareLeases;
    let mut publisher = SharedStatusPublisher;
    // SAFETY: xTaskGetTickCount is valid from a running FreeRTOS task.
    let mut next_heartbeat = unsafe { c906l_ticks() }.wrapping_add(HEARTBEAT_TICKS);

    loop {
        let mut request_word = 0_u64;
        // SAFETY: xTaskGetTickCount is valid from a running FreeRTOS task.
        let now = unsafe { c906l_ticks() };
        let wait = if deadline_reached(now, next_heartbeat) {
            0
        } else {
            next_heartbeat.wrapping_sub(now)
        };
        // SAFETY: request_word remains live for the duration of this blocking
        // call, and the C shim writes exactly one u64 on success.
        let received = unsafe { c906l_request_receive(&mut request_word, wait) } == 0;
        if received {
            let request = Message::decode(request_word);
            status.last_request = request_word;
            let response =
                if request.service == SERVICE_CONTROL && request.opcode == OP_ACTIVATE_LEASES {
                    let result = activation::handle(
                        request,
                        &mut status,
                        &mut accepted_request,
                        &mut request_source,
                        &mut leases,
                        &mut publisher,
                    );
                    request.activation_response(result)
                } else {
                    request.ordinary_response(status.capabilities)
                };
            let response_word = response.encode();
            // SAFETY: fixed-width value crossing the audited C ABI.
            if unsafe { c906l_response_send(response_word) } == 0 {
                status.last_response = response_word;
            } else {
                status.flags |= FLAG_RESPONSE_TIMEOUT;
            }
        }

        // SAFETY: xTaskGetTickCount is valid from a running FreeRTOS task.
        let now = unsafe { c906l_ticks() };
        if deadline_reached(now, next_heartbeat) {
            status.heartbeat = status.heartbeat.wrapping_add(1);
            next_heartbeat = now.wrapping_add(HEARTBEAT_TICKS);
        }

        // SAFETY: naturally aligned monotonic u32 counters written with local
        // IRQs masked by the C shim.
        status.flags |= mailbox_diagnostic_flags(
            unsafe { c906l_request_drops() },
            unsafe { c906l_mailbox_lock_failures() },
            unsafe { c906l_unexpected_mailbox_events() },
        );
        #[cfg(feature = "picoclaw-lcd")]
        if lcd_service::faulted() {
            status.flags |= contract::FLAG_PICOCLAW_LCD_FAILED;
        }
        publish(&status);
    }
}

extern "C" fn rpmsg_task(_argument: *mut c_void) {
    rpmsg::run()
}

/// Coarse FreeRTOS-backed delay for portable `embedded-hal` drivers.
///
/// The scheduler tick is 5 ms. Sub-tick delays round up; timing-sensitive
/// SG2002 peripherals receive dedicated hardware-timer implementations.
pub struct FreeRtosDelay;

impl DelayNs for FreeRtosDelay {
    fn delay_ns(&mut self, ns: u32) {
        if ns == 0 {
            return;
        }
        let ticks = ns.saturating_add(4_999_999) / 5_000_000;
        // SAFETY: delaying the current FreeRTOS task is valid after startup.
        unsafe { c906l_delay(ticks.max(1)) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn c906l_rust_main() -> ! {
    // Publish the attach-only resource table before advertising RPMsg.  Lease
    // profiles deliberately initialize and serve RPMsg while still dormant.
    rpmsg::initialize();
    let mut status = initial_status();
    #[cfg(feature = "picoclaw-lcd")]
    lcd_service::initialize_generation(status.generation);
    publish(&status);

    if !activation::publish_manifest(status.generation) {
        status.flags |= contract::FLAG_MANIFEST_FAULT;
        status.state = STATE_FAULT;
        status.activation_error = ACTIVATION_RESULT_INTERNAL_FAILURE as u8;
        publish(&status);
        loop {
            core::hint::spin_loop();
        }
    }

    // SAFETY: both tasks have the exact C ABI and never return. The shim owns
    // their queues and starts the scheduler only after all setup succeeds.
    let _error = unsafe { c906l_platform_start(control_task, rpmsg_task) };

    // SAFETY: platform startup may have recorded a terminal lock failure
    // before the scheduler and control task could publish diagnostics.
    status.flags |= mailbox_diagnostic_flags(0, unsafe { c906l_mailbox_lock_failures() }, 0);
    status.state = STATE_FAULT;
    status.activation_error = ACTIVATION_RESULT_INTERNAL_FAILURE as u8;
    publish(&status);
    loop {
        core::hint::spin_loop();
    }
}

#[cfg(all(not(test), target_os = "none"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo<'_>) -> ! {
    let mut failed = zero_status();
    failed.magic = SHMEM_MAGIC;
    failed.abi_major = ABI_MAJOR;
    failed.abi_minor = ABI_MINOR;
    failed.struct_size = STATUS_SIZE as u32;
    failed.state = STATE_FAULT;
    failed.activation_error = ACTIVATION_RESULT_INTERNAL_FAILURE as u8;
    publish(&failed);
    loop {
        core::hint::spin_loop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_is_one_cacheline() {
        assert_eq!(size_of::<Message>(), MESSAGE_SIZE);
        assert_eq!(size_of::<Status>(), STATUS_SIZE);
        assert_eq!(align_of::<Status>(), CACHE_LINE_SIZE);
    }

    #[test]
    fn message_round_trip() {
        let message = Message {
            service: 3,
            opcode: 0x42,
            sequence: 0x1234,
            value: 0x89ab_cdef,
        };
        assert_eq!(Message::decode(message.encode()), message);
    }

    #[test]
    fn ping_preserves_payload_and_sequence() {
        let request = Message {
            service: SERVICE_CONTROL,
            opcode: OP_PING,
            sequence: 17,
            value: 0xfeed_beef,
        };
        assert_eq!(
            request.ordinary_response(contract::CAP_MAILBOX),
            Message {
                opcode: OP_PING | OP_RESPONSE,
                ..request
            }
        );
    }

    #[test]
    fn unknown_service_is_rejected() {
        let request = Message {
            service: 9,
            opcode: OP_PING,
            sequence: 1,
            value: 2,
        };
        assert_eq!(request.ordinary_response(0).opcode, OP_ERROR);
    }

    #[test]
    fn ordinary_control_operations_remain_available() {
        let abi = Message {
            service: SERVICE_CONTROL,
            opcode: OP_GET_ABI,
            sequence: 7,
            value: 0,
        }
        .ordinary_response(0);
        assert_eq!(abi.opcode, OP_GET_ABI | OP_RESPONSE);
        assert_eq!(abi.sequence, 7);
        assert_eq!(abi.value, ((ABI_MAJOR as u32) << 16) | ABI_MINOR as u32);

        let capabilities = Message {
            service: SERVICE_CONTROL,
            opcode: OP_GET_CAPABILITIES,
            sequence: 8,
            value: 0,
        }
        .ordinary_response(DORMANT_CAPABILITIES);
        assert_eq!(capabilities.opcode, OP_GET_CAPABILITIES | OP_RESPONSE);
        assert_eq!(capabilities.value, DORMANT_CAPABILITIES as u32);
    }

    #[test]
    fn deadline_comparison_survives_tick_wrap() {
        assert!(!deadline_reached(u32::MAX - 2, 1));
        assert!(deadline_reached(1, u32::MAX - 2));
    }

    #[test]
    fn terminal_mailbox_lock_failures_are_published_separately_from_drops() {
        assert_eq!(mailbox_diagnostic_flags(0, 0, 0), 0);
        assert_eq!(mailbox_diagnostic_flags(1, 0, 0), FLAG_REQUEST_DROPPED);
        assert_eq!(mailbox_diagnostic_flags(0, 1, 0), FLAG_MAILBOX_LOCK_FAILURE);
        assert_eq!(
            mailbox_diagnostic_flags(0, 0, 1),
            FLAG_UNEXPECTED_MAILBOX_CHANNEL
        );
        assert_eq!(
            mailbox_diagnostic_flags(1, 1, 1),
            FLAG_REQUEST_DROPPED | FLAG_MAILBOX_LOCK_FAILURE | FLAG_UNEXPECTED_MAILBOX_CHANNEL
        );
    }
}
