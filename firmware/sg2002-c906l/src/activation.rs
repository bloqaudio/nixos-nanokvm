//! Fail-closed Linux acknowledgement for physical peripheral leases.
//!
//! The activation request cachelines are Linux-owned.  C906L reads two
//! independently invalidated snapshots and accepts only an exact, stable
//! record tied to this boot generation and generated contract.  No leased
//! peripheral function is called until every envelope and record check passes.

use core::mem::size_of;
use core::ptr::{read_volatile, write_volatile};

use crate::contract::{
    ABI_MAJOR, ABI_MINOR, ACTIVATION_REQUEST_ADDRESS, ACTIVATION_REQUEST_COMMIT,
    ACTIVATION_REQUEST_FORMAT_MAJOR, ACTIVATION_REQUEST_FORMAT_MINOR, ACTIVATION_REQUEST_MAGIC,
    ACTIVATION_REQUEST_SIZE, ACTIVATION_RESULT_ABI_MISMATCH,
    ACTIVATION_RESULT_CAPABILITIES_MISMATCH, ACTIVATION_RESULT_DIGEST_MISMATCH,
    ACTIVATION_RESULT_EPOCH_MISMATCH, ACTIVATION_RESULT_INVALID_ENVELOPE,
    ACTIVATION_RESULT_INVALID_STATE, ACTIVATION_RESULT_LEASE_MASK_MISMATCH,
    ACTIVATION_RESULT_MALFORMED_RECORD, ACTIVATION_RESULT_PROFILE_MISMATCH,
    ACTIVATION_RESULT_STALE_GENERATION, ACTIVATION_RESULT_SUCCESS, ACTIVATION_STATE_ACTIVATING,
    ACTIVATION_STATE_ACTIVE, ACTIVATION_STATE_DORMANT, ACTIVATION_STATE_LEASE_FAULT,
    ACTIVATION_STATE_REJECTED, ACTIVATION_STATE_VALIDATING, ActivationRequest, CACHE_LINE_SIZE,
    CAPABILITY_WIRE_WIDTH, CONTRACT_EPOCH, CONTRACT_SHA256, DORMANT_CAPABILITIES,
    EXPECTED_CAPABILITIES, FLAG_ACTIVATION_FAILED, FLAG_ACTIVATION_REJECTED, LEASE_MASK,
    LEASE_WIRE_WIDTH, MANIFEST_ADDRESS, MANIFEST_COMMIT, MANIFEST_FLAGS, MANIFEST_FORMAT_MAJOR,
    MANIFEST_FORMAT_MINOR, MANIFEST_MAGIC, MANIFEST_SIZE, Manifest, Message, PROFILE_ID, Status,
};
use crate::{clean, invalidate, io_fence};

#[cfg(not(feature = "timer4"))]
use crate::contract::ACTIVATION_RESULT_INTERNAL_FAILURE;

const ABI_VERSION: u32 = ((ABI_MAJOR as u32) << 16) | ABI_MINOR as u32;

pub(crate) trait StatusPublisher {
    fn publish(&mut self, status: &Status);
}

pub(crate) trait RequestSource {
    fn read_pair(&mut self) -> (ActivationRequest, ActivationRequest);
}

pub(crate) struct SharedRequestSource;

impl RequestSource for SharedRequestSource {
    fn read_pair(&mut self) -> (ActivationRequest, ActivationRequest) {
        invalidate(ACTIVATION_REQUEST_ADDRESS, ACTIVATION_REQUEST_SIZE);
        // SAFETY: the generated contract places this naturally aligned record
        // wholly inside the reserved status page and assigns it to Linux.
        let first =
            unsafe { read_volatile(ACTIVATION_REQUEST_ADDRESS as *const ActivationRequest) };
        io_fence();
        invalidate(ACTIVATION_REQUEST_ADDRESS, ACTIVATION_REQUEST_SIZE);
        // SAFETY: same fixed generated-contract range as the first snapshot.
        let second =
            unsafe { read_volatile(ACTIVATION_REQUEST_ADDRESS as *const ActivationRequest) };
        io_fence();
        (first, second)
    }
}

