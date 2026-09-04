use std::env;
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Duration;

use sg2002_c906l_ctl::{
    ABI_MAJOR, CAP_MAILBOX, CAP_SHMEM_HEARTBEAT, CAP_TIMER4_SELF_TEST, Client, DeviceTransport,
};

const DEFAULT_DEVICE: &str = "/dev/sg2002-c906l-control";
const DEFAULT_TIMEOUT_MS: u64 = 1_000;
const MAX_TIMEOUT_MS: u64 = 60_000;
const DEFAULT_BENCHMARK_COUNT: usize = 1_000;
const MAX_BENCHMARK_COUNT: usize = 1_000_000;
const CHECK_PING_VALUE: u32 = 0x4d56_4b4e;

const USAGE: &str = "\
Usage: sg2002-c906l-ctl [OPTIONS] [COMMAND]\n\
\n\
Options:\n\
  --device PATH       Control device (default: /dev/sg2002-c906l-control)\n\
  --timeout-ms MS     Per-transaction deadline, 1..60000 (default: 1000)\n\
  -h, --help          Show this help\n\
\n\
Commands:\n\
  check               Query ABI/capabilities and ping (default)\n\
  abi                 Query the firmware ABI version\n\
  capabilities        Query and decode capability bits\n\
  ping [VALUE]        Round-trip a u32 value (decimal or 0x-prefixed)\n\
  bench [COUNT]       Sequential ping latency stress test (default: 1000)\n\
  stress [COUNT]      Alias for bench\n";

#[derive(Debug, Eq, PartialEq)]
enum Command {
    Check,
    Abi,
    Capabilities,
    Ping(u32),
    Benchmark(usize),
}

#[derive(Debug, Eq, PartialEq)]
struct Config {
    device: PathBuf,
    timeout: Duration,
    command: Command,
}

fn parse_integer(text: &str) -> Result<u64, String> {
    if let Some(hex) = text.strip_prefix("0x").or_else(|| text.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).map_err(|_| format!("invalid integer: {text}"))
    } else {
        text.parse().map_err(|_| format!("invalid integer: {text}"))
    }
}

fn take_value(arguments: &[String], index: &mut usize, option: &str) -> Result<String, String> {
    *index += 1;
    arguments
        .get(*index)
        .cloned()
        .ok_or_else(|| format!("{option} requires a value"))
}

fn parse_arguments(arguments: &[String]) -> Result<Option<Config>, String> {
    let mut device = PathBuf::from(DEFAULT_DEVICE);
    let mut timeout_ms = DEFAULT_TIMEOUT_MS;
    let mut positional = Vec::new();
    let mut index = 0;

    while let Some(argument) = arguments.get(index) {
        match argument.as_str() {
            "-h" | "--help" => return Ok(None),
            "--device" => {
                device = take_value(&arguments, &mut index, "--device")?.into();
                index += 1;
            }
            "--timeout-ms" => {
                let value = take_value(&arguments, &mut index, "--timeout-ms")?;
                timeout_ms = parse_integer(&value)?;
                index += 1;
            }
            "--" => {
                positional.extend_from_slice(&arguments[index + 1..]);
                break;
            }
            _ if argument.starts_with('-') => {
                return Err(format!("unknown option: {argument}"));
            }
            _ => {
                positional.push(argument.clone());
                index += 1;
            }
        }
    }

    if !(1..=MAX_TIMEOUT_MS).contains(&timeout_ms) {
        return Err(format!("--timeout-ms must be in 1..={MAX_TIMEOUT_MS}"));
    }

    let command_name = positional.first().map(String::as_str).unwrap_or("check");
    if positional.len() > 2 {
        return Err(format!("unexpected argument: {}", positional[2]));
    }
    let value_argument = positional.get(1);
    let command = match command_name {
        "check" if value_argument.is_none() => Command::Check,
        "abi" if value_argument.is_none() => Command::Abi,
        "capabilities" | "caps" if value_argument.is_none() => Command::Capabilities,
        "ping" => {
            let value = value_argument
                .map(|text| parse_integer(text))
                .transpose()?
                .unwrap_or(CHECK_PING_VALUE.into());
            let value = u32::try_from(value).map_err(|_| "ping value exceeds u32".to_owned())?;
            Command::Ping(value)
        }
        "bench" | "stress" => {
            let count = value_argument
                .map(|text| parse_integer(text))
                .transpose()?
                .unwrap_or(DEFAULT_BENCHMARK_COUNT as u64);
            let count =
                usize::try_from(count).map_err(|_| "benchmark count exceeds usize".to_owned())?;
            if !(1..=MAX_BENCHMARK_COUNT).contains(&count) {
                return Err(format!(
                    "benchmark count must be in 1..={MAX_BENCHMARK_COUNT}"
                ));
            }
            Command::Benchmark(count)
        }
        "check" | "abi" | "capabilities" | "caps" => {
            return Err(format!("unexpected argument: {}", positional[1]));
        }
        unknown => return Err(format!("unknown command: {unknown}")),
    };

    Ok(Some(Config {
        device,
        timeout: Duration::from_millis(timeout_ms),
        command,
    }))
}

