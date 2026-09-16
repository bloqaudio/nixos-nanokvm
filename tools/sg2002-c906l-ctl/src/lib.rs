use std::collections::BTreeMap;
use std::error::Error as StdError;
use std::fmt;
use std::fs::File;
use std::io::Read as _;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use rustix::event::{PollFd, PollFlags, Timespec, poll};
use rustix::fd::OwnedFd;
use rustix::fs::{Mode, OFlags, open};
use rustix::io::{Errno, read, write};

#[allow(dead_code)]
mod contract {
    include!(env!("SG2002_C906L_CONTRACT_RS"));
}

pub use contract::Message;

pub const SERVICE_CONTROL: u8 = contract::SERVICE_CONTROL;
pub const OP_PING: u8 = contract::OP_PING;
pub const OP_GET_ABI: u8 = contract::OP_GET_ABI;
pub const OP_GET_CAPABILITIES: u8 = contract::OP_GET_CAPABILITIES;
pub const OP_RESPONSE: u8 = contract::OP_RESPONSE;
pub const OP_ERROR: u8 = contract::OP_ERROR;

pub const ABI_MAJOR: u16 = contract::ABI_MAJOR;
pub const ABI_MINOR: u16 = contract::ABI_MINOR;
pub const CAP_MAILBOX: u32 = contract::CAP_MAILBOX as u32;
pub const CAP_SHMEM_HEARTBEAT: u32 = contract::CAP_SHMEM_HEARTBEAT as u32;
pub const CAP_TIMER4_SELF_TEST: u32 = contract::CAP_TIMER4_SELF_TEST as u32;
pub const CAP_TIMER5_SELF_TEST: u32 = contract::CAP_TIMER5_SELF_TEST as u32;
pub const CAP_TIMER6_SELF_TEST: u32 = contract::CAP_TIMER6_SELF_TEST as u32;
pub const CAP_TIMER7_SELF_TEST: u32 = contract::CAP_TIMER7_SELF_TEST as u32;
pub const CAP_PICOCLAW_LCD: u32 = contract::CAP_PICOCLAW_LCD as u32;
pub const CAP_RPMSG: u32 = contract::CAP_RPMSG as u32;
pub const EXPECTED_CAPABILITIES: u64 = contract::EXPECTED_CAPABILITIES;
pub const PROFILE_NAME: &str = contract::PROFILE_NAME;
pub const CONTRACT_SHA256_HEX: &str = contract::CONTRACT_SHA256_HEX;
pub const CONTRACT_SHA256: [u8; 32] = contract::CONTRACT_SHA256;
pub const CONTRACT_EPOCH: u32 = contract::CONTRACT_EPOCH;
pub const PROFILE_ID: u32 = contract::PROFILE_ID;
pub const DORMANT_CAPABILITIES: u64 = contract::DORMANT_CAPABILITIES;
pub const LEASE_MASK: u64 = contract::LEASE_MASK;
pub const MANIFEST_FLAGS: u32 = contract::MANIFEST_FLAGS;
pub const ACTIVATION_REQUIRED: bool = contract::ACTIVATION_REQUIRED;
pub const ACTIVATION_STATE_ACTIVE: u8 = contract::ACTIVATION_STATE_ACTIVE;
pub const ACTIVATION_RESULT_SUCCESS: u32 = contract::ACTIVATION_RESULT_SUCCESS;
const _: () = assert!(EXPECTED_CAPABILITIES <= u32::MAX as u64);
pub const EXPECTED_CAPABILITIES_U32: u32 = EXPECTED_CAPABILITIES as u32;

impl Message {
    pub fn encode(self) -> [u8; 8] {
        let sequence = self.sequence.to_le_bytes();
        let value = self.value.to_le_bytes();
        [
            self.service,
            self.opcode,
            sequence[0],
            sequence[1],
            value[0],
            value[1],
            value[2],
            value[3],
        ]
    }

