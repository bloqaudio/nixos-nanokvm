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
///
/// A frame needs two steps — one to admit the request, one to stream it — so
/// sleeping a whole scheduler tick between steps adds two 5 ms gaps to every
/// frame, which is most of the cost of a 20 ms transfer. Yield without
/// sleeping while the display is working and for a second afterwards, then
/// return to tick sleeps.
///
/// This task still outranks idle, so the spin window starves it for its whole
/// duration: only a panel that stops updating entirely lets the core idle, and
/// anything refreshing at least once a second holds it at 100%. Shortening the
/// window is the lever if that ever matters.
const ACTIVE_SPIN_TICKS: u32 = 200;

pub(crate) fn run() -> ! {
    let mut service = Service::new();
    let mut last_work = 0;
    loop {
        // SAFETY: this function runs only inside the live FreeRTOS task.
        let now = unsafe { crate::c906l_ticks() };
        service.step(now);
        service.publish_snapshot();
        if service.working() {
            last_work = now;
        }
        let ticks = u32::from(now.wrapping_sub(last_work) >= ACTIVE_SPIN_TICKS);
        // SAFETY: yielding this task is valid after scheduler startup.
        unsafe { crate::c906l_delay(ticks) };
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
    x: u16,
    y: u16,
    width: u16,
    height: u16,
    reserved: [u8; PICOCLAW_LCD_RECORD_RESERVED_SIZE],
    commit: u32,
}
const _: [(); 64] = [(); core::mem::size_of::<Record>()];
const _: [(); 60] = [(); core::mem::offset_of!(Record, commit)];
const _: [(); PICOCLAW_LCD_RECORD_RECT_OFFSET as usize] = [(); core::mem::offset_of!(Record, x)];
const _: [(); 8] = [(); PICOCLAW_LCD_RECORD_RECT_SIZE];

/// Requests carry the damaged rectangle and exactly its pixels, packed from
/// the start of the slot. A full-screen update is the rectangle 0,0,240,240;
/// there is no separate whole-frame form.
fn valid_request(record: &Record, generation: u32, completed: u32) -> bool {
    let width = u32::from(record.width);
    let height = u32::from(record.height);
    record.magic == PICOCLAW_LCD_REQUEST_MAGIC
        && record.generation == generation
        && record.sequence != 0
        && record.sequence != completed
        && record.commit == record.sequence
        && width != 0
        && height != 0
        && u32::from(record.x) + width <= PICOCLAW_LCD_WIDTH
        && u32::from(record.y) + height <= PICOCLAW_LCD_HEIGHT
        && record.result == width * height * 2
        && record.reserved == [0; PICOCLAW_LCD_RECORD_RESERVED_SIZE]
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
        x: 0,
        y: 0,
        width: 0,
        height: 0,
        reserved: [0; PICOCLAW_LCD_RECORD_RESERVED_SIZE],
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
            invalidate(
                address,
                usize::from(record.width) * usize::from(record.height) * 2,
            );
            self.job_sequence = self.job_sequence.wrapping_add(1).max(1);
            if panel
                .submit(
                    self.job_sequence,
                    Job::Frame {
                        slot: slot as u8,
                        x: record.x,
                        y: record.y,
                        width: record.width,
                        height: record.height,
                    },
                )
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

    /// True while a frame is in flight or the panel has not settled, so the
    /// task keeps stepping without sleeping.
    fn working(&self) -> bool {
        self.active.is_some()
            || !matches!(
                self.panel.as_ref().map(Panel::status).map(|s| s.state),
                None | Some(State::Ready)
            )
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
            x: 0,
            y: 0,
            width: PICOCLAW_LCD_WIDTH as u16,
            height: PICOCLAW_LCD_HEIGHT as u16,
            reserved: [0; PICOCLAW_LCD_RECORD_RESERVED_SIZE],
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
                reserved: [1; PICOCLAW_LCD_RECORD_RESERVED_SIZE],
                ..r
            },
            // Rectangle must be non-empty, on-screen, and match frameBytes.
            Record { width: 0, ..r },
            Record { height: 0, ..r },
            Record { x: 1, ..r },
            Record { y: 1, ..r },
            Record {
                width: 8,
                height: 4,
                ..r
            },
        ] {
            assert!(!valid_request(&bad, 7, 0));
        }
    }
    #[test]
    fn partial_rectangle_requires_exactly_its_own_pixels() {
        let r = Record {
            x: 16,
            y: 32,
            width: 64,
            height: 8,
            result: 64 * 8 * 2,
            ..record()
        };
        assert!(valid_request(&r, 7, 0));
        assert!(!valid_request(
            &Record {
                result: 64 * 8,
                ..r
            },
            7,
            0
        ));
        assert!(!valid_request(&Record { x: 200, ..r }, 7, 0));
        assert!(!valid_request(&Record { y: 236, ..r }, 7, 0));
    }
    #[test]
    fn request_sequence_wrap_is_nonzero_and_per_slot() {
        assert!(valid_request(&record(), 7, u32::MAX));
        assert!(!valid_request(&record(), 7, 1));
    }
}
