# Linux systemd install and uninstall targets

Date: 2026-08-30

## Outcome

CMake now generates both `install` and `uninstall` build targets suitable for a
Linux system installation. With `-DCMAKE_INSTALL_PREFIX=/usr`, the managed
layout is:

- executable: `/usr/bin/macha`;
- systemd unit: `/usr/lib/systemd/system/macha.service`;
- configuration: `/etc/macha/macha.yaml`;
- configuration example: `/etc/macha/macha.yaml.example`; and
- documentation: `/usr/share/doc/macha`.

The systemd install is enabled by default only when targeting Linux. Both the
systemd unit directory and configuration directory remain CMake cache options
for distributions with different layouts.

The unit directory deliberately uses `lib/systemd/system` rather than
`CMAKE_INSTALL_LIBDIR`. Debian-family multiarch values such as
`lib/aarch64-linux-gnu` are valid library locations but are not systemd unit
search paths. Reconfiguring an affected build automatically migrates that exact
old cached default while preserving other operator-supplied overrides.

## Configuration safety

The installer copies the complete example to the operational configuration
path only when `macha.yaml` does not already exist. A reinstall or upgrade
preserves the existing file. The install output prominently prints both the
operational and example paths, followed by the commands needed to reload and
enable systemd.

The generated unit invokes the installed binary directly with the generated
configuration path. It no longer relies on the inconsistent legacy
`/etc/macha` EnvironmentFile and `/etc/macha.yaml` indirection. The unit will
not start when the YAML is missing and is not automatically enabled before the
operator reviews node-specific storage paths, mount path, advertised address,
and cluster key.

## Uninstall safety

`make uninstall` reads CMake's exact install manifest and removes managed
binaries, unit, documentation, and examples. It deliberately preserves the
operational YAML because that file is created outside the manifest. It also
preserves keys, state, cache, spool, mount, and media data and prints this fact.

Operators should stop and disable the live service before uninstalling, then
run `systemctl daemon-reload`. Staged/package installs using `DESTDIR` do not
contact the host systemd instance.

## Verification

A staged `/usr` install was performed beneath a fresh temporary `DESTDIR`.
Checks proved:

- the `install`, `install/strip`, and `uninstall` targets exist;
- the binary, unit, example, and initial YAML were created at the expected
  Linux paths;
- the unit contained exactly `ExecStart=/usr/bin/macha --config
  /etc/macha/macha.yaml` and the matching `ConditionPathExists`;
- the installer printed `/etc/macha/macha.yaml` prominently;
- uninstall removed every manifest-owned artifact checked;
- uninstall preserved `/etc/macha/macha.yaml`; and
- a second install reported that the existing configuration was preserved and
  left its checksum unchanged (`3573092133`, 13,341 bytes).

The staged check ran on the current macOS development host, so its Mach-O copy
step emitted a sandboxed `install_name_tool` cache warning. The CMake install
completed successfully; that platform-specific tool is not used by the Linux
installation path being added here.

An additional regression check simulated a cached ARM Debian installation with
`CMAKE_INSTALL_LIBDIR=lib/aarch64-linux-gnu`. Reconfiguration migrated the unit
destination to `/usr/lib/systemd/system`, the staged install placed the unit
there, and both generated install scripts ran without the former CMP0012
developer warning.
