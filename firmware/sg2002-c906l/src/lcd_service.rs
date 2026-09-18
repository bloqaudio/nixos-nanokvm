//! Fixed-slot shared framebuffer handoff. Linux owns request cachelines and
//! pixels; C906L owns completion cachelines. Neither writer shares a cacheline.
use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use crate::lcd::{Config, Job, Panel, State};
use crate::{clean, contract::*, invalidate, io_fence};

static AUTHORIZED: AtomicBool = AtomicBool::new(false);
static FAULTED: AtomicBool = AtomicBool::new(false);
static GENERATION: AtomicU32 = AtomicU32::new(0);

// Snapshot for the RPMsg status reply, published by the scanout task after
// every step so the reply never touches the panel or its task's state.
static SNAPSHOT_STATE: AtomicU32 = AtomicU32::new(0);
static SNAPSHOT_FRAMES: AtomicU32 = AtomicU32::new(0);
static SNAPSHOT_COMPLETED: [AtomicU32; 2] = [AtomicU32::new(0), AtomicU32::new(0)];

/// Scanout task body: whole-frame SPI streams run here, below the control
/// and RPMsg task priorities, so they are preempted by mailbox and RPMsg work.
pub(crate) fn run() -> ! {
    let mut service = Service::new();
    loop {
        // SAFETY: this function runs only inside the live FreeRTOS task.
        service.step(unsafe { crate::c906l_ticks() });
        service.publish_snapshot();
        // SAFETY: yielding this task is valid after scheduler startup.
        unsafe { crate::c906l_delay(1) };
    }
}

/// Set once before starting either task. Never invalidate the control task's
/// locally written status cacheline from the LCD/RPMsg task.
pub(crate) fn initialize_generation(generation: u32) {
    GENERATION.store(generation, Ordering::Release);
}

/// Called only after the exact generation-bound Linux activation request.
pub(crate) fn authorize() -> Result<(), ()> {
    if GENERATION.load(Ordering::Acquire) == 0 {
        return Err(());
    }
    for &(address, mask, expected) in PICOCLAW_LCD_SHARED_PRECONDITIONS {
        io_fence();
        // SAFETY: generated, reviewed read-only shared pad/clock preconditions.
        if unsafe { read_volatile(address as *const u32) } & mask != expected {
            return Err(());
        }
    }
    AUTHORIZED.store(true, Ordering::Release);
    Ok(())
}

pub(crate) fn faulted() -> bool {
    FAULTED.load(Ordering::Acquire)
}

#[repr(C, align(64))]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Record {
    magic: u32,
    generation: u32,
    sequence: u32,
    result: u32,
    reserved: [u8; 44],
    commit: u32,
}
const _: [(); 64] = [(); core::mem::size_of::<Record>()];
const _: [(); 60] = [(); core::mem::offset_of!(Record, commit)];

fn valid_request(record: &Record, generation: u32, completed: u32) -> bool {
    record.magic == PICOCLAW_LCD_REQUEST_MAGIC
        && record.generation == generation
        && record.sequence != 0
        && record.sequence != completed
        && record.commit == record.sequence
        && record.result == PICOCLAW_LCD_FRAME_SIZE as u32
        && record.reserved == [0; 44]
}

fn request(slot: usize, generation: u32, completed: u32) -> Option<Record> {
    let address = PICOCLAW_LCD_OWNERSHIP0_ADDRESS + slot * PICOCLAW_LCD_OWNERSHIP_SIZE;
    invalidate(address, 64);
    // SAFETY: slot is 0/1, the contract reserves this complete aligned record.
    let first = unsafe { read_volatile(address as *const Record) };
    invalidate(address, 64);
    // SAFETY: same stable, reserved mapping as above; Linux commits last.
    let second = unsafe { read_volatile(address as *const Record) };
    (first == second && valid_request(&first, generation, completed)).then_some(first)
}

fn complete(slot: usize, generation: u32, sequence: u32, error: u32) {
    let address = PICOCLAW_LCD_OWNERSHIP0_ADDRESS
        + slot * PICOCLAW_LCD_OWNERSHIP_SIZE
        + PICOCLAW_LCD_COMPLETION_OFFSET as usize;
    let record = Record {
        magic: PICOCLAW_LCD_COMPLETION_MAGIC,
        generation,
        sequence,
        result: error,
        reserved: [0; 44],
        commit: 0,
    };
    // SAFETY: exclusively C906L-written cacheline in fixed shared reservation.
    unsafe { write_volatile(address as *mut Record, record) };
    clean(address, 64);
    // SAFETY: final aligned commit word is in that same owned cacheline.
    unsafe { write_volatile((address + 60) as *mut u32, sequence) };
    clean(address, 64);
}

pub(crate) struct Service {
    panel: Option<Panel>,
    initialized: bool,
    generation: u32,
    completed: [u32; 2],
    active: Option<(usize, u32, u32)>,
    job_sequence: u32,
    next_slot: usize,
    frames: u32,
    wifi_completed: u32,
}

impl Service {
    pub(crate) const fn new() -> Self {
        Self {
            panel: None,
            initialized: false,
            generation: 0,
            completed: [0; 2],
            active: None,
            job_sequence: 0,
            next_slot: 0,
            frames: 0,
            wifi_completed: 0,
        }
    }

