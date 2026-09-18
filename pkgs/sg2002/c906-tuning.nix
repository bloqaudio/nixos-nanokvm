# Honour the target platform's C906 scheduling model throughout GCC-built
# target packages, including libc. The pinned nixpkgs cc-wrapper supports
# gcc.tune on several architectures but omits RISC-V from its dispatch.
# Use wrapCCWith's extension point, not per-package flags or a replacement
# stdenv. Native build tools, other targets and non-GNU compilers are unchanged.
final: prev:
{
  wrapCCWith = args:
    let
      platform = (args.stdenvNoCC or prev.stdenv).targetPlatform;
      enable = platform.isRiscV64
        && (platform.gcc.tune or null) == "thead-c906"
        && (args.isGNU or (args.cc.isGNU or false));
    in prev.wrapCCWith (args // prev.lib.optionalAttrs enable {
      extraBuildCommands = (args.extraBuildCommands or "") + ''
        # Defaults precede caller flags, so explicit package overrides still
        # work. Avoid duplication when nixpkgs gains native RISC-V support.
        if ! grep -Eq -- '(^|[[:space:]])-mtune=thead-c906([[:space:]]|$)' "$out/nix-support/cc-cflags-before"; then
          printf '%s\n' '-mtune=thead-c906' >> "$out/nix-support/cc-cflags-before"
        fi
      '';
    });
}