    pub fn decode(bytes: [u8; 8]) -> Self {
        Self {
            service: bytes[0],
            opcode: bytes[1],
            sequence: u16::from_le_bytes([bytes[2], bytes[3]]),
            value: u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AbiVersion {
    pub major: u16,
    pub minor: u16,
}

impl fmt::Display for AbiVersion {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{}.{}", self.major, self.minor)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ActivationOutcome {
    NotRequired,
    AlreadyActive,
    Completed,
    RecoveredLostResponse,
}

impl fmt::Display for ActivationOutcome {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::NotRequired => "not-required",
            Self::AlreadyActive => "already-active",
            Self::Completed => "completed",
            Self::RecoveredLostResponse => "recovered-lost-response",
        })
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ContractIdentity {
    pub profile: String,
    pub profile_id: u32,
    pub sha256: [u8; 32],
    pub epoch: u32,
    pub abi: AbiVersion,
    pub final_capabilities: u64,
    pub dormant_capabilities: u64,
    pub lease_mask: u64,
    pub manifest_flags: u32,
    pub activation_required: bool,
}

impl ContractIdentity {
    pub fn compiled() -> Self {
        Self {
            profile: PROFILE_NAME.to_owned(),
            profile_id: PROFILE_ID,
            sha256: CONTRACT_SHA256,
            epoch: CONTRACT_EPOCH,
            abi: AbiVersion {
                major: ABI_MAJOR,
                minor: ABI_MINOR,
            },
            final_capabilities: EXPECTED_CAPABILITIES,
            dormant_capabilities: DORMANT_CAPABILITIES,
            lease_mask: LEASE_MASK,
            manifest_flags: MANIFEST_FLAGS,
            activation_required: ACTIVATION_REQUIRED,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ActivationStatus {
    pub driver_outcome: ActivationOutcome,
    pub state: u8,
    pub error: u8,
    pub attempts: u16,
    pub request_id: u32,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ControlState {
    pub contract: ContractIdentity,
    pub activation: ActivationStatus,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LatencyStats {
    pub count: usize,
    pub p50: Duration,
    pub p99: Duration,
    pub max: Duration,
}

impl LatencyStats {
    pub fn from_samples(samples: &mut [Duration]) -> Result<Self, Error> {
        if samples.is_empty() {
            return Err(Error::InvalidArgument("sample count must be non-zero"));
        }
        samples.sort_unstable();
        Ok(Self {
            count: samples.len(),
            p50: nearest_rank(samples, 50),
            p99: nearest_rank(samples, 99),
            max: *samples.last().expect("non-empty checked above"),
        })
    }
}

fn nearest_rank(sorted: &[Duration], percentile: usize) -> Duration {
    let rank = sorted.len().saturating_mul(percentile).div_ceil(100);
    sorted[rank.saturating_sub(1)]
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Error {
    Transport(String),
    SysfsRead {
        path: PathBuf,
        detail: String,
    },
    MalformedSysfs {
        attribute: &'static str,
        detail: String,
    },
    UnstableSysfs,
    ContractMismatch {
        field: &'static str,
        detail: String,
    },
    ActivationMismatch(String),
    Timeout(&'static str),
    ShortWrite(usize),
    ShortRead(usize),
    UnexpectedService {
        expected: u8,
        actual: u8,
    },
    UnexpectedOpcode {
        expected: u8,
        actual: u8,
    },
    UnexpectedSequence {
        expected: u16,
        actual: u16,
    },
    UnexpectedValue {
        expected: u32,
        actual: u32,
    },
    RemoteError {
        sequence: u16,
        value: u32,
    },
    DataPlaneLength {
        actual: usize,
        expected: usize,
    },
    DataPlaneMismatch(usize),
    InvalidArgument(&'static str),
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Transport(detail) => write!(formatter, "transport error: {detail}"),
            Self::SysfsRead { path, detail } => {
                write!(formatter, "cannot read {}: {detail}", path.display())
            }
            Self::MalformedSysfs { attribute, detail } => {
                write!(formatter, "malformed {attribute} sysfs attribute: {detail}")
            }
            Self::UnstableSysfs => formatter
                .write_str("contract or activation sysfs attribute changed between snapshots"),
            Self::ContractMismatch { field, detail } => {
                write!(
                    formatter,
                    "compiled contract mismatch for {field}: {detail}"
                )
            }
            Self::ActivationMismatch(detail) => {
                write!(formatter, "activation state mismatch: {detail}")
            }
            Self::Timeout(phase) => write!(formatter, "timed out waiting for {phase}"),
            Self::ShortWrite(count) => {
                write!(
                    formatter,
                    "device accepted {count} bytes, expected exactly 8"
                )
            }
            Self::ShortRead(count) => {
                write!(
                    formatter,
                    "device returned {count} bytes, expected exactly 8"
                )
            }
            Self::UnexpectedService { expected, actual } => write!(
                formatter,
                "response service 0x{actual:02x}, expected 0x{expected:02x}"
            ),
            Self::UnexpectedOpcode { expected, actual } => write!(
                formatter,
                "response opcode 0x{actual:02x}, expected 0x{expected:02x}"
            ),
            Self::UnexpectedSequence { expected, actual } => {
                write!(formatter, "response sequence {actual}, expected {expected}")
            }
            Self::UnexpectedValue { expected, actual } => write!(
                formatter,
                "response value 0x{actual:08x}, expected 0x{expected:08x}"
            ),
            Self::RemoteError { sequence, value } => write!(
                formatter,
                "firmware rejected sequence {sequence} with value 0x{value:08x}"
            ),
            Self::DataPlaneLength { actual, expected } => write!(
                formatter,
                "RPMsg returned {actual} bytes, expected exactly {expected}"
            ),
            Self::DataPlaneMismatch(offset) => {
                write!(formatter, "RPMsg echo differs at byte {offset}")
            }
            Self::InvalidArgument(detail) => formatter.write_str(detail),
        }
    }
}

impl StdError for Error {}

fn malformed(attribute: &'static str, detail: impl Into<String>) -> Error {
    Error::MalformedSysfs {
        attribute,
        detail: detail.into(),
    }
}

fn parse_fields<'a>(
    attribute: &'static str,
    input: &'a str,
    expected: &[&'static str],
) -> Result<BTreeMap<&'a str, &'a str>, Error> {
    let line = input
        .strip_suffix('\n')
        .ok_or_else(|| malformed(attribute, "record is not newline-terminated"))?;
    if line.is_empty() || line.contains('\n') || line.contains('\r') {
        return Err(malformed(
            attribute,
            "record is empty or contains extra lines",
        ));
    }

    let mut fields = BTreeMap::new();
    for entry in line.split(' ') {
        if entry.is_empty() {
            return Err(malformed(
                attribute,
                "fields are not separated by one space",
            ));
        }
        let (name, value) = entry
            .split_once('=')
            .ok_or_else(|| malformed(attribute, format!("field {entry:?} has no '='")))?;
        if name.is_empty() || value.is_empty() || value.contains('=') {
            return Err(malformed(attribute, format!("invalid field {entry:?}")));
        }
        if !expected.contains(&name) {
            return Err(malformed(attribute, format!("unknown field {name:?}")));
        }
        if fields.insert(name, value).is_some() {
            return Err(malformed(attribute, format!("duplicate field {name:?}")));
        }
    }
    for name in expected {
        if !fields.contains_key(name) {
            return Err(malformed(attribute, format!("missing field {name:?}")));
        }
    }
    Ok(fields)
}

fn parse_decimal<T>(attribute: &'static str, field: &'static str, text: &str) -> Result<T, Error>
where
    T: std::str::FromStr + fmt::Display,
{
    if text.is_empty() || !text.bytes().all(|byte| byte.is_ascii_digit()) {
        return Err(malformed(attribute, format!("{field} is not decimal")));
    }
    let value = text
        .parse::<T>()
        .map_err(|_| malformed(attribute, format!("{field} is out of range")))?;
    if value.to_string() != text {
        return Err(malformed(
            attribute,
            format!("{field} is not canonically encoded"),
        ));
    }
    Ok(value)
}

fn parse_fixed_hex(
    attribute: &'static str,
    field: &'static str,
    text: &str,
    digits: usize,
) -> Result<u64, Error> {
    let Some(hex) = text.strip_prefix("0x") else {
        return Err(malformed(attribute, format!("{field} has no 0x prefix")));
    };
    if hex.len() != digits
        || !hex
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(malformed(
            attribute,
            format!("{field} is not {digits}-digit lowercase hexadecimal"),
        ));
    }
    u64::from_str_radix(hex, 16)
        .map_err(|_| malformed(attribute, format!("{field} is out of range")))
}

fn parse_abi(attribute: &'static str, text: &str) -> Result<AbiVersion, Error> {
    let (major, minor) = text
        .split_once('.')
        .ok_or_else(|| malformed(attribute, "abi has no major.minor separator"))?;
    if minor.contains('.') {
        return Err(malformed(attribute, "abi contains multiple separators"));
    }
    Ok(AbiVersion {
        major: parse_decimal(attribute, "abi major", major)?,
        minor: parse_decimal(attribute, "abi minor", minor)?,
    })
}

fn parse_digest(attribute: &'static str, text: &str) -> Result<[u8; 32], Error> {
    if text.len() != 64
        || !text
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(malformed(
            attribute,
            "sha256 is not 64-digit lowercase hexadecimal",
        ));
    }
    let mut digest = [0_u8; 32];
    for (index, byte) in digest.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&text[index * 2..index * 2 + 2], 16)
            .map_err(|_| malformed(attribute, "sha256 contains invalid hexadecimal"))?;
    }
    Ok(digest)
}

fn format_digest(digest: &[u8; 32]) -> String {
    use std::fmt::Write as _;

    let mut result = String::with_capacity(64);
    for byte in digest {
        write!(&mut result, "{byte:02x}").expect("writing to a String cannot fail");
    }
    result
}

pub fn parse_contract_attribute(input: &str) -> Result<ContractIdentity, Error> {
    const ATTRIBUTE: &str = "contract";
    const FIELDS: &[&str] = &[
        "profile",
        "profile_id",
        "sha256",
        "epoch",
        "abi",
        "final_capabilities",
        "dormant_capabilities",
        "lease_mask",
        "manifest_flags",
        "activation_required",
    ];
    let fields = parse_fields(ATTRIBUTE, input, FIELDS)?;
    let profile = fields["profile"];
    if profile.is_empty()
        || !profile.bytes().enumerate().all(|(index, byte)| {
            byte.is_ascii_lowercase()
                || byte.is_ascii_digit() && index > 0
                || byte == b'-' && index > 0
        })
    {
        return Err(malformed(ATTRIBUTE, "profile is not a canonical slug"));
    }
    let activation_required = match fields["activation_required"] {
        "0" => false,
        "1" => true,
        _ => {
            return Err(malformed(
                ATTRIBUTE,
                "activation_required is not exactly 0 or 1",
            ));
        }
    };
    Ok(ContractIdentity {
        profile: profile.to_owned(),
        profile_id: parse_decimal(ATTRIBUTE, "profile_id", fields["profile_id"])?,
        sha256: parse_digest(ATTRIBUTE, fields["sha256"])?,
        epoch: parse_decimal(ATTRIBUTE, "epoch", fields["epoch"])?,
        abi: parse_abi(ATTRIBUTE, fields["abi"])?,
        final_capabilities: parse_fixed_hex(
            ATTRIBUTE,
            "final_capabilities",
            fields["final_capabilities"],
            16,
        )?,
        dormant_capabilities: parse_fixed_hex(
            ATTRIBUTE,
            "dormant_capabilities",
            fields["dormant_capabilities"],
            16,
        )?,
        lease_mask: parse_fixed_hex(ATTRIBUTE, "lease_mask", fields["lease_mask"], 16)?,
        manifest_flags: u32::try_from(parse_fixed_hex(
            ATTRIBUTE,
            "manifest_flags",
            fields["manifest_flags"],
            8,
        )?)
        .expect("eight hexadecimal digits always fit u32"),
        activation_required,
    })
}

pub fn parse_activation_attribute(input: &str) -> Result<ActivationStatus, Error> {
    const ATTRIBUTE: &str = "activation";
    const FIELDS: &[&str] = &["driver_outcome", "state", "error", "attempts", "request_id"];
    let fields = parse_fields(ATTRIBUTE, input, FIELDS)?;
    let driver_outcome = match fields["driver_outcome"] {
        "not-required" => ActivationOutcome::NotRequired,
        "already-active" => ActivationOutcome::AlreadyActive,
        "completed" => ActivationOutcome::Completed,
        "recovered-lost-response" => ActivationOutcome::RecoveredLostResponse,
        unknown => {
            return Err(malformed(
                ATTRIBUTE,
                format!("unknown driver_outcome {unknown:?}"),
            ));
        }
    };
    Ok(ActivationStatus {
        driver_outcome,
        state: parse_decimal(ATTRIBUTE, "state", fields["state"])?,
        error: parse_decimal(ATTRIBUTE, "error", fields["error"])?,
        attempts: parse_decimal(ATTRIBUTE, "attempts", fields["attempts"])?,
        request_id: parse_decimal(ATTRIBUTE, "request_id", fields["request_id"])?,
    })
}

impl fmt::Display for ContractIdentity {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "profile={} profile_id={} sha256={} epoch={} abi={} \
             final_capabilities=0x{:016x} dormant_capabilities=0x{:016x} \
             lease_mask=0x{:016x} manifest_flags=0x{:08x} activation_required={}",
            self.profile,
            self.profile_id,
            format_digest(&self.sha256),
            self.epoch,
            self.abi,
            self.final_capabilities,
            self.dormant_capabilities,
            self.lease_mask,
            self.manifest_flags,
            u8::from(self.activation_required),
        )
    }
}

impl fmt::Display for ActivationStatus {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "driver_outcome={} state={} error={} attempts={} request_id={}",
            self.driver_outcome, self.state, self.error, self.attempts, self.request_id,
        )
    }
}

