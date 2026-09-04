//! Linux-compatible virtio-RPMsg transport for the FSBL-started C906L.
//!
//! Linux is the virtio driver and owns descriptor/available-ring publication.
//! C906L is the virtio device and owns used-ring publication.  Both sides map
//! the shared DDR uncached or perform explicit cache maintenance; neither side
//! writes the other side's cache lines.

use core::mem::{offset_of, size_of};
use core::ptr::{read_volatile, write_volatile};

use super::{
    SHMEM_BASE, c906l_cache_clean, c906l_cache_invalidate, c906l_delay, c906l_vq_kick_receive,
    c906l_vq_notify, io_fence,
};

const RESOURCE_TABLE_BASE: usize = SHMEM_BASE + 0x1000;
const RESOURCE_TABLE_SIZE: usize = 0x1000;
const VRING0_BASE: usize = SHMEM_BASE + 0x2000;
const VRING1_BASE: usize = SHMEM_BASE + 0x6000;
const VRING_SIZE: usize = 0x4000;
const RPMSG_BUFFER_BASE: usize = SHMEM_BASE + 0x1_0000;
const RPMSG_BUFFER_SIZE: usize = 0x4_0000;
const BULK_BASE: usize = SHMEM_BASE + 0x5_0000;
const BULK_SIZE: usize = 0xb_0000;

const _: () = {
    assert!(RESOURCE_TABLE_BASE + RESOURCE_TABLE_SIZE == VRING0_BASE);
    assert!(VRING0_BASE + VRING_SIZE <= VRING1_BASE);
    assert!(VRING1_BASE + VRING_SIZE <= RPMSG_BUFFER_BASE);
    assert!(RPMSG_BUFFER_BASE + RPMSG_BUFFER_SIZE == BULK_BASE);
    assert!(BULK_BASE + BULK_SIZE == SHMEM_BASE + 0x10_0000);
};

const RING_NUM: usize = 256;
const RING_ALIGN: u32 = 4096;
const RING_DRIVER_BYTES: usize = 0x2000;
const RING_USED_OFFSET: usize = 0x2000;
const RPMSG_BUFFER_BYTES: usize = 512;
const RPMSG_HEADER_BYTES: usize = 16;
const RPMSG_PAYLOAD_BYTES: usize = RPMSG_BUFFER_BYTES - RPMSG_HEADER_BYTES;

const RSC_VDEV: u32 = 3;
const VIRTIO_ID_RPMSG: u32 = 7;
const VIRTIO_RPMSG_F_NS: u32 = 1 << 0;
const VIRTIO_CONFIG_S_DRIVER_OK: u8 = 1 << 2;
const FW_RSC_ADDR_ANY: u32 = u32::MAX;

const VRING_DESC_F_NEXT: u16 = 1;
const VRING_DESC_F_WRITE: u16 = 2;
const VRING_DESC_F_INDIRECT: u16 = 4;

const RPMSG_NS_ADDRESS: u32 = 53;
const RPMSG_ECHO_ADDRESS: u32 = 0x400;
const RPMSG_NS_CREATE: u32 = 0;
const RPMSG_SERVICE_NAME: &[u8] = b"rpmsg_chrdev";

