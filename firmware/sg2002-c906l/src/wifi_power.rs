//! A single, cacheline-isolated Linux regulator command. GPIOA remains in the
//! LCD task's one OutputGroup; this service never creates another MMIO owner.
use core::ptr::{read_volatile, write_volatile};

use crate::{clean, contract::*, invalidate};

#[repr(C, align(64))]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Record {
    magic: u32,
    generation: u32,
    sequence: u32,
    enabled: u32,
    reserved: [u8; 44],
    commit: u32,
}
const _: [(); 64] = [(); core::mem::size_of::<Record>()];
const _: [(); 60] = [(); core::mem::offset_of!(Record, commit)];

fn process(
    request: Record,
    generation: u32,
    completed: &mut u32,
    mut apply: impl FnMut(bool) -> Result<(), ()>,
) -> Option<Record> {
    if generation == 0
        || request.magic != PICOCLAW_LCD_WIFI_POWER_REQUEST_MAGIC
        || request.generation != generation
        || request.sequence <= *completed
        || request.commit != request.sequence
        || request.enabled > 1
        || request.reserved != [0; 44]
    {
        return None;
    }
    let enabled = if apply(request.enabled != 0).is_ok() {
        request.enabled
    } else {
        u32::MAX
    };
    *completed = request.sequence;
    Some(Record {
        magic: PICOCLAW_LCD_WIFI_POWER_COMPLETION_MAGIC,
        generation,
        sequence: request.sequence,
        enabled,
        reserved: [0; 44],
        commit: 0,
    })
}

pub(crate) fn step(
    generation: u32,
    completed: &mut u32,
    apply: impl FnMut(bool) -> Result<(), ()>,
) {
    let address = PICOCLAW_LCD_WIFI_POWER_OWNERSHIP_ADDRESS;
    invalidate(address, 64);
    // SAFETY: fixed, aligned Linux-written cacheline in the contract's bulk
    // reservation. A generation/sequence commit is mandatory before use.
    let first = unsafe { read_volatile(address as *const Record) };
    invalidate(address, 64);
    // SAFETY: same reserved record; require two identical snapshots.
    let second = unsafe { read_volatile(address as *const Record) };
    if first != second {
        return;
    }
    let Some(response) = process(first, generation, completed, apply) else {
        return;
    };
    let address = address + 64;
    // SAFETY: this separate cacheline is written only by this task.
    unsafe { write_volatile(address as *mut Record, response) };
    clean(address, 64);
    // SAFETY: aligned commit word in the same exclusively owned cacheline.
    unsafe { write_volatile((address + 60) as *mut u32, response.sequence) };
    clean(address, 64);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(sequence: u32, enabled: u32) -> Record {
        Record {
            magic: PICOCLAW_LCD_WIFI_POWER_REQUEST_MAGIC,
            generation: 7,
            sequence,
            enabled,
            reserved: [0; 44],
            commit: sequence,
        }
    }

    #[test]
    fn malformed_and_stale_commands_never_touch_gpio() {
        let good = request(2, 1);
        for bad in [
            Record {
                generation: 6,
                ..good
            },
            Record { magic: 0, ..good },
            Record {
                sequence: 0,
                commit: 0,
                ..good
            },
            Record {
                sequence: 1,
                commit: 1,
                ..good
            },
            Record { enabled: 2, ..good },
            Record { commit: 0, ..good },
            Record {
                reserved: [1; 44],
                ..good
            },
        ] {
            let mut last = 1;
            assert!(process(bad, 7, &mut last, |_| panic!("invalid command executed")).is_none());
            assert_eq!(last, 1);
        }
    }

    #[test]
    fn acknowledged_changes_preserve_lcd_lines_and_replays_do_nothing() {
        let lcd = (1 << 27) | (1 << 28);
        let wifi = 1 << 26;
        let mut latch = lcd;
        let mut last = 0;
        for (sequence, enabled) in [(1, 0), (2, 1), (3, 0)] {
            let request = request(sequence, enabled);
            let response = process(request, 7, &mut last, |on| {
                latch = (latch & !wifi) | if on { wifi } else { 0 };
                Ok(())
            })
            .unwrap();
            assert_eq!(response.enabled, enabled);
            assert_eq!(response.sequence, sequence);
            assert_eq!(response.commit, 0); // publisher commits after cleaning
            assert_eq!(latch & lcd, lcd);
            assert_eq!(latch & wifi != 0, enabled != 0);
            assert!(process(request, 7, &mut last, |_| panic!("replay executed")).is_none());
        }
        assert!(
            process(request(1, 1), 7, &mut last, |_| panic!(
                "old command executed"
            ))
            .is_none()
        );
    }

    #[test]
    fn failed_readback_is_an_error_acknowledgement_not_success_or_retry() {
        let mut last = 0;
        let r = request(1, 1);
        let response = process(r, 7, &mut last, |_| Err(())).unwrap();
        assert_eq!(response.enabled, u32::MAX);
        assert_eq!(last, 1);
        assert!(process(r, 7, &mut last, |_| panic!("failure retried")).is_none());
    }
}