fn read_attribute(directory: &Path, name: &'static str) -> Result<String, Error> {
    let path = directory.join(name);
    let mut file = File::open(&path).map_err(|error| Error::SysfsRead {
        path: path.clone(),
        detail: error.to_string(),
    })?;
    let mut bytes = Vec::with_capacity(512);
    file.by_ref()
        .take(4097)
        .read_to_end(&mut bytes)
        .map_err(|error| Error::SysfsRead {
            path,
            detail: error.to_string(),
        })?;
    if bytes.len() > 4096 {
        return Err(malformed(name, "record exceeds one sysfs page"));
    }
    String::from_utf8(bytes).map_err(|_| malformed(name, "record is not UTF-8"))
}

fn read_stable_attribute(directory: &Path, name: &'static str) -> Result<String, Error> {
    let first = read_attribute(directory, name)?;
    let second = read_attribute(directory, name)?;
    if first != second {
        return Err(Error::UnstableSysfs);
    }
    Ok(first)
}

pub fn read_contract(directory: &Path) -> Result<ContractIdentity, Error> {
    parse_contract_attribute(&read_stable_attribute(directory, "contract")?)
}

pub fn read_activation(directory: &Path) -> Result<ActivationStatus, Error> {
    parse_activation_attribute(&read_stable_attribute(directory, "activation")?)
}