#[derive(Clone, Copy)]
pub(crate) struct LeaseFailure {
    result: u32,
    flag: u32,
}

pub(crate) trait LeaseActivator {
    fn activate(&mut self) -> Result<(), LeaseFailure>;
}

pub(crate) struct HardwareLeases;

impl LeaseActivator for HardwareLeases {
    fn activate(&mut self) -> Result<(), LeaseFailure> {
        #[cfg(feature = "timer4")]
        {
            return crate::timer4::self_test().map_err(|error| LeaseFailure {
                result: match error {
                    crate::timer4::SelfTestError::ClockXtalMiscDisabled
                    | crate::timer4::SelfTestError::ClockTimer4Disabled
                    | crate::timer4::SelfTestError::TimerResetAsserted
                    | crate::timer4::SelfTestError::Timer4ResetAsserted
                    | crate::timer4::SelfTestError::WrongClockSource => {
                        crate::contract::ACTIVATION_RESULT_PRECONDITION_FAILED
                    }
                    crate::timer4::SelfTestError::InterruptRegistration => {
                        crate::contract::ACTIVATION_RESULT_IRQ_INSTALL_FAILED
                    }
                    crate::timer4::SelfTestError::Timeout => {
                        crate::contract::ACTIVATION_RESULT_SELF_TEST_TIMEOUT
                    }
                },
                flag: crate::contract::FLAG_TIMER4_SELF_TEST_FAILED,
            });
        }

        #[cfg(not(feature = "timer4"))]
        {
            // A generated base contract has no physical lease and never calls
            // this method.  Keep the impossible path fail-closed.
            Err(LeaseFailure {
                result: ACTIVATION_RESULT_INTERNAL_FAILURE,
                flag: 0,
            })
        }
    }
}

fn request_equal(left: &ActivationRequest, right: &ActivationRequest) -> bool {
    left.magic == right.magic
        && left.format_major == right.format_major
        && left.format_minor == right.format_minor
        && left.struct_size == right.struct_size
        && left.generation == right.generation
        && left.request_id == right.request_id
        && left.contract_epoch == right.contract_epoch
        && left.profile_id == right.profile_id
        && left.abi_version == right.abi_version
        && left.final_capabilities == right.final_capabilities
        && left.lease_mask == right.lease_mask
        && left.contract_sha256 == right.contract_sha256
        && left.reserved == right.reserved
        && left.commit == right.commit
}

fn manifest_equal(left: &Manifest, right: &Manifest) -> bool {
    left.magic == right.magic
        && left.format_major == right.format_major
        && left.format_minor == right.format_minor
        && left.struct_size == right.struct_size
        && left.generation == right.generation
        && left.contract_epoch == right.contract_epoch
        && left.profile_id == right.profile_id
        && left.abi_major == right.abi_major
        && left.abi_minor == right.abi_minor
        && left.capability_width == right.capability_width
        && left.lease_width == right.lease_width
        && left.final_capabilities == right.final_capabilities
        && left.dormant_capabilities == right.dormant_capabilities
        && left.lease_mask == right.lease_mask
        && left.flags == right.flags
        && left.reserved0 == right.reserved0
        && left.contract_sha256 == right.contract_sha256
        && left.reserved1 == right.reserved1
        && left.commit == right.commit
}

fn expected_manifest(generation: u32, commit: u32) -> Manifest {
    Manifest {
        magic: MANIFEST_MAGIC,
        format_major: MANIFEST_FORMAT_MAJOR,
        format_minor: MANIFEST_FORMAT_MINOR,
        struct_size: MANIFEST_SIZE as u32,
        generation,
        contract_epoch: CONTRACT_EPOCH,
        profile_id: PROFILE_ID,
        abi_major: ABI_MAJOR,
        abi_minor: ABI_MINOR,
        capability_width: CAPABILITY_WIRE_WIDTH,
        lease_width: LEASE_WIRE_WIDTH,
        final_capabilities: EXPECTED_CAPABILITIES,
        dormant_capabilities: DORMANT_CAPABILITIES,
        lease_mask: LEASE_MASK,
        flags: MANIFEST_FLAGS,
        reserved0: 0,
        contract_sha256: CONTRACT_SHA256,
        reserved1: [0; 28],
        commit,
    }
}

