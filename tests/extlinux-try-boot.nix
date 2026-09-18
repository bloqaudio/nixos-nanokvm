{ pkgs }:
let
  configuration = pkgs.writeText "extlinux-test.conf" ''
    DEFAULT old
    LABEL old
      APPEND init=/nix/store/old-system/init
    LABEL candidate
      APPEND init=/nix/store/candidate-system/init
  '';
in
pkgs.testers.runNixOSTest {
  name = "extlinux-try-boot";
  nodes.machine = { ... }: {
    imports = [ ../modules/extlinux-try-boot.nix ];
    boot.extlinuxTryBoot = {
      enable = true;
      autoArmOnSwitch = false;
      configPath = "/var/lib/tryboot/extlinux.conf";
      statePath = "/var/lib/tryboot/state";
      timeoutSec = 2;
      successCommand = ''
        touch /run/health-checked
        test ! -e /run/fail-health
      '';
    };
    systemd.services.extlinux-try-boot-bless.environment.EXTLINUX_TRY_BOOT_CMDLINE =
      "/run/test-cmdline";
  };
  testScript = ''
    import time

    start_all()
    machine.wait_for_unit("multi-user.target")
    unit = "extlinux-try-boot-bless.service"
    cli = "env EXTLINUX_TRY_BOOT_CMDLINE=/run/test-cmdline extlinux-try-boot"

    def booted(label):
        machine.succeed(f"printf 'init=/nix/store/{label}-system/init\\n' > /run/test-cmdline")

    def restart_bless():
        # These independent boot scenarios reuse one VM rather than waiting
        # for systemd's normal start-rate window between simulated boots.
        machine.succeed(f"systemctl reset-failed {unit}")
        machine.succeed(f"systemctl restart {unit}")

    with subtest("ordinary boot skips the timer and health command"):
        machine.succeed("install -Dm644 ${configuration} /var/lib/tryboot/extlinux.conf")
        booted("old")
        machine.fail(f"{cli} is-candidate")
        restart_bless()
        machine.succeed("test ! -e /run/health-checked")
        assert machine.succeed(f"systemctl show {unit} -p ExecMainStartTimestampMonotonic --value").strip() == "0"

    with subtest("an armed candidate does not delay the running old generation"):
        machine.succeed(f"{cli} arm candidate old")
        restart_bless()
        machine.succeed("test ! -e /run/health-checked")
        machine.succeed("grep -Fx 'DEFAULT candidate' /var/lib/tryboot/extlinux.conf")

    with subtest("candidate failure retains the fallback and trial state"):
        booted("candidate")
        machine.succeed(f"{cli} early-rollback")
        machine.succeed("touch /run/fail-health")
        machine.fail(f"systemctl restart {unit}")
        machine.succeed("test -e /run/health-checked; test -s /var/lib/tryboot/state")
        machine.succeed("grep -Fx 'DEFAULT old' /var/lib/tryboot/extlinux.conf")

    with subtest("candidate must survive the full health window before blessing"):
        machine.succeed("rm /run/fail-health /run/health-checked")
        started = time.monotonic()
        restart_bless()
        assert time.monotonic() - started >= 2
        machine.succeed("test -e /run/health-checked; test ! -e /var/lib/tryboot/state")
        machine.succeed("grep -Fx 'DEFAULT candidate' /var/lib/tryboot/extlinux.conf")

    with subtest("fallback boot skips a stale trial without modifying it"):
        machine.succeed(f"{cli} set-default old; {cli} arm candidate old")
        booted("old")
        machine.succeed("rm /run/health-checked")
        restart_bless()
        machine.succeed("test ! -e /run/health-checked; test -s /var/lib/tryboot/state")

    with subtest("a reused label with a different init is not the candidate"):
        machine.succeed("sed -i s,candidate-system/init,replaced-system/init, /var/lib/tryboot/extlinux.conf")
        booted("replaced")
        restart_bless()
        machine.succeed("test ! -e /run/health-checked; test -s /var/lib/tryboot/state")

    with subtest("malformed state fails closed without running health commands"):
        machine.succeed("printf 'candidate=broken\\n' > /var/lib/tryboot/state")
        machine.succeed(f"status=0; {cli} is-candidate || status=$?; test $status = 255")
        machine.succeed(f"systemctl reset-failed {unit}")
        machine.fail(f"systemctl restart {unit}")
        machine.succeed("test ! -e /run/health-checked; test -s /var/lib/tryboot/state")
  '';
}