pub fn read_control_state(directory: &Path) -> Result<ControlState, Error> {
    let first_contract = read_attribute(directory, "contract")?;
    let first_activation = read_attribute(directory, "activation")?;
    let second_contract = read_attribute(directory, "contract")?;
    let second_activation = read_attribute(directory, "activation")?;
    if first_contract != second_contract || first_activation != second_activation {
        return Err(Error::UnstableSysfs);
    }
    Ok(ControlState {
        contract: parse_contract_attribute(&first_contract)?,
        activation: parse_activation_attribute(&first_activation)?,
    })
}

fn contract_mismatch(
    field: &'static str,
    actual: impl fmt::Display,
    expected: impl fmt::Display,
) -> Error {
    Error::ContractMismatch {
        field,
        detail: format!("got {actual}, expected {expected}"),
    }
}

pub fn validate_compiled_contract(actual: &ContractIdentity) -> Result<(), Error> {
    let expected = ContractIdentity::compiled();
    if actual.profile != expected.profile {
        return Err(contract_mismatch(
            "profile",
            &actual.profile,
            &expected.profile,
        ));
    }
    if actual.profile_id != expected.profile_id {
        return Err(contract_mismatch(
            "profile_id",
            actual.profile_id,
            expected.profile_id,
        ));
    }
    if actual.sha256 != expected.sha256 {
        return Err(contract_mismatch(
            "sha256",
            format_digest(&actual.sha256),
            format_digest(&expected.sha256),
        ));
    }
    if actual.epoch != expected.epoch {
        return Err(contract_mismatch("epoch", actual.epoch, expected.epoch));
    }
    if actual.abi != expected.abi {
        return Err(contract_mismatch("abi", actual.abi, expected.abi));
    }
    if actual.final_capabilities != expected.final_capabilities {
        return Err(contract_mismatch(
            "final_capabilities",
            format_args!("0x{:016x}", actual.final_capabilities),
            format_args!("0x{:016x}", expected.final_capabilities),
        ));
    }
    if actual.dormant_capabilities != expected.dormant_capabilities {
        return Err(contract_mismatch(
            "dormant_capabilities",
            format_args!("0x{:016x}", actual.dormant_capabilities),
            format_args!("0x{:016x}", expected.dormant_capabilities),
        ));
    }
    if actual.lease_mask != expected.lease_mask {
        return Err(contract_mismatch(
            "lease_mask",
            format_args!("0x{:016x}", actual.lease_mask),
            format_args!("0x{:016x}", expected.lease_mask),
        ));
    }
    if actual.manifest_flags != expected.manifest_flags {
        return Err(contract_mismatch(
            "manifest_flags",
            format_args!("0x{:08x}", actual.manifest_flags),
            format_args!("0x{:08x}", expected.manifest_flags),
        ));
    }
    if actual.activation_required != expected.activation_required {
        return Err(contract_mismatch(
            "activation_required",
            u8::from(actual.activation_required),
            u8::from(expected.activation_required),
        ));
    }
    Ok(())
}

pub fn validate_activation(
    contract: &ContractIdentity,
    activation: &ActivationStatus,
) -> Result<(), Error> {
    if activation.state != ACTIVATION_STATE_ACTIVE {
        return Err(Error::ActivationMismatch(format!(
            "state is {}, expected ACTIVE ({ACTIVATION_STATE_ACTIVE})",
            activation.state
        )));
    }
    if u32::from(activation.error) != ACTIVATION_RESULT_SUCCESS {
        return Err(Error::ActivationMismatch(format!(
            "error is {}, expected SUCCESS ({ACTIVATION_RESULT_SUCCESS})",
            activation.error
        )));
    }
    if contract.activation_required {
        if activation.driver_outcome == ActivationOutcome::NotRequired {
            return Err(Error::ActivationMismatch(
                "lease profile reports driver_outcome=not-required".to_owned(),
            ));
        }
        if activation.attempts == 0 || activation.request_id == 0 {
            return Err(Error::ActivationMismatch(
                "lease profile has no completed activation attempt/request".to_owned(),
            ));
        }
    } else if activation.driver_outcome != ActivationOutcome::NotRequired
        || activation.attempts != 0
        || activation.request_id != 0
    {
        return Err(Error::ActivationMismatch(
            "base profile has unexpected activation history".to_owned(),
        ));
    }
    Ok(())
}

pub fn validate_control_state(state: &ControlState) -> Result<(), Error> {
    validate_compiled_contract(&state.contract)?;
    validate_activation(&state.contract, &state.activation)
}

pub trait Transport {
    fn exchange(&mut self, request: [u8; 8], timeout: Duration) -> Result<[u8; 8], Error>;
}

pub struct DeviceTransport {
    fd: OwnedFd,
}

impl DeviceTransport {
    pub fn open(path: &Path) -> Result<Self, Error> {
        let fd = open(
            path,
            OFlags::RDWR | OFlags::NONBLOCK | OFlags::CLOEXEC,
            Mode::empty(),
        )
        .map_err(|error| Error::Transport(format!("open {}: {error}", path.display())))?;
        Ok(Self { fd })
    }

