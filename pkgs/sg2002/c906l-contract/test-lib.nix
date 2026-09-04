{ lib }:

let
  contract = import ./lib.nix { inherit lib; };
  base = contract.resolveProfile "base";
  timer4 = contract.resolveProfile "timer4";
  forceTry = value: builtins.tryEval (builtins.deepSeq value true);
  unknownProfile = forceTry (contract.resolveProfile "missing");
  duplicatePeripheral = forceTry (contract.resolvePeripherals [ "timer4" "timer4" ]);
  unknownPeripheral = forceTry (contract.resolvePeripherals [ "uart99" ]);
in
assert base.expectedCapabilities == 11;
assert timer4.expectedCapabilities == 15;
assert base.sha256 == "bffc269cedbd5e7b1e681749fbac5f2f67b3a8193d9b9828874a64c26a3d53ef";
assert timer4.sha256 == "49ddcde2653c3b194ad2349654f6d984a1bc5f0baf55e0d0dcbbc6cfd5bbf97e";
assert base.sha256 != timer4.sha256;
assert base.sortedPeripherals == [ ];
assert timer4.sortedPeripherals == [ "timer4" ];
assert base.dormantCapabilities == 11;
assert timer4.dormantCapabilities == 11;
assert base.leaseMask == 0;
assert timer4.leaseMask == 1;
assert base.profileId == 1;
assert timer4.profileId == 2;
assert base.manifestFlags == 2;
assert timer4.manifestFlags == 3;
assert (contract.resolvePeripherals [ ]).profileName == "base";
assert (contract.resolvePeripherals [ "timer4" ]).profileName == "timer4";
assert !unknownProfile.success;
assert !duplicatePeripheral.success;
assert !unknownPeripheral.success;
assert base.resolvedContract.memory.firmware.address == 2413821952;
assert base.resolvedContract.memory.shared.address == 2414870528;
assert base.resolvedContract.memory.shared.regions.bulk.offset
  + base.resolvedContract.memory.shared.regions.bulk.size
  == base.resolvedContract.memory.shared.size;
{
  inherit base timer4;
}
