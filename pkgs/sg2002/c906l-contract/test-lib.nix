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
assert base.sha256 == "da00e0fdb60c1075198fc839db5a8f61781f8fde7e751d67f9fd65e9aa8d64cf";
assert timer4.sha256 == "6124d2817cc262aebf14bfcbafeedbd055249e90b9d8510d4e41859b7e548e93";
assert base.sha256 != timer4.sha256;
assert base.sortedPeripherals == [ ];
assert timer4.sortedPeripherals == [ "timer4" ];
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
