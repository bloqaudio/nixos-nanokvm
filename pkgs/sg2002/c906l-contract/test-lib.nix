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
assert base.sha256 == "f1168ec929220e57889c1d5c6218f4422600cf2f53c831a55bebf5ede0017a43";
assert timer4.sha256 == "6ef871536468cc39e3931fa9c529710c5c4627cdea6cd81e96b16db3565e3060";
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