    pub(crate) fn step(&mut self, now: u32) {
        if FAULTED.load(Ordering::Acquire) {
            return;
        }
        if !AUTHORIZED.load(Ordering::Acquire) {
            return;
        }
        if !self.initialized {
            self.initialized = true;
            self.generation = GENERATION.load(Ordering::Acquire);
            // SAFETY: activation validated all preconditions after Linux
            // exclusively leased SPI1/GPIOA. This is the only constructor,
            // and the handles remain in this one task until whole-board reset.
            let panel = unsafe {
                Panel::new(
                    Config {
                        spi_base: PICOCLAW_LCD_SPI_ADDRESS,
                        gpio_base: PICOCLAW_LCD_GPIO_ADDRESS,
                        divider: PICOCLAW_LCD_SPI_DIVIDER as u16,
                        poll_budget: 100_000,
                        framebuffer_base: PICOCLAW_LCD_FRAME_SLOT0_ADDRESS,
                        framebuffer_stride: PICOCLAW_LCD_FRAME_SLOT_STRIDE as usize,
                    },
                    now,
                )
            };
            match panel {
                Ok(panel) => self.panel = Some(panel),
                Err(_) => {
                    FAULTED.store(true, Ordering::Release);
                    return;
                }
            }
        }
        let Some(panel) = self.panel.as_mut() else {
            return;
        };
        // Run before bounded SPI work, through the same sole GPIOA owner.
        crate::wifi_power::step(self.generation, &mut self.wifi_completed, |enabled| {
            panel.wifi_power(enabled).map_err(|_| ())
        });
        panel.step(now);
        let status = panel.status();
        if let Some((slot, sequence, job_sequence)) = self.active {
            if status.state == State::Fault || status.completed_sequence == job_sequence {
                let error = u32::from(status.state == State::Fault);
                complete(slot, self.generation, sequence, error);
                self.completed[slot] = sequence;
                self.active = None;
                self.next_slot = slot ^ 1;
                if error == 0 {
                    self.frames = self.frames.wrapping_add(1);
                }
            }
        }
        if status.state == State::Fault {
            FAULTED.store(true, Ordering::Release);
            return;
        }
        if self.active.is_some() || status.state != State::Ready {
            return;
        }
        // Expected slot first keeps normal alternating submissions ordered.
        // An absent request never authorizes reads from its pixel buffer.
        for slot in [self.next_slot, self.next_slot ^ 1] {
            let Some(record) = request(slot, self.generation, self.completed[slot]) else {
                continue;
            };
            let address =
                PICOCLAW_LCD_FRAME_SLOT0_ADDRESS + slot * PICOCLAW_LCD_FRAME_SLOT_STRIDE as usize;
            // Linux published all bytes before the commit and cannot reclaim
            // this slot until our completion. Evict previous-generation data.
            invalidate(address, PICOCLAW_LCD_FRAME_SIZE);
            self.job_sequence = self.job_sequence.wrapping_add(1).max(1);
            if panel
                .submit(self.job_sequence, Job::Frame { slot: slot as u8 })
                .is_err()
            {
                complete(slot, self.generation, record.sequence, 1);
                self.completed[slot] = record.sequence;
                FAULTED.store(true, Ordering::Release);
            } else {
                self.active = Some((slot, record.sequence, self.job_sequence));
            }
            break;
        }
    }

    fn publish_snapshot(&self) {
        let state = match self.panel.as_ref().map(Panel::status).map(|s| s.state) {
            None => 0_u32,
            Some(State::Initializing) => 1,
            Some(State::Ready) => 2,
            Some(State::Busy) => 3,
            Some(State::Fault) => 4,
        };
        SNAPSHOT_STATE.store(state, Ordering::Relaxed);
        SNAPSHOT_FRAMES.store(self.frames, Ordering::Relaxed);
        SNAPSHOT_COMPLETED[0].store(self.completed[0], Ordering::Relaxed);
        SNAPSHOT_COMPLETED[1].store(self.completed[1], Ordering::Relaxed);
    }
}

/// Tiny diagnostic reply from the RPMsg task; pixel data never passes
/// through RPMsg and nothing here waits on the scanout task.
pub(crate) fn reply(request: &[u8]) -> [u8; 32] {
    let generation = GENERATION.load(Ordering::Acquire);
    let mut reply = [0_u8; 32];
    reply[..4].copy_from_slice(b"LCS1");
    let valid = request.len() == 8
        && &request[..4] == b"LCQ1"
        && u32::from_le_bytes(request[4..8].try_into().unwrap()) == generation;
    for (offset, value) in [
        (4, generation),
        (8, SNAPSHOT_STATE.load(Ordering::Relaxed)),
        (12, SNAPSHOT_FRAMES.load(Ordering::Relaxed)),
        (16, SNAPSHOT_COMPLETED[0].load(Ordering::Relaxed)),
        (20, SNAPSHOT_COMPLETED[1].load(Ordering::Relaxed)),
        (24, u32::from(faulted())),
        (28, u32::from(!valid)),
    ] {
        reply[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
    reply
}

#[cfg(test)]
mod tests {
    use super::*;
    fn record() -> Record {
        Record {
            magic: PICOCLAW_LCD_REQUEST_MAGIC,
            generation: 7,
            sequence: 1,
            result: PICOCLAW_LCD_FRAME_SIZE as u32,
            reserved: [0; 44],
            commit: 1,
        }
    }
    #[test]
    fn ownership_requires_committed_exact_generation_and_format() {
        let r = record();
        assert!(valid_request(&r, 7, 0));
        assert!(!valid_request(&r, 8, 0));
        assert!(!valid_request(&r, 7, 1));
        for bad in [
            Record { commit: 0, ..r },
            Record { sequence: 0, ..r },
            Record {
                result: 115199,
                ..r
            },
            Record { magic: 0, ..r },
            Record {
                reserved: [1; 44],
                ..r
            },
        ] {
            assert!(!valid_request(&bad, 7, 0));
        }
    }
    #[test]
    fn request_sequence_wrap_is_nonzero_and_per_slot() {
        assert!(valid_request(&record(), 7, u32::MAX));
        assert!(!valid_request(&record(), 7, 1));
    }
}
