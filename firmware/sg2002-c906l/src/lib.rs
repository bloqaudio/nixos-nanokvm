#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(target_arch = "riscv64")]
use core::arch::asm;
use core::ffi::{c_int, c_void};
use core::mem::size_of;
use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{Ordering, compiler_fence};
use embedded_hal::delay::DelayNs;

#[cfg(any(feature = "timer4", test))]
mod timer4;

const ABI_MAJOR: u16 = 1;
const ABI_MINOR: u16 = 0;
const SHMEM_BASE: usize = 0x8ff0_0000;
const SHMEM_MAGIC: u32 = 0x4d56_4b4e; // "NKVM" in little-endian memory.
const HEARTBEAT_TICKS: u32 = 20; // FreeRTOS runs at 200 Hz.

const SERVICE_CONTROL: u8 = 1;
const OP_PING: u8 = 0x01;
const OP_GET_ABI: u8 = 0x02;
const OP_GET_CAPABILITIES: u8 = 0x03;
const OP_RESPONSE: u8 = 0x80;
const OP_ERROR: u8 = 0xff;

const CAP_MAILBOX: u64 = 1 << 0;
const CAP_SHMEM_HEARTBEAT: u64 = 1 << 1;
#[cfg(feature = "timer4")]
const CAP_TIMER4_SELF_TEST: u64 = 1 << 2;

const STATE_BOOTING: u32 = 1;
const STATE_RUNNING: u32 = 2;
const STATE_FAULT: u32 = 3;
const FLAG_RESPONSE_TIMEOUT: u32 = 1 << 0;
const FLAG_REQUEST_DROPPED: u32 = 1 << 1;
#[cfg(feature = "timer4")]
const FLAG_TIMER4_SELF_TEST_FAILED: u32 = 1 << 2;

#[repr(C, align(64))]
#[derive(Clone, Copy)]
struct Status {
    magic: u32,
    abi_major: u16,
    abi_minor: u16,
    struct_size: u32,
    state: u32,
    generation: u32,
    flags: u32,
    heartbeat: u64,
    capabilities: u64,
    last_request: u64,
    last_response: u64,
    reserved: u64,
}

impl Status {
    #[cfg(all(not(test), target_os = "none"))]
    const fn zeroed() -> Self {
        Self {
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
            reserved: 0,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Message {
    service: u8,
    opcode: u8,
    sequence: u16,
    value: u32,
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

    fn response(self, capabilities: u64) -> Self {
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
}

unsafe extern "C" {
    fn c906l_platform_start(task: extern "C" fn(*mut c_void)) -> c_int;
    fn c906l_request_receive(word: *mut u64, timeout_ticks: u32) -> c_int;
    fn c906l_response_send(word: u64) -> c_int;
    fn c906l_request_drops() -> u32;
    fn c906l_ticks() -> u32;
    fn c906l_cache_clean(address: usize, size: usize);
    fn c906l_cache_invalidate(address: usize, size: usize);
    fn c906l_delay(ticks: u32);
}

fn status_ptr() -> *mut Status {
    SHMEM_BASE as *mut Status
}

fn io_fence() {
    compiler_fence(Ordering::SeqCst);
    // SAFETY: this is a memory-ordering instruction with no operands.
    #[cfg(target_arch = "riscv64")]
    unsafe {
        asm!("fence iorw, iorw", options(nostack, preserves_flags))
    };
}

fn publish(status: &Status) {
    // SAFETY: the DT reserves this aligned status cacheline exclusively for
    // C906L/Linux communication for the entire firmware lifetime.
    unsafe {
        write_volatile(status_ptr(), *status);
        c906l_cache_clean(SHMEM_BASE, size_of::<Status>());
    }
    io_fence();
}

fn initial_status() -> Status {
    // SAFETY: same reserved-memory invariant as publish().  Invalidation is
    // required because the SG2002 interconnect is not hardware coherent.
    unsafe {
        c906l_cache_invalidate(SHMEM_BASE, size_of::<Status>());
        io_fence();
        let previous = read_volatile(status_ptr());
        let generation = if previous.magic == SHMEM_MAGIC
            && previous.abi_major == ABI_MAJOR
            && previous.struct_size == size_of::<Status>() as u32
        {
            previous.generation.wrapping_add(1)
        } else {
            1
        };
        Status {
            magic: SHMEM_MAGIC,
            abi_major: ABI_MAJOR,
            abi_minor: ABI_MINOR,
            struct_size: size_of::<Status>() as u32,
            state: STATE_BOOTING,
            generation,
            flags: 0,
            heartbeat: 0,
            capabilities: CAP_MAILBOX | CAP_SHMEM_HEARTBEAT,
            last_request: 0,
            last_response: 0,
            reserved: 0,
        }
    }
}

fn load_status() -> Status {
    // SAFETY: the status cacheline is permanently reserved and aligned.
    unsafe {
        c906l_cache_invalidate(SHMEM_BASE, size_of::<Status>());
        io_fence();
        read_volatile(status_ptr())
    }
}

const fn deadline_reached(now: u32, deadline: u32) -> bool {
    now.wrapping_sub(deadline) as i32 >= 0
}

extern "C" fn control_task(_argument: *mut c_void) {
    let mut status = load_status();
    #[cfg(feature = "timer4")]
    match timer4::self_test() {
        Ok(()) => status.capabilities |= CAP_TIMER4_SELF_TEST,
        Err(_) => status.flags |= FLAG_TIMER4_SELF_TEST_FAILED,
    }
    // SAFETY: xTaskGetTickCount is valid from a running FreeRTOS task.
    let mut next_heartbeat = unsafe { c906l_ticks() }.wrapping_add(HEARTBEAT_TICKS);
    status.state = STATE_RUNNING;
    publish(&status);

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
            let response = Message::decode(request_word).response(status.capabilities);
            let response_word = response.encode();
            status.last_request = request_word;
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

        // SAFETY: atomic read of a monotonic diagnostic counter in the shim.
        if unsafe { c906l_request_drops() } != 0 {
            status.flags |= FLAG_REQUEST_DROPPED;
        }
        publish(&status);
    }
}

/// Coarse FreeRTOS-backed delay for portable `embedded-hal` drivers.
///
/// The scheduler tick is 5 ms. Sub-tick delays round up; timing-sensitive
/// SG2002 peripherals will receive dedicated hardware-timer implementations.
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
    let status = initial_status();
    publish(&status);

    // SAFETY: control_task has the exact C ABI and never returns.  The shim
    // owns its queue and starts the scheduler only after all setup succeeds.
    let _error = unsafe { c906l_platform_start(control_task) };

    let mut failed = status;
    failed.state = STATE_FAULT;
    publish(&failed);
    loop {
        core::hint::spin_loop();
    }
}

#[cfg(all(not(test), target_os = "none"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo<'_>) -> ! {
    let mut failed = Status::zeroed();
    failed.magic = SHMEM_MAGIC;
    failed.abi_major = ABI_MAJOR;
    failed.abi_minor = ABI_MINOR;
    failed.struct_size = size_of::<Status>() as u32;
    failed.state = STATE_FAULT;
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
        assert_eq!(size_of::<Status>(), 64);
        assert_eq!(align_of::<Status>(), 64);
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
            request.response(CAP_MAILBOX),
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
        assert_eq!(request.response(0).opcode, OP_ERROR);
    }

    #[test]
    fn deadline_comparison_survives_tick_wrap() {
        assert!(!deadline_reached(u32::MAX - 2, 1));
        assert!(deadline_reached(1, u32::MAX - 2));
    }
}