trait ManifestIo {
    fn write_commit(&mut self, value: u32);
    fn write_record(&mut self, manifest: &Manifest);
    fn clean(&mut self, address: usize, size: usize);
    fn fence(&mut self);
    fn invalidate(&mut self, address: usize, size: usize);
    fn read_record(&mut self) -> Manifest;
}

struct SharedManifestIo;

impl ManifestIo for SharedManifestIo {
    fn write_commit(&mut self, value: u32) {
        let address = MANIFEST_ADDRESS + MANIFEST_SIZE - size_of::<u32>();
        // SAFETY: the generated layout fixes commit to the final aligned u32
        // of the C906L-owned manifest.
        unsafe { write_volatile(address as *mut u32, value) };
    }

    fn write_record(&mut self, manifest: &Manifest) {
        // SAFETY: generated address, size and alignment assertions place the
        // entire C906L-owned record inside reserved shared memory.
        unsafe { write_volatile(MANIFEST_ADDRESS as *mut Manifest, *manifest) };
    }

    fn clean(&mut self, address: usize, size: usize) {
        clean(address, size);
    }

    fn fence(&mut self) {
        io_fence();
    }

    fn invalidate(&mut self, address: usize, size: usize) {
        invalidate(address, size);
    }

    fn read_record(&mut self) -> Manifest {
        // SAFETY: same generated C906L-owned range as write_record().
        unsafe { read_volatile(MANIFEST_ADDRESS as *const Manifest) }
    }
}

fn publish_manifest_with<I: ManifestIo>(io: &mut I, generation: u32) -> bool {
    let uncommitted = expected_manifest(generation, 0);
    let expected = expected_manifest(generation, MANIFEST_COMMIT);
    let last_line = MANIFEST_ADDRESS + MANIFEST_SIZE - CACHE_LINE_SIZE;

    // Invalidate any manifest from an earlier boot before replacing its body.
    io.write_commit(0);
    io.clean(last_line, CACHE_LINE_SIZE);
    io.fence();

    io.write_record(&uncommitted);
    io.clean(MANIFEST_ADDRESS, MANIFEST_SIZE);
    io.fence();

    // Commit is the final store and the final cache-clean publication.
    io.write_commit(MANIFEST_COMMIT);
    io.clean(last_line, CACHE_LINE_SIZE);
    io.fence();

    io.invalidate(MANIFEST_ADDRESS, MANIFEST_SIZE);
    io.fence();
    manifest_equal(&io.read_record(), &expected)
}

pub(crate) fn publish_manifest(generation: u32) -> bool {
    publish_manifest_with(&mut SharedManifestIo, generation)
}

fn validate_record(request: &ActivationRequest, generation: u32) -> Result<(), u32> {
    if request.magic != ACTIVATION_REQUEST_MAGIC
        || request.format_major != ACTIVATION_REQUEST_FORMAT_MAJOR
        || request.format_minor != ACTIVATION_REQUEST_FORMAT_MINOR
        || request.struct_size != ACTIVATION_REQUEST_SIZE as u32
        || request.request_id == 0
        || request.reserved != [0; 44]
        || request.commit != ACTIVATION_REQUEST_COMMIT
    {
        return Err(ACTIVATION_RESULT_MALFORMED_RECORD);
    }
    if request.generation != generation {
        return Err(ACTIVATION_RESULT_STALE_GENERATION);
    }
    if request.abi_version != ABI_VERSION {
        return Err(ACTIVATION_RESULT_ABI_MISMATCH);
    }
    if request.contract_epoch != CONTRACT_EPOCH {
        return Err(ACTIVATION_RESULT_EPOCH_MISMATCH);
    }
    if request.profile_id != PROFILE_ID {
        return Err(ACTIVATION_RESULT_PROFILE_MISMATCH);
    }
    if request.contract_sha256 != CONTRACT_SHA256 {
        return Err(ACTIVATION_RESULT_DIGEST_MISMATCH);
    }
    if request.final_capabilities != EXPECTED_CAPABILITIES {
        return Err(ACTIVATION_RESULT_CAPABILITIES_MISMATCH);
    }
    if request.lease_mask != LEASE_MASK {
        return Err(ACTIVATION_RESULT_LEASE_MASK_MISMATCH);
    }
    Ok(())
}