    fn wait_for(
        &self,
        wanted: PollFlags,
        deadline: Instant,
        phase: &'static str,
    ) -> Result<(), Error> {
        loop {
            let remaining = deadline
                .checked_duration_since(Instant::now())
                .ok_or(Error::Timeout(phase))?;
            if remaining.is_zero() {
                return Err(Error::Timeout(phase));
            }
            let timeout = Timespec::try_from(remaining)
                .map_err(|error| Error::Transport(format!("poll timeout: {error}")))?;
            let mut descriptor = [PollFd::new(&self.fd, wanted)];
            match poll(&mut descriptor, Some(&timeout)) {
                Ok(0) => return Err(Error::Timeout(phase)),
                Ok(_) => {
                    let events = descriptor[0].revents();
                    let failures = PollFlags::ERR | PollFlags::HUP | PollFlags::NVAL;
                    if events.intersects(failures) {
                        return Err(Error::Transport(format!(
                            "poll reported terminal events {events:?}"
                        )));
                    }
                    if events.intersects(wanted) {
                        return Ok(());
                    }
                }
                Err(Errno::INTR) => continue,
                Err(error) => return Err(Error::Transport(format!("poll: {error}"))),
            }
        }
    }
}

impl Transport for DeviceTransport {
    fn exchange(&mut self, request: [u8; 8], timeout: Duration) -> Result<[u8; 8], Error> {
        let deadline = Instant::now()
            .checked_add(timeout)
            .ok_or_else(|| Error::Transport("timeout exceeds monotonic clock range".into()))?;

        loop {
            self.wait_for(PollFlags::OUT, deadline, "request readiness")?;
            match write(&self.fd, &request) {
                Ok(8) => break,
                Ok(count) => return Err(Error::ShortWrite(count)),
                Err(error) if error == Errno::INTR || error == Errno::AGAIN => continue,
                Err(error) => return Err(Error::Transport(format!("write: {error}"))),
            }
        }

        loop {
            self.wait_for(PollFlags::IN, deadline, "response")?;
            let mut response = [0_u8; 8];
            match read(&self.fd, &mut response) {
                Ok(8) => return Ok(response),
                Ok(count) => return Err(Error::ShortRead(count)),
                Err(error) if error == Errno::INTR || error == Errno::AGAIN => continue,
                Err(error) => return Err(Error::Transport(format!("read: {error}"))),
            }
        }
    }
}

/// Linux's standard `rpmsg_char` endpoint for the firmware echo service.
pub struct RpmsgEcho {
    fd: OwnedFd,
}

