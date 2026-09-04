{ lib }:

let
  contract = import ./lib.nix { inherit lib; };
  base = contract.resolveProfile "base";
  timer4 = contract.resolveProfile "timer4";
  timer5 = contract.resolveProfile "timer5";
  timer6 = contract.resolveProfile "timer6";
  timer7 = contract.resolveProfile "timer7";
  allTimers = contract.resolvePeripherals [ "timer7" "timer5" "timer4" "timer6" ];
  forceTry = value: builtins.tryEval (builtins.deepSeq value true);
  unknownProfile = forceTry (contract.resolveProfile "missing");
  duplicatePeripheral = forceTry (contract.resolvePeripherals [ "timer4" "timer4" ]);
  unknownPeripheral = forceTry (contract.resolvePeripherals [ "uart99" ]);
in
assert base.expectedCapabilities == 11;
assert timer4.expectedCapabilities == 15;
assert timer5.expectedCapabilities == 27;
assert timer6.expectedCapabilities == 43;
assert timer7.expectedCapabilities == 75;
assert allTimers.expectedCapabilities == 127;
assert base.sha256 == "ceea0d1660a09a2e064617228ad68ebfc3d09ff5a9dd335f29206f10e6edf212";
assert timer4.sha256 == "977b3662069ac92b05c457f2ed6064c0bf6a139e71f2798e6d53218b450262dd";
assert timer5.sha256 == "d163093d9e104375c58b5e90d98e386129903df54488fc85451ce2c04dee6c0b";
assert timer6.sha256 == "bc9f57dd2d10d9f9ed952670a679529a32e4a882a0c17809f5bfe897e7ad5dd7";
assert timer7.sha256 == "cd585e4c6426d34baeb133464bddb03fbfe6f6dcac78704c28ca33b60c90331f";
assert base.sha256 != timer4.sha256;
assert base.sortedPeripherals == [ ];
assert timer4.sortedPeripherals == [ "timer4" ];
assert timer5.sortedPeripherals == [ "timer5" ];
assert timer6.sortedPeripherals == [ "timer6" ];
assert timer7.sortedPeripherals == [ "timer7" ];
assert allTimers.sortedPeripherals == [ "timer4" "timer5" "timer6" "timer7" ];
assert base.dormantCapabilities == 11;
assert timer4.dormantCapabilities == 11;
assert base.leaseMask == 0;
assert timer4.leaseMask == 1;
assert timer5.leaseMask == 2;
assert timer6.leaseMask == 4;
assert timer7.leaseMask == 8;
assert allTimers.leaseMask == 15;
assert base.profileId == 1;
assert timer4.profileId == 2;
assert timer5.profileId == 3;
assert timer6.profileId == 5;
assert timer7.profileId == 9;
assert allTimers.profileId == 16;
assert base.manifestFlags == 2;
assert timer4.manifestFlags == 3;
assert timer5.manifestFlags == 3;
assert timer6.manifestFlags == 3;
assert timer7.manifestFlags == 3;
assert allTimers.manifestFlags == 3;
assert (contract.resolvePeripherals [ ]).profileName == "base";
assert (contract.resolvePeripherals [ "timer4" ]).profileName == "timer4";
assert (contract.resolvePeripherals [ "timer5" ]).profileName == "timer5";
assert (contract.resolvePeripherals [ "timer6" ]).profileName == "timer6";
assert (contract.resolvePeripherals [ "timer7" ]).profileName == "timer7";
assert allTimers.profileName == "custom-timer4-timer5-timer6-timer7";
assert !unknownProfile.success;
assert !duplicatePeripheral.success;
assert !unknownPeripheral.success;
assert base.resolvedContract.memory.firmware.address == 2413821952;
assert base.resolvedContract.memory.shared.address == 2414870528;
assert base.resolvedContract.memory.shared.regions.bulk.offset
  + base.resolvedContract.memory.shared.regions.bulk.size
  == base.resolvedContract.memory.shared.size;
{
  inherit
    allTimers
    base
    timer4
    timer5
    timer6
    timer7
    ;
}