fn reject<P: StatusPublisher>(status: &mut Status, result: u32, publisher: &mut P) -> u32 {
    status.activation_state = ACTIVATION_STATE_REJECTED;
    status.activation_error = result as u8;
    status.capabilities = DORMANT_CAPABILITIES;
    status.flags |= FLAG_ACTIVATION_REJECTED;
    publisher.publish(status);
    result
}

pub(crate) fn handle<R, L, P>(
    envelope: Message,
    status: &mut Status,
    accepted_request: &mut Option<ActivationRequest>,
    request_source: &mut R,
    leases: &mut L,
    publisher: &mut P,
) -> u32
where
    R: RequestSource,
    L: LeaseActivator,
    P: StatusPublisher,
{
    if status.activation_state == ACTIVATION_STATE_LEASE_FAULT {
        return ACTIVATION_RESULT_INVALID_STATE;
    }

    if status.activation_state == ACTIVATION_STATE_ACTIVE {
        let Some(accepted) = accepted_request.as_ref() else {
            return ACTIVATION_RESULT_INVALID_STATE;
        };
        if envelope.value == 0 || envelope.sequence != envelope.value as u16 {
            return ACTIVATION_RESULT_INVALID_STATE;
        }
        let (first, second) = request_source.read_pair();
        if !request_equal(&first, &second)
            || !request_equal(&first, accepted)
            || first.request_id != envelope.value
        {
            return ACTIVATION_RESULT_INVALID_STATE;
        }
        return ACTIVATION_RESULT_SUCCESS;
    }

    if status.activation_state != ACTIVATION_STATE_DORMANT
        && status.activation_state != ACTIVATION_STATE_REJECTED
    {
        return ACTIVATION_RESULT_INVALID_STATE;
    }

    status.activation_attempts = status.activation_attempts.saturating_add(1);
    status.activation_request_id = envelope.value;
    if envelope.value == 0 || envelope.sequence != envelope.value as u16 {
        return reject(status, ACTIVATION_RESULT_INVALID_ENVELOPE, publisher);
    }

    status.activation_state = ACTIVATION_STATE_VALIDATING;
    status.activation_error = ACTIVATION_RESULT_SUCCESS as u8;
    publisher.publish(status);

    let (first, second) = request_source.read_pair();
    if !request_equal(&first, &second) {
        return reject(status, ACTIVATION_RESULT_MALFORMED_RECORD, publisher);
    }
    if let Err(result) = validate_record(&first, status.generation) {
        return reject(status, result, publisher);
    }
    if first.request_id != envelope.value {
        return reject(status, ACTIVATION_RESULT_INVALID_ENVELOPE, publisher);
    }

    status.activation_request_id = first.request_id;
    status.activation_state = ACTIVATION_STATE_ACTIVATING;
    publisher.publish(status);

    match leases.activate() {
        Ok(()) => {
            *accepted_request = Some(first);
            status.capabilities = EXPECTED_CAPABILITIES;
            status.activation_state = ACTIVATION_STATE_ACTIVE;
            status.activation_error = ACTIVATION_RESULT_SUCCESS as u8;
            publisher.publish(status);
            ACTIVATION_RESULT_SUCCESS
        }
        Err(failure) => {
            status.capabilities = DORMANT_CAPABILITIES;
            status.activation_state = ACTIVATION_STATE_LEASE_FAULT;
            status.activation_error = failure.result as u8;
            status.flags |= FLAG_ACTIVATION_FAILED | failure.flag;
            publisher.publish(status);
            failure.result
        }
    }
}