#[repr(C)]
#[derive(Clone, Copy)]
struct ResourceTableHeader {
    version: u32,
    entries: u32,
    reserved: [u32; 2],
    offsets: [u32; 1],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct VdevHeader {
    id: u32,
    notify_id: u32,
    device_features: u32,
    guest_features: u32,
    config_len: u32,
    status: u8,
    vring_count: u8,
    reserved: [u8; 2],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct VringResource {
    device_address: u32,
    align: u32,
    descriptors: u32,
    notify_id: u32,
    physical_address: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RpmsgResource {
    resource_type: u32,
    vdev: VdevHeader,
    vrings: [VringResource; 2],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ResourceTable {
    header: ResourceTableHeader,
    rpmsg: RpmsgResource,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Descriptor {
    address: u64,
    length: u32,
    flags: u16,
    next: u16,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct RpmsgHeader {
    source: u32,
    destination: u32,
    reserved: u32,
    length: u16,
    flags: u16,
}

const _: [(); 20] = [(); size_of::<ResourceTableHeader>()];
const _: [(); 24] = [(); size_of::<VdevHeader>()];
const _: [(); 20] = [(); size_of::<VringResource>()];
const _: [(); 68] = [(); size_of::<RpmsgResource>()];
const _: [(); 88] = [(); size_of::<ResourceTable>()];
const _: [(); 16] = [(); size_of::<Descriptor>()];
const _: [(); 16] = [(); size_of::<RpmsgHeader>()];
const _: [(); 44] = [(); offset_of!(ResourceTable, rpmsg.vdev.status)];

const fn initial_resource_table() -> ResourceTable {
    ResourceTable {
        header: ResourceTableHeader {
            version: 1,
            entries: 1,
            reserved: [0; 2],
            offsets: [size_of::<ResourceTableHeader>() as u32],
        },
        rpmsg: RpmsgResource {
            resource_type: RSC_VDEV,
            vdev: VdevHeader {
                id: VIRTIO_ID_RPMSG,
                notify_id: 0,
                device_features: VIRTIO_RPMSG_F_NS,
                guest_features: 0,
                config_len: 0,
                status: 0,
                vring_count: 2,
                reserved: [0; 2],
            },
            vrings: [
                VringResource {
                    device_address: VRING0_BASE as u32,
                    align: RING_ALIGN,
                    descriptors: RING_NUM as u32,
                    notify_id: FW_RSC_ADDR_ANY,
                    physical_address: 0,
                },
                VringResource {
                    device_address: VRING1_BASE as u32,
                    align: RING_ALIGN,
                    descriptors: RING_NUM as u32,
                    notify_id: FW_RSC_ADDR_ANY,
                    physical_address: 0,
                },
            ],
        },
    }
}

fn invalidate(address: usize, size: usize) {
    // SAFETY: all callers pass a subrange of the permanently reserved shared
    // DDR area.  The vendor primitive rounds as required by the C906 cache.
    unsafe { c906l_cache_invalidate(address, size) };
    io_fence();
}

fn clean(address: usize, size: usize) {
    io_fence();
    // SAFETY: same shared-DDR invariant as `invalidate`.
    unsafe { c906l_cache_clean(address, size) };
    io_fence();
}

fn read_u8(address: usize) -> u8 {
    // SAFETY: callers supply an audited, naturally addressed shared byte.
    unsafe { read_volatile(address as *const u8) }
}

fn read_u16(address: usize) -> u16 {
    debug_assert_eq!(address & 1, 0);
    // SAFETY: callers supply an audited, naturally aligned shared word.
    u16::from_le(unsafe { read_volatile(address as *const u16) })
}

fn read_u32(address: usize) -> u32 {
    debug_assert_eq!(address & 3, 0);
    // SAFETY: callers supply an audited, naturally aligned shared word.
    u32::from_le(unsafe { read_volatile(address as *const u32) })
}

fn read_u64(address: usize) -> u64 {
    debug_assert_eq!(address & 7, 0);
    // SAFETY: callers supply an audited, naturally aligned shared word.
    u64::from_le(unsafe { read_volatile(address as *const u64) })
}

fn write_u8(address: usize, value: u8) {
    // SAFETY: callers supply an audited shared byte owned by C906L.
    unsafe { write_volatile(address as *mut u8, value) };
}

fn write_u16(address: usize, value: u16) {
    debug_assert_eq!(address & 1, 0);
    // SAFETY: callers supply an audited, naturally aligned shared word owned
    // by C906L.
    unsafe { write_volatile(address as *mut u16, value.to_le()) };
}

fn write_u32(address: usize, value: u32) {
    debug_assert_eq!(address & 3, 0);
    // SAFETY: callers supply an audited, naturally aligned shared word owned
    // by C906L.
    unsafe { write_volatile(address as *mut u32, value.to_le()) };
}

fn write_bytes(address: usize, bytes: &[u8]) {
    for (offset, byte) in bytes.iter().copied().enumerate() {
        write_u8(address + offset, byte);
    }
}

fn read_bytes(address: usize, bytes: &mut [u8]) {
    for (offset, byte) in bytes.iter_mut().enumerate() {
        *byte = read_u8(address + offset);
    }
}

/// Publish the resource table before the firmware advertises RPMsg support.
pub(crate) fn initialize() {
    for offset in 0..RESOURCE_TABLE_SIZE {
        write_u8(RESOURCE_TABLE_BASE + offset, 0);
    }
    // SAFETY: the table is naturally aligned, POD, and entirely contained in
    // the resource-table page reserved by the cross-component memory map.
    unsafe {
        write_volatile(
            RESOURCE_TABLE_BASE as *mut ResourceTable,
            initial_resource_table(),
        )
    };
    clean(RESOURCE_TABLE_BASE, RESOURCE_TABLE_SIZE);
}

fn vdev_online() -> bool {
    invalidate(RESOURCE_TABLE_BASE, 64);
    let status = read_u8(RESOURCE_TABLE_BASE + offset_of!(ResourceTable, rpmsg.vdev.status));
    let negotiated =
        read_u32(RESOURCE_TABLE_BASE + offset_of!(ResourceTable, rpmsg.vdev.guest_features));
    status & VIRTIO_CONFIG_S_DRIVER_OK != 0 && negotiated & !VIRTIO_RPMSG_F_NS == 0
}

fn descriptor(ring: usize, index: u16) -> Descriptor {
    let address = ring + usize::from(index) * size_of::<Descriptor>();
    Descriptor {
        address: read_u64(address),
        length: read_u32(address + 8),
        flags: read_u16(address + 12),
        next: read_u16(address + 14),
    }
}

fn descriptor_buffer(descriptor: Descriptor, write: bool, minimum: usize) -> Option<usize> {
    let disallowed = VRING_DESC_F_NEXT | VRING_DESC_F_INDIRECT;
    if descriptor.flags & disallowed != 0
        || (descriptor.flags & VRING_DESC_F_WRITE != 0) != write
        || descriptor.length < minimum as u32
        || descriptor.length > RPMSG_BUFFER_BYTES as u32
    {
        return None;
    }

    let address = usize::try_from(descriptor.address).ok()?;
    let end = address.checked_add(descriptor.length as usize)?;
    let pool_end = RPMSG_BUFFER_BASE.checked_add(RPMSG_BUFFER_SIZE)?;
    if address < RPMSG_BUFFER_BASE || end > pool_end {
        return None;
    }
    Some(address)
}

fn avail_index_address(ring: usize) -> usize {
    ring + RING_NUM * size_of::<Descriptor>() + 2
}

fn avail_entry_address(ring: usize, cursor: u16) -> usize {
    ring + RING_NUM * size_of::<Descriptor>()
        + 4
        + usize::from(cursor) % RING_NUM * size_of::<u16>()
}

fn publish_used(ring: usize, cursor: u16, descriptor_id: u16, length: u32) {
    let used = ring + RING_USED_OFFSET;
    let entry = used + 4 + usize::from(cursor) % RING_NUM * 8;
    write_u32(entry, u32::from(descriptor_id));
    write_u32(entry + 4, length);
    clean(entry, 8);

    let next = cursor.wrapping_add(1);
    write_u16(used + 2, next);
    clean(used, 64);
}

fn encode_header(header: RpmsgHeader, output: &mut [u8; RPMSG_HEADER_BYTES]) {
    output[0..4].copy_from_slice(&header.source.to_le_bytes());
    output[4..8].copy_from_slice(&header.destination.to_le_bytes());
    output[8..12].copy_from_slice(&header.reserved.to_le_bytes());
    output[12..14].copy_from_slice(&header.length.to_le_bytes());
    output[14..16].copy_from_slice(&header.flags.to_le_bytes());
}

fn decode_header(input: &[u8; RPMSG_HEADER_BYTES]) -> RpmsgHeader {
    RpmsgHeader {
        source: u32::from_le_bytes(input[0..4].try_into().unwrap()),
        destination: u32::from_le_bytes(input[4..8].try_into().unwrap()),
        reserved: u32::from_le_bytes(input[8..12].try_into().unwrap()),
        length: u16::from_le_bytes(input[12..14].try_into().unwrap()),
        flags: u16::from_le_bytes(input[14..16].try_into().unwrap()),
    }
}

struct Transport {
    online: bool,
    announced: bool,
    rx_available: u16,
    rx_used: u16,
    tx_available: u16,
    tx_used: u16,
    pending_notifications: u8,
    next_notification: u32,
}

impl Transport {
    const fn new() -> Self {
        Self {
            online: false,
            announced: false,
            rx_available: 0,
            rx_used: 0,
            tx_available: 0,
            tx_used: 0,
            pending_notifications: 0,
            next_notification: 0,
        }
    }

    fn reset_for_attach(&mut self) {
        self.online = true;
        self.announced = false;
        self.rx_available = 0;
        self.rx_used = 0;
        self.tx_available = 0;
        self.tx_used = 0;
        self.pending_notifications = 0;
        self.next_notification = 0;
    }

    fn queue_notification(&mut self, vqid: u32) {
        self.pending_notifications |= 1_u8 << vqid;
    }

    fn flush_notification(&mut self) {
        for offset in 0..2 {
            let vqid = (self.next_notification + offset) % 2;
            let bit = 1_u8 << vqid;
            if self.pending_notifications & bit == 0 {
                continue;
            }

            // SAFETY: channel 2 is exclusively assigned to C906L-to-Linux
            // RPMsg notifications. A busy channel is retried on every task
            // iteration so a published used entry cannot lose its wakeup.
            if unsafe { c906l_vq_notify(vqid) } == 0 {
                self.pending_notifications &= !bit;
                self.next_notification = (vqid + 1) % 2;
            }
            break;
        }
    }

    fn send(&mut self, source: u32, destination: u32, payload: &[u8]) -> bool {
        if payload.len() > RPMSG_PAYLOAD_BYTES {
            return false;
        }

        invalidate(VRING0_BASE, RING_DRIVER_BYTES);
        let available = read_u16(avail_index_address(VRING0_BASE));
        if self.rx_available == available {
            return false;
        }

        let descriptor_id = read_u16(avail_entry_address(VRING0_BASE, self.rx_available));
        if usize::from(descriptor_id) >= RING_NUM {
            return false;
        }
        let descriptor = descriptor(VRING0_BASE, descriptor_id);
        let Some(buffer) = descriptor_buffer(
            descriptor,
            true,
            RPMSG_HEADER_BYTES.saturating_add(payload.len()),
        ) else {
            return false;
        };

        let header = RpmsgHeader {
            source,
            destination,
            reserved: 0,
            length: payload.len() as u16,
            flags: 0,
        };
        let mut encoded = [0_u8; RPMSG_HEADER_BYTES];
        encode_header(header, &mut encoded);
        write_bytes(buffer, &encoded);
        write_bytes(buffer + RPMSG_HEADER_BYTES, payload);
        let message_size = RPMSG_HEADER_BYTES + payload.len();
        clean(buffer, message_size);

        publish_used(
            VRING0_BASE,
            self.rx_used,
            descriptor_id,
            message_size as u32,
        );
        self.rx_available = self.rx_available.wrapping_add(1);
        self.rx_used = self.rx_used.wrapping_add(1);
        true
    }

    fn announce(&mut self) -> bool {
        let mut announcement = [0_u8; 40];
        announcement[..RPMSG_SERVICE_NAME.len()].copy_from_slice(RPMSG_SERVICE_NAME);
        announcement[32..36].copy_from_slice(&RPMSG_ECHO_ADDRESS.to_le_bytes());
        announcement[36..40].copy_from_slice(&RPMSG_NS_CREATE.to_le_bytes());
        self.send(RPMSG_NS_ADDRESS, RPMSG_NS_ADDRESS, &announcement)
    }

    fn drain_host_messages(&mut self) -> (bool, bool) {
        let mut sent = false;
        let mut completed = false;

        for _ in 0..RING_NUM {
            invalidate(VRING1_BASE, RING_DRIVER_BYTES);
            let available = read_u16(avail_index_address(VRING1_BASE));
            if self.tx_available == available {
                break;
            }

            let descriptor_id = read_u16(avail_entry_address(VRING1_BASE, self.tx_available));
            if usize::from(descriptor_id) >= RING_NUM {
                break;
            }
            let descriptor = descriptor(VRING1_BASE, descriptor_id);
            let Some(buffer) = descriptor_buffer(descriptor, false, RPMSG_HEADER_BYTES) else {
                publish_used(VRING1_BASE, self.tx_used, descriptor_id, 0);
                self.tx_available = self.tx_available.wrapping_add(1);
                self.tx_used = self.tx_used.wrapping_add(1);
                completed = true;
                continue;
            };

            invalidate(buffer, descriptor.length as usize);
            let mut encoded = [0_u8; RPMSG_HEADER_BYTES];
            read_bytes(buffer, &mut encoded);
            let header = decode_header(&encoded);
            let payload_len = usize::from(header.length);
            let valid = header.reserved == 0
                && header.flags == 0
                && payload_len <= RPMSG_PAYLOAD_BYTES
                && RPMSG_HEADER_BYTES + payload_len <= descriptor.length as usize;

            if valid && header.destination == RPMSG_ECHO_ADDRESS {
                let mut payload = [0_u8; RPMSG_PAYLOAD_BYTES];
                read_bytes(buffer + RPMSG_HEADER_BYTES, &mut payload[..payload_len]);
                if !self.send(RPMSG_ECHO_ADDRESS, header.source, &payload[..payload_len]) {
                    break;
                }
                sent = true;
            }

            publish_used(VRING1_BASE, self.tx_used, descriptor_id, 0);
            self.tx_available = self.tx_available.wrapping_add(1);
            self.tx_used = self.tx_used.wrapping_add(1);
            completed = true;
        }

        (sent, completed)
    }

    fn service(&mut self) {
        let online = vdev_online();
        if !online {
            self.online = false;
            return;
        }
        if !self.online {
            self.reset_for_attach();
        }

        let mut rx_changed = false;
        if !self.announced && self.announce() {
            self.announced = true;
            rx_changed = true;
        }
        let (sent, completed) = self.drain_host_messages();
        rx_changed |= sent;

        if rx_changed {
            self.queue_notification(0);
        }
        if completed {
            self.queue_notification(1);
        }
        self.flush_notification();
    }
}

pub(crate) fn run() -> ! {
    let mut transport = Transport::new();
    loop {
        transport.service();
        let mut kick = 0_u32;
        // SAFETY: the C shim owns the queue and writes exactly one u32 when a
        // kick arrives.  A one-tick timeout makes a lost doorbell recoverable.
        let _ = unsafe { c906l_vq_kick_receive(&mut kick, 1) };
        // Keep the compiler from considering the payload irrelevant: it is a
        // diagnostic notify id even though service() drains both rings.
        core::hint::black_box(kick);
        // SAFETY: yielding the RPMsg task is valid after scheduler startup.
        unsafe { c906l_delay(0) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resource_table_matches_linux_uapi_layout() {
        let table = initial_resource_table();
        assert_eq!(size_of::<ResourceTable>(), 88);
        assert_eq!(table.header.version, 1);
        assert_eq!(table.header.entries, 1);
        assert_eq!(table.header.offsets, [20]);
        assert_eq!(table.rpmsg.resource_type, RSC_VDEV);
        assert_eq!(table.rpmsg.vdev.id, VIRTIO_ID_RPMSG);
        assert_eq!(table.rpmsg.vdev.vring_count, 2);
        assert_eq!(table.rpmsg.vrings[0].device_address, VRING0_BASE as u32);
        assert_eq!(table.rpmsg.vrings[1].device_address, VRING1_BASE as u32);
    }

    #[test]
    fn all_transport_regions_are_disjoint_and_fit() {
        assert_eq!(RESOURCE_TABLE_BASE + RESOURCE_TABLE_SIZE, VRING0_BASE);
        assert!(VRING0_BASE + VRING_SIZE <= VRING1_BASE);
        assert!(VRING1_BASE + VRING_SIZE <= RPMSG_BUFFER_BASE);
        assert_eq!(RPMSG_BUFFER_BASE + RPMSG_BUFFER_SIZE, BULK_BASE);
        assert_eq!(BULK_BASE + BULK_SIZE, SHMEM_BASE + 0x10_0000);
    }

    #[test]
    fn header_round_trip_is_little_endian() {
        let expected = RpmsgHeader {
            source: 0x1122_3344,
            destination: 0x5566_7788,
            reserved: 0,
            length: 496,
            flags: 0,
        };
        let mut encoded = [0_u8; RPMSG_HEADER_BYTES];
        encode_header(expected, &mut encoded);
        assert_eq!(decode_header(&encoded), expected);
        assert_eq!(&encoded[0..4], &[0x44, 0x33, 0x22, 0x11]);
    }

    #[test]
    fn buffer_validation_rejects_chains_direction_and_escape() {
        let valid = Descriptor {
            address: RPMSG_BUFFER_BASE as u64,
            length: RPMSG_BUFFER_BYTES as u32,
            flags: VRING_DESC_F_WRITE,
            next: 0,
        };
        assert_eq!(
            descriptor_buffer(valid, true, RPMSG_BUFFER_BYTES),
            Some(RPMSG_BUFFER_BASE)
        );
        assert_eq!(descriptor_buffer(valid, false, 16), None);
        assert_eq!(
            descriptor_buffer(
                Descriptor {
                    flags: VRING_DESC_F_WRITE | VRING_DESC_F_NEXT,
                    ..valid
                },
                true,
                16
            ),
            None
        );
        assert_eq!(
            descriptor_buffer(
                Descriptor {
                    address: (RPMSG_BUFFER_BASE - 1) as u64,
                    ..valid
                },
                true,
                16
            ),
            None
        );
    }
}
