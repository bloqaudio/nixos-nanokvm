{ lib }:

let
  contractLib = import ./c906l-contract/lib.nix { inherit lib; };
  contract = (contractLib.resolveProfile "base").resolvedContract;
  firmware = contract.memory.firmware;
  shared = contract.memory.shared;
  region = name: shared.regions.${name};
  regionAddress = name: shared.address + (region name).offset;
in
{
  # Compatibility view for binary-format consumers.  The canonical values,
  # validation, profiles, and ownership metadata live in c906l-contract.
  dramEnd = contract.soc.dram.address + contract.soc.dram.size;
  firmwareAddress = firmware.address;
  firmwareSize = firmware.size;
  sharedMemoryAddress = shared.address;
  sharedMemorySize = shared.size;

  statusAddress = regionAddress "status";
  statusSize = (region "status").size;
  resourceTableAddress = regionAddress "resourceTable";
  resourceTableSize = (region "resourceTable").size;
  rpmsgVring0Address = regionAddress "rpmsgVring0";
  rpmsgVring0Size = (region "rpmsgVring0").size;
  rpmsgVring1Address = regionAddress "rpmsgVring1";
  rpmsgVring1Size = (region "rpmsgVring1").size;
  traceAddress = regionAddress "trace";
  traceSize = (region "trace").size;
  rpmsgBufferAddress = regionAddress "rpmsgBuffer";
  rpmsgBufferSize = (region "rpmsgBuffer").size;
  bulkAddress = regionAddress "bulk";
  bulkSize = (region "bulk").size;
}
