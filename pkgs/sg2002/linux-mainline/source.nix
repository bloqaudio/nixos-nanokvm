{ fetchurl }:

rec {
  version = "7.2-rc5";
  modDirVersion = "7.2.0-rc5";
  src = fetchurl {
    url = "https://git.kernel.org/torvalds/t/linux-${version}.tar.gz";
    hash = "sha256-i+W/JFxbyJkn8VqfV1wEhpwlqNUR2LgQ7NimflsN1R4=";
  };
}
