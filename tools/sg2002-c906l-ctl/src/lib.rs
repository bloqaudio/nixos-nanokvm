use std::error::Error as StdError;
use std::fmt;
use std::path::Path;
use std::time::{Duration, Instant};

use rustix::event::{PollFd, PollFlags, Timespec, poll};
use rustix::fd::OwnedFd;
use rustix::fs::{Mode, OFlags, open};
use rustix::io::{Errno, read, write};

pub const SERVICE_CONTROL: u8 = 1;
pub const OP_PING: u8 = 0x01;
pub const OP_GET_ABI: u8 = 0x02;
pub const OP_GET_CAPABILITIES: u8 = 0x03;
pub const OP_RESPONSE: u8 = 0x80;
pub const OP_ERROR: u8 = 0xff;

pub const ABI_MAJOR: u16 = 1;
pub const CAP_MAILBOX: u32 = 1 << 0;
pub const CAP_SHMEM_HEARTBEAT: u32 = 1 << 1;
pub const CAP_TIMER4_SELF_TEST: u32 = 1 << 2;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Message {
    pub service: u8,
    pub opcode: u8,
    pub sequence: u16,
    pub value: u32,
}

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
    Timeout(&'static str),
    ShortWrite(usize),
    ShortRead(usize),
    UnexpectedService { expected: u8, actual: u8 },
    UnexpectedOpcode { expected: u8, actual: u8 },
    UnexpectedSequence { expected: u16, actual: u16 },
    UnexpectedValue { expected: u32, actual: u32 },
    RemoteError { sequence: u16, value: u32 },
    InvalidArgument(&'static str),
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Transport(detail) => write!(formatter, "transport error: {detail}"),
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
            Self::InvalidArgument(detail) => formatter.write_str(detail),
        }
    }
}

impl StdError for Error {}

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
                OP_GET_ABI => u32::from(ABI_MAJOR) << 16,
                OP_GET_CAPABILITIES => CAP_MAILBOX | CAP_SHMEM_HEARTBEAT,
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
            AbiVersion { major: 1, minor: 0 }
        );
        assert_eq!(
            client.capabilities(timeout).unwrap(),
            CAP_MAILBOX | CAP_SHMEM_HEARTBEAT
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
