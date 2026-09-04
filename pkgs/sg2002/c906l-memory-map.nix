# One source of truth for the SG2002 256 MiB board's top-of-RAM C906L
# reservation.  Consumers still verify their generated binary formats (ELF,
# FIP, DTB and U-Boot .config) so a value cannot silently disappear between
# build stages.
{
  dramEnd = 2415919104; # 0x90000000
  firmwareAddress = 2413821952; # 0x8fe00000
  firmwareSize = 1048576; # 0x00100000
  sharedMemoryAddress = 2414870528; # 0x8ff00000
  sharedMemorySize = 1048576; # 0x00100000

  # The first shared page remains the cacheline status/control area.  RPMsg
  # uses fixed device addresses so the FSBL-started firmware and Linux's
  # attach-only remoteproc driver agree before either side starts touching a
  # vring.  Keep every region page-aligned: Linux maps the metadata and
  # vrings uncached and declares the buffer range as coherent device memory.
  statusAddress = 2414870528; # 0x8ff00000
  statusSize = 4096; # 0x00001000
  resourceTableAddress = 2414874624; # 0x8ff01000
  resourceTableSize = 4096; # 0x00001000
  rpmsgVring0Address = 2414878720; # 0x8ff02000
  rpmsgVring0Size = 16384; # 0x00004000
  rpmsgVring1Address = 2414895104; # 0x8ff06000
  rpmsgVring1Size = 16384; # 0x00004000
  traceAddress = 2414911488; # 0x8ff0a000
  traceSize = 8192; # 0x00002000
  rpmsgBufferAddress = 2414936064; # 0x8ff10000
  rpmsgBufferSize = 262144; # 0x00040000
  bulkAddress = 2415198208; # 0x8ff50000
  bulkSize = 720896; # 0x000b0000
}