#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use std::vec::Vec;

    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    enum ManifestOperation {
        Commit(u32),
        Record,
        Clean(usize, usize),
        Fence,
        Invalidate(usize, usize),
        Read,
    }

    struct FakeManifestIo {
        record: Manifest,
        operations: Vec<ManifestOperation>,
        corrupt_readback: bool,
    }

    impl FakeManifestIo {
        fn new() -> Self {
            Self {
                record: expected_manifest(1, 0),
                operations: Vec::new(),
                corrupt_readback: false,
            }
        }
    }

    impl ManifestIo for FakeManifestIo {
        fn write_commit(&mut self, value: u32) {
            self.operations.push(ManifestOperation::Commit(value));
            self.record.commit = value;
        }

        fn write_record(&mut self, manifest: &Manifest) {
            self.operations.push(ManifestOperation::Record);
            self.record = *manifest;
        }

        fn clean(&mut self, address: usize, size: usize) {
            self.operations
                .push(ManifestOperation::Clean(address, size));
        }

        fn fence(&mut self) {
            self.operations.push(ManifestOperation::Fence);
        }

        fn invalidate(&mut self, address: usize, size: usize) {
            self.operations
                .push(ManifestOperation::Invalidate(address, size));
        }

        fn read_record(&mut self) -> Manifest {
            self.operations.push(ManifestOperation::Read);
            let mut record = self.record;
            if self.corrupt_readback {
                record.contract_sha256[0] ^= 1;
            }
            record
        }
    }

    #[test]
    fn manifest_body_is_cleaned_before_commit_last() {
        let mut io = FakeManifestIo::new();
        assert!(publish_manifest_with(&mut io, 7));
        let last_line = MANIFEST_ADDRESS + MANIFEST_SIZE - CACHE_LINE_SIZE;
        assert_eq!(
            io.operations,
            [
                ManifestOperation::Commit(0),
                ManifestOperation::Clean(last_line, CACHE_LINE_SIZE),
                ManifestOperation::Fence,
                ManifestOperation::Record,
                ManifestOperation::Clean(MANIFEST_ADDRESS, MANIFEST_SIZE),
                ManifestOperation::Fence,
                ManifestOperation::Commit(MANIFEST_COMMIT),
                ManifestOperation::Clean(last_line, CACHE_LINE_SIZE),
                ManifestOperation::Fence,
                ManifestOperation::Invalidate(MANIFEST_ADDRESS, MANIFEST_SIZE),
                ManifestOperation::Fence,
                ManifestOperation::Read,
            ]
        );
        assert_eq!(io.record.generation, 7);
        assert_eq!(io.record.commit, MANIFEST_COMMIT);
        assert_eq!(io.record.contract_sha256, CONTRACT_SHA256);
        assert_eq!(io.record.reserved0, 0);
        assert_eq!(io.record.reserved1, [0; 28]);
    }

    #[test]
    fn manifest_readback_mismatch_fails_closed() {
        let mut io = FakeManifestIo::new();
        io.corrupt_readback = true;
        assert!(!publish_manifest_with(&mut io, 7));
    }

    #[cfg(feature = "timer4")]
    mod leased {
        use super::*;
        use crate::contract::{
            ACTIVATION_RESULT_SELF_TEST_FAILED, ACTIVATION_STATE_INITIALIZING, STATE_RUNNING,
        };

        struct FakeRequestSource {
            first: ActivationRequest,
            second: ActivationRequest,
            reads: usize,
        }

        impl RequestSource for FakeRequestSource {
            fn read_pair(&mut self) -> (ActivationRequest, ActivationRequest) {
                self.reads += 1;
                (self.first, self.second)
            }
        }

        #[derive(Default)]
        struct FakeTimerLease {
            mmio_accesses: usize,
            plic_installs: usize,
            irq_actions: usize,
            failure: Option<LeaseFailure>,
        }

        impl LeaseActivator for FakeTimerLease {
            fn activate(&mut self) -> Result<(), LeaseFailure> {
                self.mmio_accesses += 1;
                self.plic_installs += 1;
                self.irq_actions += 1;
                match self.failure {
                    Some(failure) => Err(failure),
                    None => Ok(()),
                }
            }
        }

        #[derive(Default)]
        struct FakePublisher {
            snapshots: Vec<Status>,
        }

        impl StatusPublisher for FakePublisher {
            fn publish(&mut self, status: &Status) {
                self.snapshots.push(*status);
            }
        }

        fn dormant_status() -> Status {
            Status {
                magic: crate::contract::SHMEM_MAGIC,
                abi_major: ABI_MAJOR,
                abi_minor: ABI_MINOR,
                struct_size: size_of::<Status>() as u32,
                state: STATE_RUNNING,
                generation: 9,
                flags: 0,
                heartbeat: 0,
                capabilities: DORMANT_CAPABILITIES,
                last_request: 0,
                last_response: 0,
                activation_state: ACTIVATION_STATE_DORMANT,
                activation_error: 0,
                activation_attempts: 0,
                activation_request_id: 0,
            }
        }

        fn valid_request(id: u32) -> ActivationRequest {
            ActivationRequest {
                magic: ACTIVATION_REQUEST_MAGIC,
                format_major: ACTIVATION_REQUEST_FORMAT_MAJOR,
                format_minor: ACTIVATION_REQUEST_FORMAT_MINOR,
                struct_size: ACTIVATION_REQUEST_SIZE as u32,
                generation: 9,
                request_id: id,
                contract_epoch: CONTRACT_EPOCH,
                profile_id: PROFILE_ID,
                abi_version: ABI_VERSION,
                final_capabilities: EXPECTED_CAPABILITIES,
                lease_mask: LEASE_MASK,
                contract_sha256: CONTRACT_SHA256,
                reserved: [0; 44],
                commit: ACTIVATION_REQUEST_COMMIT,
            }
        }

        fn source(request: ActivationRequest) -> FakeRequestSource {
            FakeRequestSource {
                first: request,
                second: request,
                reads: 0,
            }
        }

        fn envelope(id: u32) -> Message {
            Message {
                service: crate::contract::SERVICE_CONTROL,
                opcode: crate::contract::OP_ACTIVATE_LEASES,
                sequence: id as u16,
                value: id,
            }
        }

        fn run_rejected_case(
            envelope: Message,
            first: ActivationRequest,
            second: ActivationRequest,
            expected: u32,
        ) {
            let mut status = dormant_status();
            let mut accepted = None;
            let mut source = FakeRequestSource {
                first,
                second,
                reads: 0,
            };
            let mut timer = FakeTimerLease::default();
            let mut publisher = FakePublisher::default();
            assert_eq!(
                handle(
                    envelope,
                    &mut status,
                    &mut accepted,
                    &mut source,
                    &mut timer,
                    &mut publisher,
                ),
                expected
            );
            assert_eq!(timer.mmio_accesses, 0);
            assert_eq!(timer.plic_installs, 0);
            assert_eq!(timer.irq_actions, 0);
            assert_eq!(status.state, STATE_RUNNING);
            assert_eq!(status.capabilities, DORMANT_CAPABILITIES);
            assert_eq!(status.activation_state, ACTIVATION_STATE_REJECTED);
            assert_eq!(status.activation_error, expected as u8);
            assert_ne!(status.flags & FLAG_ACTIVATION_REJECTED, 0);
        }

        #[test]
        fn no_timer_mmio_plic_or_irq_before_exact_ack() {
            let good = valid_request(0x1234_5678);

            let mut zero = envelope(0);
            zero.sequence = 0;
            run_rejected_case(zero, good, good, ACTIVATION_RESULT_INVALID_ENVELOPE);

            let mut bad_sequence = envelope(good.request_id);
            bad_sequence.sequence ^= 1;
            run_rejected_case(bad_sequence, good, good, ACTIVATION_RESULT_INVALID_ENVELOPE);

            let mut torn = good;
            torn.generation += 1;
            run_rejected_case(
                envelope(good.request_id),
                good,
                torn,
                ACTIVATION_RESULT_MALFORMED_RECORD,
            );

            let mut cases = [
                (good, ACTIVATION_RESULT_MALFORMED_RECORD),
                (good, ACTIVATION_RESULT_STALE_GENERATION),
                (good, ACTIVATION_RESULT_ABI_MISMATCH),
                (good, ACTIVATION_RESULT_EPOCH_MISMATCH),
                (good, ACTIVATION_RESULT_PROFILE_MISMATCH),
                (good, ACTIVATION_RESULT_DIGEST_MISMATCH),
                (good, ACTIVATION_RESULT_CAPABILITIES_MISMATCH),
                (good, ACTIVATION_RESULT_LEASE_MASK_MISMATCH),
            ];
            cases[0].0.reserved[17] = 1;
            cases[1].0.generation += 1;
            cases[2].0.abi_version ^= 1;
            cases[3].0.contract_epoch += 1;
            cases[4].0.profile_id += 1;
            cases[5].0.contract_sha256[31] ^= 1;
            cases[6].0.final_capabilities ^= 1;
            cases[7].0.lease_mask ^= 1;
            for (request, expected) in cases {
                run_rejected_case(envelope(request.request_id), request, request, expected);
            }

            let mut bad_commit = good;
            bad_commit.commit = 0;
            run_rejected_case(
                envelope(good.request_id),
                bad_commit,
                bad_commit,
                ACTIVATION_RESULT_MALFORMED_RECORD,
            );

            let mut zero_record_id = good;
            zero_record_id.request_id = 0;
            run_rejected_case(
                envelope(good.request_id),
                zero_record_id,
                zero_record_id,
                ACTIVATION_RESULT_MALFORMED_RECORD,
            );

            let mut wrong_id = good;
            wrong_id.request_id += 1;
            run_rejected_case(
                envelope(good.request_id),
                wrong_id,
                wrong_id,
                ACTIVATION_RESULT_INVALID_ENVELOPE,
            );
        }

        #[test]
        fn valid_ack_publishes_dormant_until_atomic_active_transition() {
            let request = valid_request(0x1234_5678);
            let mut status = dormant_status();
            let mut accepted = None;
            let mut source = source(request);
            let mut timer = FakeTimerLease::default();
            let mut publisher = FakePublisher::default();
            assert_eq!(
                handle(
                    envelope(request.request_id),
                    &mut status,
                    &mut accepted,
                    &mut source,
                    &mut timer,
                    &mut publisher,
                ),
                ACTIVATION_RESULT_SUCCESS
            );
            assert_eq!(
                (timer.mmio_accesses, timer.plic_installs, timer.irq_actions),
                (1, 1, 1)
            );
            assert_eq!(source.reads, 1);
            assert_eq!(publisher.snapshots.len(), 3);
            assert!(
                publisher.snapshots[..2]
                    .iter()
                    .all(|snapshot| snapshot.capabilities == DORMANT_CAPABILITIES)
            );
            assert!(
                publisher
                    .snapshots
                    .iter()
                    .all(|snapshot| snapshot.state == STATE_RUNNING)
            );
            assert_eq!(status.activation_state, ACTIVATION_STATE_ACTIVE);
            assert_eq!(status.capabilities, EXPECTED_CAPABILITIES);
            assert!(accepted.is_some());
        }

        #[test]
        fn zero_sequence_is_valid_when_request_id_low_half_is_zero() {
            let request = valid_request(0x0001_0000);
            let mut status = dormant_status();
            let mut accepted = None;
            let mut source = source(request);
            let mut timer = FakeTimerLease::default();
            let mut publisher = FakePublisher::default();
            assert_eq!(
                handle(
                    envelope(request.request_id),
                    &mut status,
                    &mut accepted,
                    &mut source,
                    &mut timer,
                    &mut publisher,
                ),
                ACTIVATION_RESULT_SUCCESS
            );
        }

        #[test]
        fn exact_active_duplicate_is_idempotent() {
            let request = valid_request(99);
            let mut status = dormant_status();
            let mut accepted = None;
            let mut source = source(request);
            let mut timer = FakeTimerLease::default();
            let mut publisher = FakePublisher::default();
            assert_eq!(
                handle(
                    envelope(99),
                    &mut status,
                    &mut accepted,
                    &mut source,
                    &mut timer,
                    &mut publisher,
                ),
                ACTIVATION_RESULT_SUCCESS
            );
            let counters = (timer.mmio_accesses, timer.plic_installs, timer.irq_actions);
            let attempts = status.activation_attempts;
            assert_eq!(
                handle(
                    envelope(99),
                    &mut status,
                    &mut accepted,
                    &mut source,
                    &mut timer,
                    &mut publisher,
                ),
                ACTIVATION_RESULT_SUCCESS
            );
            assert_eq!(
                (timer.mmio_accesses, timer.plic_installs, timer.irq_actions),
                counters
            );
            assert_eq!(status.activation_attempts, attempts);

            let mut different = request;
            different.request_id = 100;
            source.first = different;
            source.second = different;
            assert_eq!(
                handle(
                    envelope(100),
                    &mut status,
                    &mut accepted,
                    &mut source,
                    &mut timer,
                    &mut publisher,
                ),
                ACTIVATION_RESULT_INVALID_STATE
            );
            assert_eq!(
                (timer.mmio_accesses, timer.plic_installs, timer.irq_actions),
                counters
            );
            assert_eq!(status.activation_state, ACTIVATION_STATE_ACTIVE);
        }

        #[test]
        fn lease_failure_is_terminal_but_base_services_remain_running() {
            let request = valid_request(42);
            let mut status = dormant_status();
            let mut accepted = None;
            let mut source = source(request);
            let failure = LeaseFailure {
                result: ACTIVATION_RESULT_SELF_TEST_FAILED,
                flag: crate::contract::FLAG_TIMER4_SELF_TEST_FAILED,
            };
            let mut timer = FakeTimerLease {
                failure: Some(failure),
                ..FakeTimerLease::default()
            };
            let mut publisher = FakePublisher::default();
            assert_eq!(
                handle(
                    envelope(42),
                    &mut status,
                    &mut accepted,
                    &mut source,
                    &mut timer,
                    &mut publisher,
                ),
                ACTIVATION_RESULT_SELF_TEST_FAILED
            );
            assert_eq!(status.state, STATE_RUNNING);
            assert_eq!(status.activation_state, ACTIVATION_STATE_LEASE_FAULT);
            assert_eq!(status.capabilities, DORMANT_CAPABILITIES);
            assert_ne!(status.flags & FLAG_ACTIVATION_FAILED, 0);
            assert_ne!(
                status.flags & crate::contract::FLAG_TIMER4_SELF_TEST_FAILED,
                0
            );
            assert_eq!(
                handle(
                    envelope(42),
                    &mut status,
                    &mut accepted,
                    &mut source,
                    &mut timer,
                    &mut publisher,
                ),
                ACTIVATION_RESULT_INVALID_STATE
            );
            assert_eq!(timer.mmio_accesses, 1);
        }

        #[test]
        fn invalid_transitional_state_never_reads_or_touches_timer() {
            let request = valid_request(9);
            let mut status = dormant_status();
            status.activation_state = ACTIVATION_STATE_INITIALIZING;
            let mut accepted = None;
            let mut source = source(request);
            let mut timer = FakeTimerLease::default();
            let mut publisher = FakePublisher::default();
            assert_eq!(
                handle(
                    envelope(9),
                    &mut status,
                    &mut accepted,
                    &mut source,
                    &mut timer,
                    &mut publisher,
                ),
                ACTIVATION_RESULT_INVALID_STATE
            );
            assert_eq!(source.reads, 0);
            assert_eq!(
                (timer.mmio_accesses, timer.plic_installs, timer.irq_actions),
                (0, 0, 0)
            );
        }
    }
}