// Change every cacheline on every exchange, including after the 16-bit ring
// indices wrap. Repeating a sequence-dependent word also exercises short tails.
fn stress_payload(payload: &mut [u8], iteration: usize) {
    let mut marker = (iteration as u64).wrapping_add(0x9e37_79b9_7f4a_7c15);
    marker = (marker ^ (marker >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    marker = (marker ^ (marker >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    let marker = (marker ^ (marker >> 31)).to_le_bytes();
    for (offset, byte) in payload.iter_mut().enumerate() {
        *byte = marker[offset % marker.len()] ^ (offset as u8).wrapping_mul(0x5b);
    }
}

impl RpmsgEcho {
    pub fn open(path: &Path) -> Result<Self, Error> {
        let fd = open(
            path,
            OFlags::RDWR | OFlags::NONBLOCK | OFlags::CLOEXEC,
            Mode::empty(),
        )
        .map_err(|error| Error::Transport(format!("open {}: {error}", path.display())))?;
        Ok(Self { fd })
    }

    fn wait_for(
        &self,
        wanted: PollFlags,
        deadline: Instant,
        phase: &'static str,
    ) -> Result<(), Error> {
        loop {
            let remaining = deadline
                .checked_duration_since(Instant::now())
                .ok_or(Error::Timeout(phase))?;
            if remaining.is_zero() {
                return Err(Error::Timeout(phase));
            }
            let timeout = Timespec::try_from(remaining)
                .map_err(|error| Error::Transport(format!("poll timeout: {error}")))?;
            let mut descriptor = [PollFd::new(&self.fd, wanted)];
            match poll(&mut descriptor, Some(&timeout)) {
                Ok(0) => return Err(Error::Timeout(phase)),
                Ok(_) => {
                    let events = descriptor[0].revents();
                    let failures = PollFlags::ERR | PollFlags::HUP | PollFlags::NVAL;
                    if events.intersects(failures) {
                        return Err(Error::Transport(format!(
                            "RPMsg poll reported terminal events {events:?}"
                        )));
                    }
                    if events.intersects(wanted) {
                        return Ok(());
                    }
                }
                Err(Errno::INTR) => continue,
                Err(error) => return Err(Error::Transport(format!("RPMsg poll: {error}"))),
            }
        }
    }

    pub fn echo(&mut self, payload: &[u8], timeout: Duration) -> Result<(), Error> {
        if payload.is_empty() || payload.len() > 496 {
            return Err(Error::InvalidArgument(
                "RPMsg payload length must be in 1..=496",
            ));
        }
        let deadline = Instant::now()
            .checked_add(timeout)
            .ok_or_else(|| Error::Transport("timeout exceeds monotonic clock range".into()))?;

        loop {
            self.wait_for(PollFlags::OUT, deadline, "RPMsg request readiness")?;
            match write(&self.fd, payload) {
                Ok(count) if count == payload.len() => break,
                Ok(count) => {
                    return Err(Error::DataPlaneLength {
                        actual: count,
                        expected: payload.len(),
                    });
                }
                Err(error) if error == Errno::INTR || error == Errno::AGAIN => continue,
                Err(error) => return Err(Error::Transport(format!("RPMsg write: {error}"))),
            }
        }

        loop {
            self.wait_for(PollFlags::IN, deadline, "RPMsg echo")?;
            // rpmsg_char truncates a datagram to the read buffer and discards
            // the rest. Read beyond the maximum valid payload so an oversized
            // reply with the correct prefix can never pass the length check.
            let mut response = [0_u8; 497];
            match read(&self.fd, &mut response) {
                Ok(count) if count == payload.len() => {
                    if let Some(offset) = response[..count]
                        .iter()
                        .zip(payload)
                        .position(|(actual, expected)| actual != expected)
                    {
                        return Err(Error::DataPlaneMismatch(offset));
                    }
                    return Ok(());
                }
                Ok(count) => {
                    return Err(Error::DataPlaneLength {
                        actual: count,
                        expected: payload.len(),
                    });
                }
                Err(error) if error == Errno::INTR || error == Errno::AGAIN => continue,
                Err(error) => return Err(Error::Transport(format!("RPMsg read: {error}"))),
            }
        }
    }

    pub fn benchmark(
        &mut self,
        count: usize,
        payload_size: usize,
        timeout: Duration,
    ) -> Result<LatencyStats, Error> {
        if count == 0 {
            return Err(Error::InvalidArgument("sample count must be non-zero"));
        }
        if !(1..=496).contains(&payload_size) {
            return Err(Error::InvalidArgument(
                "RPMsg payload length must be in 1..=496",
            ));
        }

        self.benchmark_sizes(count, |_| payload_size, timeout)
    }

    /// Cycle through every supported non-empty length with changing data.
    pub fn stress(&mut self, count: usize, timeout: Duration) -> Result<LatencyStats, Error> {
        if count == 0 {
            return Err(Error::InvalidArgument("sample count must be non-zero"));
        }
        self.benchmark_sizes(count, |iteration| 1 + iteration % 496, timeout)
    }

    fn benchmark_sizes(
        &mut self,
        count: usize,
        size: impl Fn(usize) -> usize,
        timeout: Duration,
    ) -> Result<LatencyStats, Error> {
        let mut payload = [0_u8; 496];
        let mut samples = Vec::with_capacity(count);
        for iteration in 0..count {
            let payload = &mut payload[..size(iteration)];
            stress_payload(payload, iteration);

            let started = Instant::now();
            self.echo(payload, timeout)?;
            samples.push(started.elapsed());
        }
        LatencyStats::from_samples(&mut samples)
    }
}

pub struct Client<T> {
    transport: T,
    next_sequence: u16,
}

impl<T: Transport> Client<T> {
    pub fn new(transport: T) -> Self {
        Self::with_sequence(transport, 1)
    }

    pub fn with_sequence(transport: T, first_sequence: u16) -> Self {
        Self {
            transport,
            next_sequence: first_sequence,
        }
    }

    pub fn into_inner(self) -> T {
        self.transport
    }

    fn request(&mut self, opcode: u8, value: u32, timeout: Duration) -> Result<Message, Error> {
        let sequence = self.next_sequence;
        self.next_sequence = self.next_sequence.wrapping_add(1);
        let request = Message {
            service: SERVICE_CONTROL,
            opcode,
            sequence,
            value,
        };
        let response = Message::decode(self.transport.exchange(request.encode(), timeout)?);
        if response.service != SERVICE_CONTROL {
            return Err(Error::UnexpectedService {
                expected: SERVICE_CONTROL,
                actual: response.service,
            });
        }
        if response.sequence != sequence {
            return Err(Error::UnexpectedSequence {
                expected: sequence,
                actual: response.sequence,
            });
        }
        if response.opcode == OP_ERROR {
            return Err(Error::RemoteError {
                sequence,
                value: response.value,
            });
        }
        let expected_opcode = opcode | OP_RESPONSE;
        if response.opcode != expected_opcode {
            return Err(Error::UnexpectedOpcode {
                expected: expected_opcode,
                actual: response.opcode,
            });
        }
        Ok(response)
    }

    pub fn abi(&mut self, timeout: Duration) -> Result<AbiVersion, Error> {
        let value = self.request(OP_GET_ABI, 0, timeout)?.value;
        Ok(AbiVersion {
            major: (value >> 16) as u16,
            minor: value as u16,
        })
    }

    pub fn capabilities(&mut self, timeout: Duration) -> Result<u32, Error> {
        Ok(self.request(OP_GET_CAPABILITIES, 0, timeout)?.value)
    }

    pub fn ping(&mut self, value: u32, timeout: Duration) -> Result<u32, Error> {
        let actual = self.request(OP_PING, value, timeout)?.value;
        if actual != value {
            return Err(Error::UnexpectedValue {
                expected: value,
                actual,
            });
        }
        Ok(actual)
    }

    pub fn benchmark(&mut self, count: usize, timeout: Duration) -> Result<LatencyStats, Error> {
        if count == 0 {
            return Err(Error::InvalidArgument("benchmark count must be non-zero"));
        }
        let mut samples = Vec::with_capacity(count);
        for iteration in 0..count {
            let started = Instant::now();
            self.ping(iteration as u32, timeout)?;
            samples.push(started.elapsed());
        }
        LatencyStats::from_samples(&mut samples)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::net::UnixDatagram;

    fn echo_pair() -> (RpmsgEcho, UnixDatagram) {
        let (client, peer) = UnixDatagram::pair().unwrap();
        client.set_nonblocking(true).unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        (RpmsgEcho { fd: client.into() }, peer)
    }

    #[test]
    fn rpmsg_rejects_oversized_matching_prefix_including_maximum_payload() {
        for size in [1, 48, 496] {
            let (mut echo, peer) = echo_pair();
            let worker = std::thread::spawn(move || {
                let mut request = [0_u8; 497];
                let count = peer.recv(&mut request).unwrap();
                peer.send(&request[..count + 1]).unwrap();
            });
            assert_eq!(
                echo.echo(&vec![0xa5; size], Duration::from_secs(1)),
                Err(Error::DataPlaneLength {
                    actual: size + 1,
                    expected: size
                })
            );
            worker.join().unwrap();
        }
    }

    #[test]
    fn rpmsg_rejects_corruption_and_has_a_finite_response_deadline() {
        let (mut echo, peer) = echo_pair();
        let worker = std::thread::spawn(move || {
            let mut request = [0_u8; 496];
            let count = peer.recv(&mut request).unwrap();
            request[count - 1] ^= 1;
            peer.send(&request[..count]).unwrap();
        });
        assert_eq!(
            echo.echo(&[0xa5; 496], Duration::from_secs(1)),
            Err(Error::DataPlaneMismatch(495))
        );
        worker.join().unwrap();
        let (mut echo, _peer) = echo_pair();
        assert_eq!(
            echo.echo(&[1], Duration::from_millis(10)),
            Err(Error::Timeout("RPMsg echo"))
        );
    }

    #[test]
    fn rpmsg_stress_exercises_every_length_and_reuses_buffers_with_new_data() {
        let (mut echo, peer) = echo_pair();
        let worker = std::thread::spawn(move || {
            let mut request = [0_u8; 496];
            for iteration in 0..992 {
                let count = peer.recv(&mut request).unwrap();
                assert_eq!(count, 1 + iteration % 496);
                let mut expected = vec![0; count];
                stress_payload(&mut expected, iteration);
                assert_eq!(&request[..count], expected);
                peer.send(&request[..count]).unwrap();
            }
        });
        assert_eq!(echo.stress(992, Duration::from_secs(1)).unwrap().count, 992);
        worker.join().unwrap();
    }

    #[test]
    fn stress_patterns_change_every_cacheline_across_ring_wrap() {
        for iteration in [0, 255, 65535, 65536] {
            let mut before = [0; 496];
            let mut after = [0; 496];
            stress_payload(&mut before, iteration);
            stress_payload(&mut after, iteration + 1);
            for (before, after) in before.chunks(64).zip(after.chunks(64)) {
                assert_ne!(before, after);
            }
        }
    }

    fn valid_activation() -> ActivationStatus {
        if ACTIVATION_REQUIRED {
            ActivationStatus {
                driver_outcome: ActivationOutcome::Completed,
                state: ACTIVATION_STATE_ACTIVE,
                error: ACTIVATION_RESULT_SUCCESS as u8,
                attempts: 1,
                request_id: 0x1234,
            }
        } else {
            ActivationStatus {
                driver_outcome: ActivationOutcome::NotRequired,
                state: ACTIVATION_STATE_ACTIVE,
                error: ACTIVATION_RESULT_SUCCESS as u8,
                attempts: 0,
                request_id: 0,
            }
        }
    }

    #[test]
    fn generated_contract_is_exact_abi_1_1_and_round_trips_sysfs() {
        assert_eq!((ABI_MAJOR, ABI_MINOR), (1, 1));
        let expected = ContractIdentity::compiled();
        let encoded = format!("{expected}\n");
        assert_eq!(parse_contract_attribute(&encoded).unwrap(), expected);

        let activation = valid_activation();
        let encoded = format!("{activation}\n");
        assert_eq!(parse_activation_attribute(&encoded).unwrap(), activation);
        validate_control_state(&ControlState {
            contract: expected,
            activation,
        })
        .unwrap();
    }

    #[test]
    fn contract_rejects_duplicate_missing_unknown_and_malformed_fields() {
        let canonical = format!("{}\n", ContractIdentity::compiled());
        let without_newline = canonical.trim_end_matches('\n');
        let duplicate = format!("{without_newline} profile={PROFILE_NAME}\n");
        assert!(
            parse_contract_attribute(&duplicate)
                .unwrap_err()
                .to_string()
                .contains("duplicate")
        );

        let missing = canonical
            .split(' ')
            .filter(|field| !field.starts_with("epoch="))
            .collect::<Vec<_>>()
            .join(" ");
        assert!(
            parse_contract_attribute(&missing)
                .unwrap_err()
                .to_string()
                .contains("missing")
        );

        let unknown = format!("{without_newline} extra=0\n");
        assert!(
            parse_contract_attribute(&unknown)
                .unwrap_err()
                .to_string()
                .contains("unknown")
        );

        for malformed in [
            without_newline.to_owned(),
            canonical.replace("profile_id=", "profile_id=00"),
            canonical.replace("sha256=", "sha256=ABCDEF"),
            canonical.replace(" epoch=", "  epoch="),
            format!("{canonical}\n"),
        ] {
            assert!(
                parse_contract_attribute(&malformed).is_err(),
                "accepted {malformed:?}"
            );
        }
    }

    #[test]
    fn activation_rejects_duplicate_missing_unknown_and_malformed_fields() {
        let canonical = format!("{}\n", valid_activation());
        let without_newline = canonical.trim_end_matches('\n');
        let duplicate = format!("{without_newline} state={}\n", ACTIVATION_STATE_ACTIVE);
        assert!(
            parse_activation_attribute(&duplicate)
                .unwrap_err()
                .to_string()
                .contains("duplicate")
        );
        let missing = canonical.replacen("error=0 ", "", 1);
        assert!(
            parse_activation_attribute(&missing)
                .unwrap_err()
                .to_string()
                .contains("missing")
        );
        let unknown = format!("{without_newline} extra=0\n");
        assert!(
            parse_activation_attribute(&unknown)
                .unwrap_err()
                .to_string()
                .contains("unknown")
        );
        assert!(
            parse_activation_attribute(
                "driver_outcome=bogus state=4 error=0 attempts=0 request_id=0\n"
            )
            .is_err()
        );
        assert!(
            parse_activation_attribute(
                "driver_outcome=not-required state=04 error=0 attempts=0 request_id=0\n"
            )
            .is_err()
        );
    }

    #[test]
    fn every_compiled_identity_field_and_activation_policy_is_checked() {
        let expected = ContractIdentity::compiled();
        let mut mismatch = expected.clone();
        mismatch.sha256[0] ^= 1;
        assert!(
            validate_compiled_contract(&mismatch)
                .unwrap_err()
                .to_string()
                .contains("sha256")
        );
        mismatch = expected.clone();
        mismatch.final_capabilities ^= 1;
        assert!(
            validate_compiled_contract(&mismatch)
                .unwrap_err()
                .to_string()
                .contains("final_capabilities")
        );

        let mut activation = valid_activation();
        activation.state ^= 1;
        assert!(validate_activation(&expected, &activation).is_err());
        activation = valid_activation();
        activation.error = 1;
        assert!(validate_activation(&expected, &activation).is_err());
        activation = valid_activation();
        if ACTIVATION_REQUIRED {
            activation.request_id = 0;
        } else {
            activation.attempts = 1;
        }
        assert!(validate_activation(&expected, &activation).is_err());
    }

    #[derive(Default)]
    struct EchoTransport {
        requests: Vec<Message>,
        timeouts: Vec<Duration>,
        wrong_sequence: bool,
        wrong_value: bool,
    }

    impl Transport for EchoTransport {
        fn exchange(&mut self, request: [u8; 8], timeout: Duration) -> Result<[u8; 8], Error> {
            let request = Message::decode(request);
            self.requests.push(request);
            self.timeouts.push(timeout);
            let value = match request.opcode {
                OP_GET_ABI => u32::from(ABI_MAJOR) << 16 | u32::from(ABI_MINOR),
                OP_GET_CAPABILITIES => EXPECTED_CAPABILITIES_U32,
                _ if self.wrong_value => request.value ^ 1,
                _ => request.value,
            };
            Ok(Message {
                service: SERVICE_CONTROL,
                opcode: request.opcode | OP_RESPONSE,
                sequence: request
                    .sequence
                    .wrapping_add(u16::from(self.wrong_sequence)),
                value,
            }
            .encode())
        }
    }

    #[test]
    fn wire_format_is_exactly_eight_bytes_little_endian() {
        let message = Message {
            service: 1,
            opcode: 3,
            sequence: 0x1234,
            value: 0x89ab_cdef,
        };
        let encoded = [1, 3, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89];
        assert_eq!(message.encode(), encoded);
        assert_eq!(Message::decode(encoded), message);
    }

    #[test]
    fn fake_transport_exercises_queries_and_sequence_progression() {
        let mut client = Client::with_sequence(EchoTransport::default(), 0xfffe);
        let timeout = Duration::from_millis(20);
        assert_eq!(
            client.abi(timeout).unwrap(),
            AbiVersion {
                major: ABI_MAJOR,
                minor: ABI_MINOR,
            }
        );
        assert_eq!(
            client.capabilities(timeout).unwrap(),
            EXPECTED_CAPABILITIES_U32
        );
        assert_eq!(client.ping(0xdead_beef, timeout).unwrap(), 0xdead_beef);
        let transport = client.into_inner();
        assert_eq!(
            transport
                .requests
                .iter()
                .map(|request| request.sequence)
                .collect::<Vec<_>>(),
            [0xfffe, 0xffff, 0]
        );
        assert_eq!(transport.requests[0].encode().len(), 8);
        assert_eq!(transport.timeouts, [timeout, timeout, timeout]);
    }

    #[test]
    fn mailbox_benchmark_validates_an_entire_sequence_wrap() {
        let mut client = Client::new(EchoTransport::default());
        let timeout = Duration::from_millis(20);
        assert_eq!(client.benchmark(65537, timeout).unwrap().count, 65537);
        let transport = client.into_inner();
        for (iteration, request) in transport.requests.iter().enumerate() {
            assert_eq!(request.sequence, (iteration as u16).wrapping_add(1));
            assert_eq!(request.value, iteration as u32);
            assert_eq!(transport.timeouts[iteration], timeout);
        }
    }

    #[test]
    fn mailbox_poll_path_rejects_short_replies_and_times_out() {
        let (echo, peer) = echo_pair();
        let mut transport = DeviceTransport { fd: echo.fd };
        let worker = std::thread::spawn(move || {
            let mut request = [0_u8; 8];
            assert_eq!(peer.recv(&mut request).unwrap(), 8);
            peer.send(&request[..7]).unwrap();
        });
        assert_eq!(
            transport.exchange([0; 8], Duration::from_secs(1)),
            Err(Error::ShortRead(7))
        );
        worker.join().unwrap();

        let (echo, _peer) = echo_pair();
        let mut transport = DeviceTransport { fd: echo.fd };
        assert_eq!(
            transport.exchange([0; 8], Duration::from_millis(10)),
            Err(Error::Timeout("response"))
        );
    }

    #[test]
    fn wrong_sequence_is_rejected() {
        let transport = EchoTransport {
            wrong_sequence: true,
            ..EchoTransport::default()
        };
        let error = Client::new(transport)
            .ping(7, Duration::from_millis(20))
            .unwrap_err();
        assert_eq!(
            error,
            Error::UnexpectedSequence {
                expected: 1,
                actual: 2
            }
        );
    }

    #[test]
    fn wrong_ping_value_is_rejected() {
        let transport = EchoTransport {
            wrong_value: true,
            ..EchoTransport::default()
        };
        let error = Client::new(transport)
            .ping(7, Duration::from_millis(20))
            .unwrap_err();
        assert_eq!(
            error,
            Error::UnexpectedValue {
                expected: 7,
                actual: 6
            }
        );
    }

    struct FixedReply(Message);

    impl Transport for FixedReply {
        fn exchange(&mut self, _request: [u8; 8], _timeout: Duration) -> Result<[u8; 8], Error> {
            Ok(self.0.encode())
        }
    }

    #[test]
    fn wrong_service_and_opcode_are_rejected() {
        let wrong_service = Message {
            service: 2,
            opcode: OP_PING | OP_RESPONSE,
            sequence: 1,
            value: 7,
        };
        assert_eq!(
            Client::new(FixedReply(wrong_service))
                .ping(7, Duration::from_millis(20))
                .unwrap_err(),
            Error::UnexpectedService {
                expected: SERVICE_CONTROL,
                actual: 2,
            }
        );

        let wrong_opcode = Message {
            service: SERVICE_CONTROL,
            opcode: OP_GET_ABI | OP_RESPONSE,
            sequence: 1,
            value: 7,
        };
        assert_eq!(
            Client::new(FixedReply(wrong_opcode))
                .ping(7, Duration::from_millis(20))
                .unwrap_err(),
            Error::UnexpectedOpcode {
                expected: OP_PING | OP_RESPONSE,
                actual: OP_GET_ABI | OP_RESPONSE,
            }
        );
    }

    #[test]
    fn firmware_error_response_is_not_treated_as_success() {
        let reply = Message {
            service: SERVICE_CONTROL,
            opcode: OP_ERROR,
            sequence: 1,
            value: 0x55,
        };
        assert_eq!(
            Client::new(FixedReply(reply))
                .ping(7, Duration::from_millis(20))
                .unwrap_err(),
            Error::RemoteError {
                sequence: 1,
                value: 0x55,
            }
        );
    }

    #[test]
    fn percentiles_use_nearest_rank() {
        let mut samples = (1..=100)
            .rev()
            .map(Duration::from_nanos)
            .collect::<Vec<_>>();
        let stats = LatencyStats::from_samples(&mut samples).unwrap();
        assert_eq!(stats.count, 100);
        assert_eq!(stats.p50, Duration::from_nanos(50));
        assert_eq!(stats.p99, Duration::from_nanos(99));
        assert_eq!(stats.max, Duration::from_nanos(100));
    }
}
