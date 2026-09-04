{
  description = "NixOS image and packages for Sipeed NanoKVM on SG2002";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # Sipeed's megarepo — vendor 5.10 kernel, vendor DTS/defconfig,
    # vendor AIC8800 osdrv tree, Cvitek's fiptool.py. (Was forwarded
    # via the nixos-sg2002 flake input; now a direct input so this
    # flake stands on its own.)
    licheerv-nano-build = {
      url = "git+https://github.com/sipeed/LicheeRV-Nano-Build?submodules=1";
      flake = false;
    };

    # radxa-pkg/aic8800: modern-kernel-compatible rewrite of the
    # vendor AIC8800 driver.
    aic8800-radxa = {
      url = "github:radxa-pkg/aic8800/bd11969265809a0fc948f1107c8256bbb2c1aa60";
      flake = false;
    };

    # AIC8800DC firmware blobs (pinned by Sipeed's Buildroot recipe).
    aic8800-firmware-src = {
      url = "github:lxowalle/aic8800-sdio-firmware/c56f910044cc854d6c553bcb9a644f3bca5a4c38";
      flake = false;
    };

    # Sophgo's fiptool — LZMA B3MA blob format. Bundles FSBL + DDR
    # params under data/ so we don't have to reverse engineer them.
    sophgo-fiptool = {
      url = "github:sophgo/fiptool/7f59889c91f7d5d440d6a09aad0209f0aca3d09d";
      flake = false;
    };

    # Sipeed's NanoKVM userspace. Pinned to the release commit so the
    # flake evaluates from anywhere — used to be a `git+file:` to a
    # local checkout which only resolved on the dev host. The commit
    # message says "release: nanokvm@2.4.1" but there's no matching
    # tag upstream, hence pinning by SHA.
    nanokvm-src = {
      url = "github:sipeed/NanoKVM/2ca5b19efe64266b5bcde7ef167b6961659154d6";
      flake = false;
    };

    disko = {
      url = "github:nix-community/disko";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    # Stateless-root pattern from ../nixos-config: tmpfs / with
    # opt-in bind-mounted state. Nothing enables it on the NFS-live
    # boards (they're fully ephemeral), but the module is wired in so
    # boards that later gain a writable backing can just set
    # nanokvm.impermanence.enable.
    impermanence = {
      url = "github:nix-community/impermanence";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    { self
    , nixpkgs
    , disko
    , ...
    } @ inputs:
    let
      lib = nixpkgs.lib;
      protocol = import ./lib/protocol.nix;
      mkBoardFn = import ./lib/mkBoard.nix nixpkgs;
      hostShellPrelude = import ./lib/host-prelude.nix protocol;

      # Host-build platforms.
      #
      # - x86_64-linux: full output set, including the riscv64-cross
      #   board matrix (boards.licheerv.* and boards.pcie.* via
      #   pkgsCross.riscv64). This is the developer-workstation path.
      # - aarch64-linux: only the userspace nanokvm-* packages. The
      #   board matrix is gated off on aarch64 because:
      #     1. platform/cv181x.nix pins `nixpkgs.buildPlatform =
      #        "x86_64-linux"` (cross-from-aarch64 isn't supported),
      #     2. nobody builds the cv181x SD image from an aarch64 host.
      #   This lets a Rock-5B (aarch64-linux NixOS) consume just
      #   `packages.aarch64-linux.nanokvm-server` and friends to run
      #   the web UI natively.
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = f:
        lib.genAttrs systems (system:
          f (import nixpkgs {
            inherit system;
            config.allowUnfreePredicate = allowUnfreePredicate;
            overlays = [ self.overlays.default ];
          }));

      patchDir = ./patches/nanokvm;
      patchNames =
        lib.sort builtins.lessThan
          (builtins.filter
            (name: lib.hasSuffix ".patch" name || lib.hasSuffix ".diff" name)
            (builtins.attrNames (builtins.readDir patchDir)));
      nanokvmPatches = map (name: patchDir + "/${name}") patchNames;

      rootAuthorizedKeys =
        lib.optionals (builtins.pathExists ./authorized_keys)
          (lib.filter (key: key != "") (lib.splitString "\n" (builtins.readFile ./authorized_keys)));

      # Local wpa_supplicant.conf for WiFi-booted live variants. Git flakes
      # exclude ignored files, so standalone secret injection is explicit:
      #   NANOKVM_WIFI_CONFIG=$PWD/wifi.conf nix build --impure ...
      # A path-flake invocation can still use the historical local file.
      localWifiConfig = builtins.getEnv "NANOKVM_WIFI_CONFIG";
      rootWpaConf =
        if localWifiConfig != ""
        then builtins.readFile localWifiConfig
        else if builtins.pathExists ./wifi.conf
        then builtins.readFile ./wifi.conf
        else null;

      allowUnfreePredicate = pkg:
        builtins.elem (lib.getName pkg) [
          "nanokvm-factory-runtime"
          "sg2002-coda980-firmware"
          "sg2002-c906l-firmware"
          "sophgo-host-tools"
        ];

      # Extra args threaded into every NixOS module via `_module.args`
      # (a module inside the list, not specialArgs — see lib/mkBoard.nix).
      # Lets module files reference flake-level facts (the wifi conf,
      # the overlay) without importing flake.nix.
      boardExtraArgs = {
        inherit rootAuthorizedKeys rootWpaConf allowUnfreePredicate;
        selfOverlay = self.overlays.default;
      };

      # Resolve a catalog-style {board, kernel, profile, …} record to
      # the arg set lib/mkBoard.nix expects. Shared by the two leaf
      # builders below so the nixosConfigurations and nixosModules
      # views of a board can never drift apart.
      resolveBoardArgs =
        { board
        , kernel
        , profile
        , mixins ? [ ]
        , extraModules ? [ ]
        ,
        }: {
          board = ./boards + "/${board}.nix";
          kernel = ./profiles/kernel + "/${kernel}.nix";
          profile = ./profiles + "/${profile}.nix";
          mixins = mixins ++ [
            self.nixosModules.default
            inputs.impermanence.nixosModules.impermanence
          ];
          extraModules = [ disko.nixosModules.disko ] ++ extraModules;
          extraArgs = boardExtraArgs;
        };

      # Compose a NixOS system from board / kernel / profile [+ mixins].
      mkBoard = args: mkBoardFn.mkBoard (resolveBoardArgs args);

      # The same composition as a plain importable module, for
      # downstream flakes that build their own nixosSystem around a
      # board (fleet base modules, Colmena deployment options, …).
      mkBoardModule = args: {
        imports = mkBoardFn.mkBoardModules (resolveBoardArgs args);
      };

      # Catalog of every {board, kernel, profile, variant} we publish.
      # One record per shipped configuration; both nixosConfigurations
      # and legacyPackages.boards.* are derived from this single source.
      catalog = import ./lib/catalog.nix { inherit lib; };

      # Walk the catalog and produce a nested attrset keyed by
      # entry.path, with each leaf built by `mkLeaf` from the entry's
      # mkBoard-style args. Instantiated twice: once with `mkBoard` (the source
      # of the flat nixosConfigurations output) and once with `mkBoardModule`
      # (nixosModules.boards).
      walkCatalog = mkLeaf: entries:
        lib.foldl'
          (acc: entry:
            lib.recursiveUpdate acc (lib.setAttrByPath entry.path (mkLeaf {
              board = entry.boardName;
              inherit (entry) kernel profile;
              mixins = entry.mixins or [ ];
              extraModules =
                entry.modules or [ ];
            })))
          { }
          entries;

      # =============================================================
      # The catalog of NixOS systems we publish, organised as
      #   boards.<board>.<kernel>.<profile>[.<variant>]
      #
      # Variants are encoded by which mixin modules get layered on top.
      # `<variant>` names use dashes to combine mixin tags
      # (e.g. `usb-oled` = USB transport + OLED panel mixin).
      # =============================================================
      # NixOS systems for every catalog entry, attrpath = entry.path.
      boardSystems = walkCatalog mkBoard catalog;
      boardModules = walkCatalog mkBoardModule catalog;

      k3BoardModules = {
        k3."pico-itx" = {
          uefi = {
            imports = [
              self.nixosModules.overlay
              ./boards/spacemit-k3-pico-itx.nix
              ./modules/spacemit-k3-uefi-boot.nix
              ./modules/spacemit-k3-usb-gadget.nix
              ({ ... }: { spacemit.k3.usbGadget.enable = true; })
            ];
          };
          "recovery-sd" = {
            imports = [
              self.nixosModules.overlay
              ./modules/spacemit-k3-recovery-sd-image.nix
            ];
          };
          "kexec-installer" = {
            imports = [
              self.nixosModules.overlay
              ./modules/spacemit-k3-kexec-installer.nix
            ];
          };
          "initrd-rescue" = {
            imports = [
              self.nixosModules.overlay
              ./modules/spacemit-k3-initrd-rescue.nix
            ];
          };
        };
      };

      k3BoardSystems =
        let
          mkK3System = module:
            nixpkgs.lib.nixosSystem {
              modules = [
                module
                ({ ... }: { spacemit.k3.authorizedKeys = rootAuthorizedKeys; })
              ];
            };
        in
        {
          k3."pico-itx"."recovery-sd" =
            mkK3System k3BoardModules.k3."pico-itx"."recovery-sd";
          k3."pico-itx"."kexec-installer" =
            mkK3System k3BoardModules.k3."pico-itx"."kexec-installer";
          k3."pico-itx"."initrd-rescue" =
            mkK3System k3BoardModules.k3."pico-itx"."initrd-rescue";
        };

      # `nixosConfigurations` is a standard flake schema: every direct child
      # must be a standalone NixOS system. Publish the self-contained mainline
      # systems under stable dash-joined names. Vendor systems and the K3
      # initrd rescue require site inputs; they remain available as modules and
      # legacyPackages artifacts without pretending to be standalone configs.
      flatBoardSystems =
        builtins.listToAttrs
          (map
            (entry: {
              name = lib.concatStringsSep "-" entry.path;
              value = lib.getAttrFromPath entry.path boardSystems;
            })
            (builtins.filter (entry: entry.kernel == "mainline") catalog))
        // {
          k3-pico-itx-recovery-sd = k3BoardSystems.k3."pico-itx"."recovery-sd";
          k3-pico-itx-kexec-installer = k3BoardSystems.k3."pico-itx"."kexec-installer";
        };

      # =============================================================
      # Helpers that build the host-side artifacts (FIT, kexec payload,
      # rootfs, and the runner shell scripts). Body lives in
      # lib/artifacts.nix so this file doesn't carry ~450 lines of bash.
      # =============================================================
      mkArtifacts = import ./lib/artifacts.nix {
        inherit lib hostShellPrelude;
      };
    in
    {
      overlays.default = import ./pkgs {
        inherit inputs nanokvmPatches;
      };

      nixosModules.overlay = {
        nixpkgs.overlays = [ self.overlays.default ];
      };
      nixosModules.extlinuxTryBoot = import ./modules/extlinux-try-boot.nix;
      nixosModules.nanokvm = import ./modules/nanokvm.nix;
      nixosModules.sg2002C906L = import ./modules/sg2002-c906l.nix;
      nixosModules.default = {
        imports = [
          self.nixosModules.nanokvm
          self.nixosModules.overlay
        ];
      };
      nixosModules.spacemitK3 = import ./platform/spacemit-k3.nix;
      nixosModules.spacemitK3UefiBoot = import ./modules/spacemit-k3-uefi-boot.nix;
      nixosModules.spacemitK3UsbGadget = import ./modules/spacemit-k3-usb-gadget.nix;
      nixosModules.spacemitK3UfsDisko = import ./modules/spacemit-k3-ufs-disko.nix;
      nixosModules.spacemitK3RecoverySdImage = import ./modules/spacemit-k3-recovery-sd-image.nix;
      nixosModules.spacemitK3KexecInstaller = import ./modules/spacemit-k3-kexec-installer.nix;
      nixosModules.spacemitK3InitrdRescue = import ./modules/spacemit-k3-initrd-rescue.nix;
      # Every catalog entry as a plain module. Downstream fleets import e.g.
      # `nixosModules.boards.pcie.mainline.sd` into their own
      # lib.nixosSystem to make the board a regular fleet member; the
      # module list is self-contained (no specialArgs required), so
      # Colmena-style re-instantiation from `_module.args.modules`
      # works without reconstructing anything.
      nixosModules.boards = lib.recursiveUpdate boardModules k3BoardModules;

      nixosConfigurations = flatBoardSystems;

      legacyPackages = forAllSystems (pkgs:
        let
          hostSys = pkgs.stdenv.hostPlatform.system;
          # The board matrix evaluates the riscv64 cross set + cv181x
          # platform module, both of which pin nixpkgs.buildPlatform =
          # "x86_64-linux". Don't try to construct it on aarch64.
          withBoardMatrix = hostSys == "x86_64-linux";

          art = mkArtifacts pkgs;

          # Specialise the artifact builders so the catalog-walker below
          # can just call them with a catalog entry. The DTB each artifact
          # boots comes from the entry's resolved `config.sg2002.fdt`, so
          # nothing board-specific needs threading through here anymore.
          entryCfg = entry: lib.getAttrFromPath entry.path boardSystems;
          entryArtifactArgs = entry: entry.artifactArgs or { };
          entryArtifactArg = name: default: entry: (entryArtifactArgs entry).${name} or default;
          entryOled = entryArtifactArg "oled" false;
          entryExtraBootargs = entryArtifactArg "extraBootargs" [ ];
          entryRootfsBindIp = entryArtifactArg "rootfsBindIp" null;
          entryRequireRootfsHostOverride = entryArtifactArg "requireRootfsHostOverride" false;
          entryIncludeKexec = entryArtifactArg "includeKexec" true;
          entryUsbConsole = entryArtifactArg "usbConsole" true;
          entryUartConsole = entryArtifactArg "uartConsole" "ttyS0";
          entryUsbBootTool = entry: cfg:
            if cfg.config.sg2002.auxCore.enable
            then pkgs.sg2002-usb-boot-for cfg.config.system.build.fipFastboot
            else if entry.boardName == "licheerv-nano-picoclaw"
            then pkgs.sg2002-usb-boot-picoclaw-splash
            else pkgs.sg2002-usb-boot;

          mkEntryPayload =
            { entry
            , cfg
            , extraBootargs ? [ ]
            ,
            }:
            art.mkKexecPayload {
              name = "nanokvm-kexec-${entry.tag}.erofs";
              inherit cfg extraBootargs;
              oled = entryOled entry;
              usbConsole = entryUsbConsole entry;
              uartConsole = entryUartConsole entry;
            };

          mkEntryBootFit =
            { entry
            , cfg
            , profile
            , description
            ,
            }:
            art.mkBootFit {
              inherit cfg profile description;
            };

          liveArtifacts = entry:
            let
              cfg = entryCfg entry;
              tag = entry.tag;
              oled = entryOled entry;
              rootfsBindIp = entryRootfsBindIp entry;
              requireRootfsHostOverride = entryRequireRootfsHostOverride entry;
              extraBootargs = entryExtraBootargs entry;
              includeKexec = entryIncludeKexec entry;
              usbConsole = entryUsbConsole entry;
              rootfs = art.mkLiveRootfs cfg;
              payload = mkEntryPayload {
                inherit entry cfg;
                extraBootargs =
                  [
                    "init=${cfg.config.system.build.toplevel}/init"
                    "nanokvm.kexec_target=${tag}"
                  ]
                  ++ extraBootargs;
              };
              kexec = art.mkKexecRunner {
                name = "kexec";
                inherit payload rootfs oled rootfsBindIp requireRootfsHostOverride;
                useRunningDtb = !oled && rootfsBindIp == null;
              };
              usb-boot = art.mkUsbBootRunner ({
                name = "usb-boot";
                usbBootTool = entryUsbBootTool entry cfg;
                inherit rootfsBindIp requireRootfsHostOverride;
                fit = mkEntryBootFit {
                  inherit entry cfg;
                  profile = "live";
                  description = "NanoKVM SG2002 USB NBD live boot (${tag})";
                };
                inherit rootfs;
                bootargs = art.mkLiveBootargs {
                  inherit cfg oled usbConsole;
                  extra = extraBootargs;
                  uartConsole = entryUartConsole entry;
                };
                waitForSsh = true;
              } // lib.optionalAttrs includeKexec {
                onShellDetachCommand = "${kexec}/bin/kexec";
              });
            in
            {
              inherit rootfs usb-boot;
            }
            // lib.optionalAttrs includeKexec {
              inherit payload kexec;
            };

          kernelTestArtifacts = entry:
            let
              cfg = entryCfg entry;
              oled = entryOled entry;
              payload = mkEntryPayload {
                inherit entry cfg;
              };
              kexec = art.mkKexecRunner {
                name = "kexec";
                inherit payload oled;
                useRunningDtb = !oled;
              };
              usb-boot = art.mkUsbBootRunner {
                name = "usb-boot";
                usbBootTool = entryUsbBootTool entry cfg;
                fit = mkEntryBootFit {
                  inherit entry cfg;
                  profile = "kernel-test";
                  description = "NanoKVM SG2002 USB kernel test (${entry.tag})";
                };
                bootargs = art.mkKexecBootargs {
                  extra = entryExtraBootargs entry;
                  usbConsole = entryUsbConsole entry;
                  uartConsole = entryUartConsole entry;
                };
                attachPicocom = true;
              };
            in
            {
              inherit payload kexec usb-boot;
            };

          debugArtifacts = entry:
            let
              cfg = entryCfg entry;
              liveCfg = lib.getAttrFromPath entry.liveCfgPath boardSystems;
              tag = entry.tag;
              rootfs = art.mkLiveRootfs liveCfg;
              payload = mkEntryPayload {
                inherit entry cfg;
                extraBootargs = [ "nanokvm.kexec_target=${tag}" ];
              };
              kexec = art.mkKexecRunner {
                name = "kexec";
                inherit payload rootfs;
                useRunningDtb = true;
              };
              usb-boot = art.mkUsbBootRunner {
                name = "usb-boot";
                usbBootTool = entryUsbBootTool entry cfg;
                fit = mkEntryBootFit {
                  inherit entry cfg;
                  profile = "debug";
                  description = "NanoKVM SG2002 USB NBD debug boot (${tag})";
                };
                inherit rootfs;
                bootargs = art.kernelTestBootargs;
              };
            in
            {
              inherit rootfs payload kexec usb-boot;
            };

          sdImageArtifact = entry:
            (lib.getAttrFromPath entry.path boardSystems).config.system.build.sdImage;

          # NFS-rooted live: no rootfs image at all. The host's kernel
          # nfsd exports /nix/store read-only; the target mounts it
          # from the initrd (config baked into the system, no
          # runtime bootargs needed). The kexec payload still travels
          # over NBD — it's tiny and the agent already speaks it.
          nfsLiveArtifacts = entry:
            let
              cfg = entryCfg entry;
              tag = entry.tag;
              payload = mkEntryPayload {
                inherit entry cfg;
                extraBootargs = [
                  "init=${cfg.config.system.build.toplevel}/init"
                  "nanokvm.kexec_target=${tag}"
                ];
              };
              kexec = art.mkNfsKexecRunner {
                name = "kexec";
                inherit payload;
                nfsServer = cfg.config.nanokvm.nfsLive.server;
                nfsExport = cfg.config.nanokvm.nfsLive.storeExport;
                # The payload carries the board's resolved fdt
                # (wifi-variant DTB via the aic8800 mixin); don't keep
                # whatever DTB the source kernel happened to boot with
                # (e.g. the nowifi kernel-test one).
                useRunningDtb = false;
              };
              usb-boot = art.mkNfsUsbBootRunner {
                name = "usb-boot";
                usbBootTool = entryUsbBootTool entry cfg;
                fit = mkEntryBootFit {
                  inherit entry cfg;
                  profile = "live";
                  description = "SG2002 USB NFS live boot (${tag})";
                };
                bootargs = art.mkLiveBootargs {
                  inherit cfg;
                  extra = entryExtraBootargs entry;
                  uartConsole = entryUartConsole entry;
                  usbConsole = entryUsbConsole entry;
                };
                nfsServer = cfg.config.nanokvm.nfsLive.server;
                nfsExport = cfg.config.nanokvm.nfsLive.storeExport;
                waitForSsh = true;
                onShellDetachCommand = "${kexec}/bin/kexec";
              };
            in
            {
              inherit payload kexec usb-boot;
            };

          # Dispatch table indexed by entry.artifact.
          artifactBuilder = {
            "kernel-test" = kernelTestArtifacts;
            "live" = liveArtifacts;
            "debug" = debugArtifacts;
            "nfs-live" = nfsLiveArtifacts;
            "sd" = sdImageArtifact;
          };

          # Walk the catalog and produce the nested legacyPackages.boards tree.
          boardsTree =
            lib.foldl'
              (acc: entry:
                if entry.artifact == null
                then acc
                else
                  lib.recursiveUpdate acc (lib.setAttrByPath entry.path
                    (artifactBuilder.${entry.artifact} entry)))
              { }
              catalog;

          k3PackagesTree = {
            k3."pico-itx"."recovery-sd" =
              k3BoardSystems.k3."pico-itx"."recovery-sd".config.system.build.sdImage;
            k3."pico-itx"."kexec-installer" =
              k3BoardSystems.k3."pico-itx"."kexec-installer".config.system.build.kexecInstallerTarball;
          };
        in
        (lib.optionalAttrs withBoardMatrix {
          boards = lib.recursiveUpdate boardsTree k3PackagesTree;
        })
        // {
          # Convenience: surface the underlying packages so callers can
          # `nix build .#nanokvm-server` etc without reaching into the
          # boards/ tree.
          inherit
            (pkgs)
            nanokvm-bench-usb-transport
            nanokvm-patched-src
            nanokvm-factory-runtime
            nanokvm-host-keys
            nanokvm-server
            nanokvm-server-nocamera
            nanokvm-web
            nbd-client-minimal
            sg2002-c906l-contract
            sg2002-c906l-contract-timer4
            sg2002-c906l-contract-timer5
            sg2002-c906l-contract-timer6
            sg2002-c906l-contract-timer7
            sg2002-dtb-mainline-nowifi-c906l
            sg2002-dtb-mainline-nowifi-c906l-timer4
            sg2002-dtb-mainline-nowifi-c906l-timer5
            sg2002-dtb-mainline-nowifi-c906l-timer6
            sg2002-dtb-mainline-nowifi-c906l-timer7
            sg2002-dtb-mainline-pcie-nowifi-c906l
            sg2002-dtb-mainline-pcie-nowifi-c906l-timer4
            sg2002-dtb-mainline-pcie-nowifi-c906l-timer5
            sg2002-dtb-mainline-pcie-nowifi-c906l-timer6
            sg2002-dtb-mainline-pcie-nowifi-c906l-timer7
            sg2002-fiptool
            sg2002-fip-mainline-fastboot
            sg2002-fip-mainline-fastboot-c906l
            sg2002-fip-mainline-uboot-c906l
            sg2002-fip-mainline-fastboot-c906l-timer4
            sg2002-fip-mainline-uboot-c906l-timer4
            sg2002-fip-mainline-fastboot-c906l-timer5
            sg2002-fip-mainline-uboot-c906l-timer5
            sg2002-fip-mainline-fastboot-c906l-timer6
            sg2002-fip-mainline-uboot-c906l-timer6
            sg2002-fip-mainline-fastboot-c906l-timer7
            sg2002-fip-mainline-uboot-c906l-timer7
            sg2002-fip-mainline-picoclaw-splash
            sg2002-c906l-firmware
            sg2002-c906l-firmware-timer4
            sg2002-c906l-firmware-timer5
            sg2002-c906l-firmware-timer6
            sg2002-c906l-firmware-timer7
            sg2002-c906l-control
            sg2002-c906l-control-timer4
            sg2002-c906l-control-timer5
            sg2002-c906l-control-timer6
            sg2002-c906l-control-timer7
            sg2002-c906l-remoteproc
            sg2002-c906l-remoteproc-timer4
            sg2002-c906l-remoteproc-timer5
            sg2002-c906l-remoteproc-timer6
            sg2002-c906l-remoteproc-timer7
            sg2002-c906l-ctl
            sg2002-c906l-ctl-timer4
            sg2002-c906l-ctl-timer5
            sg2002-c906l-ctl-timer6
            sg2002-c906l-ctl-timer7
            sg2002-c906l-rust
            sg2002-c906l-rust-timer4
            sg2002-c906l-rust-timer5
            sg2002-c906l-rust-timer6
            sg2002-c906l-rust-timer7
            sg2002-alsa-kernel-test
            sg2002-h264-bridge
            sg2002-h264-bridge-pcma
            sg2002-kernel-mainline
            sg2002-usb-boot
            sg2002-usb-boot-c906l
            sg2002-usb-boot-c906l-timer4
            sg2002-usb-boot-c906l-timer5
            sg2002-usb-boot-c906l-timer6
            sg2002-usb-boot-c906l-timer7
            sg2002-usb-boot-picoclaw-splash
            sg2002-uboot-mainline-c906l
            sg2002-uboot-mainline-fastboot
            sg2002-uboot-mainline-fastboot-c906l
            sg2002-uboot-mainline-picoclaw-splash
            spacemit-k3-fsbl
            spacemit-k3-linux
            spacemit-k3-raw-fastboot-boot
            spacemit-k3-uefi-blobs
            sophgo-host-tools
            ;
          default = pkgs.nanokvm-server;
        }
        // lib.optionalAttrs withBoardMatrix {
          # This helper executes on the K3 target; expose an actual riscv64
          # derivation instead of lying about the x86 host platform.
          spacemit-k3-flash-uefi = pkgs.pkgsCross.riscv64.spacemit-k3-flash-uefi;
          sg2002-licheerv-nano-oled-dtbo = art.sg2002OledOverlayDtbo;
        });

      # `packages` must contain flat derivations. Nix installable lookup falls
      # back to legacyPackages, preserving `.#boards.picoclaw...` commands.
      packages = lib.mapAttrs
        (_system: attrs: builtins.removeAttrs attrs [ "boards" ])
        self.legacyPackages;

      checks = forAllSystems (pkgs:
        lib.optionalAttrs (pkgs.stdenv.hostPlatform.system == "x86_64-linux") {
          sg2002-c906l-contract = pkgs.sg2002-c906l-contract;
          sg2002-c906l-contract-timer4 = pkgs.sg2002-c906l-contract-timer4;
          sg2002-c906l-contract-timer5 = pkgs.sg2002-c906l-contract-timer5;
          sg2002-c906l-contract-timer6 = pkgs.sg2002-c906l-contract-timer6;
          sg2002-c906l-contract-timer7 = pkgs.sg2002-c906l-contract-timer7;
          sg2002-c906l-contract-generator =
            pkgs.sg2002-c906l-contract.tests.generator;
          sg2002-c906l-rust = pkgs.sg2002-c906l-rust-tests;
          sg2002-c906l-rust-timer4 = pkgs.sg2002-c906l-rust-tests-timer4;
          sg2002-c906l-rust-timer5 = pkgs.sg2002-c906l-rust-tests-timer5;
          sg2002-c906l-rust-timer6 = pkgs.sg2002-c906l-rust-tests-timer6;
          sg2002-c906l-rust-timer7 = pkgs.sg2002-c906l-rust-tests-timer7;
          sg2002-c906l-firmware = pkgs.sg2002-c906l-firmware;
          sg2002-c906l-firmware-timer4 = pkgs.sg2002-c906l-firmware-timer4;
          sg2002-c906l-firmware-timer5 = pkgs.sg2002-c906l-firmware-timer5;
          sg2002-c906l-firmware-timer6 = pkgs.sg2002-c906l-firmware-timer6;
          sg2002-c906l-firmware-timer7 = pkgs.sg2002-c906l-firmware-timer7;
          sg2002-c906l-control = pkgs.sg2002-c906l-control;
          sg2002-c906l-control-timer4 = pkgs.sg2002-c906l-control-timer4;
          sg2002-c906l-control-timer5 = pkgs.sg2002-c906l-control-timer5;
          sg2002-c906l-control-timer6 = pkgs.sg2002-c906l-control-timer6;
          sg2002-c906l-control-timer7 = pkgs.sg2002-c906l-control-timer7;
          sg2002-c906l-remoteproc = pkgs.sg2002-c906l-remoteproc;
          sg2002-c906l-remoteproc-timer4 = pkgs.sg2002-c906l-remoteproc-timer4;
          sg2002-c906l-remoteproc-timer5 = pkgs.sg2002-c906l-remoteproc-timer5;
          sg2002-c906l-remoteproc-timer6 = pkgs.sg2002-c906l-remoteproc-timer6;
          sg2002-c906l-remoteproc-timer7 = pkgs.sg2002-c906l-remoteproc-timer7;
          sg2002-c906l-ctl = pkgs.sg2002-c906l-ctl;
          sg2002-c906l-ctl-timer4 = pkgs.sg2002-c906l-ctl-timer4;
          sg2002-c906l-ctl-timer5 = pkgs.sg2002-c906l-ctl-timer5;
          sg2002-c906l-ctl-timer6 = pkgs.sg2002-c906l-ctl-timer6;
          sg2002-c906l-ctl-timer7 = pkgs.sg2002-c906l-ctl-timer7;
          sg2002-c906l-fip-disabled = pkgs.sg2002-fip-mainline-fastboot;
          sg2002-c906l-fip = pkgs.sg2002-fip-mainline-fastboot-c906l;
          sg2002-c906l-fip-timer4 = pkgs.sg2002-fip-mainline-fastboot-c906l-timer4;
          sg2002-c906l-fip-timer5 = pkgs.sg2002-fip-mainline-fastboot-c906l-timer5;
          sg2002-c906l-fip-timer6 = pkgs.sg2002-fip-mainline-fastboot-c906l-timer6;
          sg2002-c906l-fip-timer7 = pkgs.sg2002-fip-mainline-fastboot-c906l-timer7;
          sg2002-c906l-uboot = pkgs.sg2002-uboot-mainline-fastboot-c906l;
          sg2002-c906l-runner = pkgs.sg2002-usb-boot-c906l.tests.runner;
          sg2002-c906l-runner-timer4 =
            pkgs.sg2002-usb-boot-c906l-timer4.tests.runner;
          sg2002-c906l-runner-timer5 =
            pkgs.sg2002-usb-boot-c906l-timer5.tests.runner;
          sg2002-c906l-runner-timer6 =
            pkgs.sg2002-usb-boot-c906l-timer6.tests.runner;
          sg2002-c906l-runner-timer7 =
            pkgs.sg2002-usb-boot-c906l-timer7.tests.runner;
          sg2002-c906l-dtb = pkgs.sg2002-dtb-mainline-nowifi-c906l;
          sg2002-c906l-dtb-lease-guard =
            pkgs.sg2002-dtb-mainline-nowifi-c906l.tests.leaseGuard;
          sg2002-c906l-dtb-timer4 =
            pkgs.sg2002-dtb-mainline-nowifi-c906l-timer4;
          sg2002-c906l-dtb-timer5 =
            pkgs.sg2002-dtb-mainline-nowifi-c906l-timer5;
          sg2002-c906l-dtb-timer6 =
            pkgs.sg2002-dtb-mainline-nowifi-c906l-timer6;
          sg2002-c906l-dtb-timer7 =
            pkgs.sg2002-dtb-mainline-nowifi-c906l-timer7;
          sg2002-c906l-pcie-dtb = pkgs.sg2002-dtb-mainline-pcie-nowifi-c906l;
          sg2002-c906l-pcie-dtb-timer4 =
            pkgs.sg2002-dtb-mainline-pcie-nowifi-c906l-timer4;
          sg2002-c906l-pcie-dtb-timer5 =
            pkgs.sg2002-dtb-mainline-pcie-nowifi-c906l-timer5;
          sg2002-c906l-pcie-dtb-timer6 =
            pkgs.sg2002-dtb-mainline-pcie-nowifi-c906l-timer6;
          sg2002-c906l-pcie-dtb-timer7 =
            pkgs.sg2002-dtb-mainline-pcie-nowifi-c906l-timer7;
        });

      # `apps.<system>` is reserved for flat `nix run` shortcuts. The
      # boards.* tree lives under `legacyPackages.<system>.boards.…` instead;
      # the runner derivations there have `bin/kexec` and `bin/usb-boot`
      # so `nix run .#boards.licheerv.mainline.live.usb.kexec` finds the
      # right binary directly.
      apps = forAllSystems (pkgs:
        let
          system = pkgs.stdenv.hostPlatform.system;
          usbOledTop = pkgs.writeShellApplication {
            name = "usb-oled-top";
            text = ''
                            case "''${1:-}" in
                              -h|--help)
                                cat <<'EOF'
              Usage: nix run .#usb-oled-top -- [usb-boot options]

              Boots or kexecs the LicheeRV-Nano-W OLED live image and runs top on
              the 128x128 framebuffer via fbcon.

              Defaults:
                --attempts 120
                --rom-dl-timeout 1800
                --wait 120

              Environment overrides:
                NANOKVM_USB_BOOT_ATTEMPTS
                NANOKVM_USB_BOOT_ROM_DL_TIMEOUT
                NANOKVM_USB_BOOT_WAIT
                NANOKVM_NBD_ROOTFS_PORT=auto|0|<port>
                NANOKVM_NBD_ROOTFS_HOST=<target-visible-host-ip>
                NANOKVM_NBD_ROOTFS_BIND=<host-bind-ip>
                NANOKVM_NBD_CLEANUP=0
                NANOKVM_ATTACH=shell|none
                NANOKVM_ON_DETACH=hold|kexec|exit
                NANOKVM_BOOT_MODE=auto|usb|kexec
                NANOKVM_STATUS_LISTEN=1
                USB_IFACE
              EOF
                                exit 0
                                ;;
                            esac

                            usb_iface_present() {
                              local path mac
                              if [ -n "''${USB_IFACE:-}" ]; then
                                [ -d "/sys/class/net/$USB_IFACE" ]
                                return
                              fi

                              for path in /sys/class/net/*; do
                                [ -r "$path/address" ] || continue
                                IFS= read -r mac < "$path/address" || true
                                if [ "$mac" = "${protocol.hostMac}" ]; then
                                  return 0
                                fi
                              done
                              return 1
                            }

                            case "''${NANOKVM_BOOT_MODE:-auto}" in
                              auto|"")
                                if usb_iface_present; then
                                  echo "[usb-oled-top] USB debug interface is present; using kexec"
                                  exec ${self.legacyPackages.${system}.boards.licheerv.mainline.live.usb-oled.kexec}/bin/kexec "$@"
                                fi
                                ;;
                              kexec)
                                exec ${self.legacyPackages.${system}.boards.licheerv.mainline.live.usb-oled.kexec}/bin/kexec "$@"
                                ;;
                              usb|usb-boot)
                                ;;
                              *)
                                echo "[usb-oled-top] invalid NANOKVM_BOOT_MODE=''${NANOKVM_BOOT_MODE}; expected auto, usb, or kexec" >&2
                                exit 1
                                ;;
                            esac

                            export NANOKVM_ON_DETACH="''${NANOKVM_ON_DETACH:-kexec}"
                            exec ${self.legacyPackages.${system}.boards.licheerv.mainline.live.usb-oled.usb-boot}/bin/usb-boot \
                              --attempts "''${NANOKVM_USB_BOOT_ATTEMPTS:-120}" \
                              --rom-dl-timeout "''${NANOKVM_USB_BOOT_ROM_DL_TIMEOUT:-1800}" \
                              --wait "''${NANOKVM_USB_BOOT_WAIT:-120}" \
                              "$@"
            '';
          };
          captureUsbOledTop = pkgs.writeShellApplication {
            name = "capture-usb-oled-top";
            runtimeInputs = with pkgs; [
              asciinema
              asciinema-agg
              coreutils
              git
              gnused
              openssh
              sshpass
            ];
            text = ''
              export NANOKVM_USB_OLED_TOP="''${NANOKVM_USB_OLED_TOP:-${usbOledTop}/bin/usb-oled-top}"
              export NANOKVM_CAPTURE_FONT_DIR="''${NANOKVM_CAPTURE_FONT_DIR:-${pkgs.dejavu_fonts}/share/fonts/truetype}"
              export NANOKVM_CAPTURE_FONT_FAMILY="''${NANOKVM_CAPTURE_FONT_FAMILY:-DejaVu Sans Mono}"
              ${builtins.readFile ./scripts/capture-usb-oled-top.sh}
            '';
          };
        in
        lib.optionalAttrs pkgs.stdenv.isLinux
          {
            usb-boot-mainline = {
              type = "app";
              program = "${pkgs.sg2002-usb-boot}/bin/usb-boot-mainline";
            };
          }
        // lib.optionalAttrs (system == "x86_64-linux") {
          usb-oled-top = {
            type = "app";
            program = "${usbOledTop}/bin/usb-oled-top";
          };
          capture-usb-oled-top = {
            type = "app";
            program = "${captureUsbOledTop}/bin/capture-usb-oled-top";
          };
        });

      devShells = forAllSystems (pkgs:
        {
          default = pkgs.mkShell {
            packages = with pkgs;
              [
                go_1_25
                nodejs_24
                pnpm_10
                patchelf
                dtc
                erofs-utils
                nbd
                sg2002-cv181x-usb-dl
                usbutils
                pkgsCross.riscv64-musl.stdenv.cc
              ]
              ++ lib.optionals pkgs.stdenv.isLinux [
                android-tools
                picocom
              ];

            shellHook = ''
              export GOOS=linux
              export GOARCH=riscv64
              export CGO_ENABLED=1
              export CC=${pkgs.pkgsCross.riscv64-musl.stdenv.cc}/bin/riscv64-unknown-linux-musl-gcc
              export CGO_CFLAGS="-mcpu=thead-c906 -march=rv64gc_xtheadba_xtheadbb_xtheadbs_xtheadcmo_xtheadcondmov_xtheadfmemidx_xtheadmac_xtheadmemidx_xtheadmempair_xtheadsync -mcmodel=medany -mabi=lp64d"
            '';
          };
        }
        // lib.optionalAttrs (pkgs.stdenv.hostPlatform.system == "x86_64-linux") {
          c906l = pkgs.mkShell {
            inputsFrom = [ pkgs.sg2002-c906l-rust ];
            packages = [
              pkgs.cargo
              pkgs.clippy
              pkgs.rust-analyzer
              pkgs.rustc
              pkgs.rustfmt
              pkgs.pkgsCross.riscv64-embedded.stdenv.cc
            ];
            shellHook = ''
              export CARGO_BUILD_TARGET=riscv64gc-unknown-none-elf
              export RUSTFLAGS="-C code-model=medium"
            '';
          };
        });
    };
}
