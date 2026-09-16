# C906L peripheral register drivers

The `sg2002-pac` crate includes executable, host-tested GPIO, UART, I2C and SPI
drivers alongside its timer channel access. These are **not active firmware
services or new leases**. No new profile, capability bit, MMIO instance address,
Linux device-tree handoff or RPMsg endpoint is introduced. Current firmware
continues to own only its existing mailbox and explicit timer leases.

| Family | Implemented operations | Deliberate limits |
| --- | --- | --- |
| GPIO | Bank-exclusive input/output; `embedded-hal` digital traits; initial latch before direction; selected-line interrupt trigger/debounce/acknowledgement | One borrowed pin handle at a time serializes bank RMW; no pinmux, shared-bank access or PLIC handler |
| UART | 8N1 configuration with checked DLAB aliases; nonblocking and bounded byte TX/RX; RX line errors; bounded flush | Board supplies divisor and quiescent RX; no DMA, interrupts, flow control or loopback |
| I2C | Standard-speed seven-bit write, read and repeated-start write/read; raw abort cause; shared poll budget; bounded disable cleanup | Addresses `0x08..0x77`; at most 256 bytes per direction; one read in flight; no DMA, slave mode, scanning or bus recovery |
| SPI | Eight-bit full-duplex transaction under hardware CS0; all four Motorola modes; bounded completion and error cleanup | At most eight bytes per transaction, preloaded before chip select; no FIFO streaming, DMA, GPIO CS or slave mode |

GPIO borrows enforce bank-local serialization in safe Rust. Controller handles
are non-cloneable and expose no raw pointer or register-by-offset API. Their
only MMIO constructors are `unsafe`: board integration must already hold an
activated lease, prove mapping/lifetime and exclusive controller/pin ownership,
and establish clock/reset/pinmux prerequisites. These constructors do not
acquire leases or establish cross-core ownership. A future board lease-token
and singleton layer must enforce those prerequisites before any constructor is
called. Current firmware does not call them.

Private register structs assert offsets and sizes at compile time. `safe-mmio`
distinguishes ordinary readback, destructive reads, write-only acknowledgements
and FIFO aliases. New drivers surround accesses with RISC-V I/O fences. GPIO
writes preserve neighboring bits; EOI writes only the requested mask. UART,
SPI and I2C tests use semantic fake controllers for sequencing, aliases, FIFOs
and failures. GPIO also has an ownership compile-fail doctest. These tests do
not establish physical signal timing, interrupt routing or pin isolation.

## Failure and timing contracts

UART budgets bound status samples per byte/flush/configuration call. Flush waits
for TX FIFO empty and UART idle without reading LSR, preserving RX errors;
incoming traffic can therefore delay flush. Rejected configuration leaves data
operations unavailable. It can have partially changed divisor/latches and does
not roll back ownership to Linux.

SPI preloads the complete transaction into its documented eight-entry FIFO
before selecting CS0, avoiding assumptions about CPU refill keeping CS asserted.
Every started transaction ends with SPI disabled and CS selection cleared,
including errors. A timeout may have clocked a prefix and must not automatically
retry. The poll budget covers the entire receive/completion loop; prefill has
at most eight iterations.

I2C uses one budget for the full transfer and a separate equal budget for disable
acknowledgement. Abort, FIFO and timeout failures invalidate configuration.
`DisableTimeout` means the controller may still be active: retain exclusive
ownership until board policy proves recovery. There is no reset, GPIO recovery
or generic DesignWare ABORT-bit write. A failed transfer may have written a
prefix. Reconfiguration requires renewed proof of bus quiescence.

Poll counts bound work, **not wall-clock time**. Service code must select budgets
appropriate to its clock/scheduler and supervise operations. UART divisors and
I2C timing counts need board electrical/clock validation. These drivers do not
modify shared clock, reset, DMA or pinctrl banks.

## Hardware evidence and next integration

GPIO/UART/I2C/SPI have no silicon validation in this change. Timer validation
remains as recorded in [the main C906L document](sg2002-c906l.md). Disabled Linux
nodes alone do not prove a whole controller and its pins are safe to lease.
Missing steps include a reviewed board pin map, clock/reset read-only
preconditions, Linux controller and client quiescence, matching generated
contract/DT profile, activation service, error supervision and hardware tests.
ISP, CSI, media, camera-control buses and system-critical blocks remain Linux
owned.

UART1 is a possible later lease, but a generic 16550 loopback test is unjustified:
SG2002 TRM v1.02 marks MCR bit 4 **reserved**. Its FAR/TFR/RFW FIFO test mechanism
is different and is not implemented. SPI documents shift-register loopback in
CTRLR0 bit 11, but that does not prove external pins are isolated; this driver
does not expose it.

The long-term HAL/board/service layering in the main document still applies.
These register drivers currently live beside timer access in `sg2002-pac`, with
private transport-independent state machines. GPIO implements `embedded-hal`.
UART, I2C and SPI have narrow APIs reflecting their actual operation/budget
limits; they do not claim full `embedded-io`, `I2c` or `SpiDevice` conformance.

## Controlling sources

Layouts and controller-specific restrictions were checked against Sophgo's
SG2002 TRM v1.02, commit `6da1ee8cad3ab730401cc8f775d690c57017f433`:

- [GPIO operation and overview](https://github.com/sophgo/sophgo-doc/blob/sg2002-trm-v1.02/SG200X/TRM/contents/en/peripherals/gpio.rst) and [register semantics](https://github.com/sophgo/sophgo-doc/blob/sg2002-trm-v1.02/SG200X/TRM/contents/cn/peripherals/gpio_registers_description.table.rst)
- [UART operation](https://github.com/sophgo/sophgo-doc/blob/sg2002-trm-v1.02/SG200X/TRM/contents/en/peripherals/uart.rst) and [register semantics including reserved MCR bit 4](https://github.com/sophgo/sophgo-doc/blob/sg2002-trm-v1.02/SG200X/TRM/contents/cn/peripherals/uart_registers_description.table.rst)
- [I2C register overview](https://github.com/sophgo/sophgo-doc/blob/sg2002-trm-v1.02/SG200X/TRM/contents/cn/peripherals/i2c_registers_overview.table.rst) and [semantics including STOP/RESTART and reserved ENABLE bit 1](https://github.com/sophgo/sophgo-doc/blob/sg2002-trm-v1.02/SG200X/TRM/contents/cn/peripherals/i2c_registers_description.table.rst)
- [SPI register overview](https://github.com/sophgo/sophgo-doc/blob/sg2002-trm-v1.02/SG200X/TRM/contents/cn/peripherals/spi_registers_overview.table.rst) and [semantics including FIFO depth and single CS](https://github.com/sophgo/sophgo-doc/blob/sg2002-trm-v1.02/SG200X/TRM/contents/cn/peripherals/spi_registers_description.table.rst)
