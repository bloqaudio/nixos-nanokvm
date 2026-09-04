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
}