fn parse_config() -> Result<Option<Config>, String> {
    parse_arguments(&env::args().skip(1).collect::<Vec<_>>())
}

fn capability_names(bits: u32) -> String {
    let mut names = Vec::new();
    if bits & CAP_MAILBOX != 0 {
        names.push("mailbox");
    }
    if bits & CAP_SHMEM_HEARTBEAT != 0 {
        names.push("shmem-heartbeat");
    }
    if bits & CAP_TIMER4_SELF_TEST != 0 {
        names.push("timer4-self-test");
    }
    let known = CAP_MAILBOX | CAP_SHMEM_HEARTBEAT | CAP_TIMER4_SELF_TEST;
    let unknown = bits & !known;
    if unknown != 0 {
        names.push("unknown");
    }
    names.join(",")
}

fn microseconds(duration: Duration) -> f64 {
    duration.as_secs_f64() * 1_000_000.0
}

fn run(config: Config) -> Result<(), String> {
    let transport = DeviceTransport::open(&config.device).map_err(|error| error.to_string())?;
    let mut client = Client::new(transport);

    match config.command {
        Command::Check => {
            let abi = client
                .abi(config.timeout)
                .map_err(|error| error.to_string())?;
            if abi.major != ABI_MAJOR {
                return Err(format!(
                    "unsupported firmware ABI {}.{} (expected major {ABI_MAJOR})",
                    abi.major, abi.minor
                ));
            }
            println!("abi={}.{}", abi.major, abi.minor);

            let capabilities = client
                .capabilities(config.timeout)
                .map_err(|error| error.to_string())?;
            let required = CAP_MAILBOX | CAP_SHMEM_HEARTBEAT;
            if capabilities & required != required {
                return Err(format!(
                    "required capabilities missing: got 0x{capabilities:08x}, need 0x{required:08x}"
                ));
            }
            println!(
                "capabilities=0x{capabilities:08x} [{}]",
                capability_names(capabilities)
            );

            let reply = client
                .ping(CHECK_PING_VALUE, config.timeout)
                .map_err(|error| error.to_string())?;
            println!("ping=0x{reply:08x}");
            println!("result=ok");
        }
        Command::Abi => {
            let abi = client
                .abi(config.timeout)
                .map_err(|error| error.to_string())?;
            println!("abi={}.{}", abi.major, abi.minor);
        }
        Command::Capabilities => {
            let capabilities = client
                .capabilities(config.timeout)
                .map_err(|error| error.to_string())?;
            println!(
                "capabilities=0x{capabilities:08x} [{}]",
                capability_names(capabilities)
            );
        }
        Command::Ping(value) => {
            let reply = client
                .ping(value, config.timeout)
                .map_err(|error| error.to_string())?;
            println!("ping=0x{reply:08x}");
        }
        Command::Benchmark(count) => {
            let stats = client
                .benchmark(count, config.timeout)
                .map_err(|error| error.to_string())?;
            println!(
                "count={} p50_us={:.3} p99_us={:.3} max_us={:.3}",
                stats.count,
                microseconds(stats.p50),
                microseconds(stats.p99),
                microseconds(stats.max)
            );
        }
    }
    Ok(())
}

fn main() -> ExitCode {
    match parse_config() {
        Ok(None) => {
            print!("{USAGE}");
            ExitCode::SUCCESS
        }
        Ok(Some(config)) => match run(config) {
            Ok(()) => ExitCode::SUCCESS,
            Err(error) => {
                eprintln!("sg2002-c906l-ctl: {error}");
                ExitCode::FAILURE
            }
        },
        Err(error) => {
            eprintln!("sg2002-c906l-ctl: {error}\n\n{USAGE}");
            ExitCode::from(2)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn strings(arguments: &[&str]) -> Vec<String> {
        arguments
            .iter()
            .map(|argument| (*argument).to_owned())
            .collect()
    }

    #[test]
    fn default_is_bounded_check() {
        assert_eq!(
            parse_arguments(&[]).unwrap(),
            Some(Config {
                device: DEFAULT_DEVICE.into(),
                timeout: Duration::from_millis(DEFAULT_TIMEOUT_MS),
                command: Command::Check,
            })
        );
    }

    #[test]
    fn global_options_are_accepted_after_benchmark_arguments() {
        assert_eq!(
            parse_arguments(&strings(&[
                "bench",
                "10000",
                "--timeout-ms",
                "100",
                "--device",
                "/tmp/fake-control",
            ]))
            .unwrap(),
            Some(Config {
                device: "/tmp/fake-control".into(),
                timeout: Duration::from_millis(100),
                command: Command::Benchmark(10_000),
            })
        );
    }

    #[test]
    fn zero_timeout_and_excess_arguments_are_rejected() {
        assert!(parse_arguments(&strings(&["--timeout-ms", "0"])).is_err());
        assert!(parse_arguments(&strings(&["ping", "1", "2"])).is_err());
    }
}
