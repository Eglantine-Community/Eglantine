# SPDX-FileCopyrightText: Copyright 2026 Eglantine Community
# SPDX-License-Identifier: GPL-3.0-or-later
#
# RenHDrum: the HD Rumble / DualSense haptics engine (https://github.com/trzz1214/RenHDrum).
#
# The emulator opens it at runtime and keeps its own rumble as the fallback when it is missing, so
# this is a runtime dependency and not a link dependency: it has to exist in the store and be
# reachable through the binaries' RPATH (see nix/package.nix), not appear on the linker command
# line. nixpkgs has no recipe for it, so it is built here from the source that the vendored
# externals/renhdrum/renhdrum.h belongs to.
{
  lib,
  rustPlatform,
  stdenv,
  fetchFromGitHub,
}:

rustPlatform.buildRustPackage rec {
  pname = "renhdrum";
  version = "0.4.0";

  src = fetchFromGitHub {
    owner = "trzz1214";
    repo = "RenHDrum";
    tag = "v${version}";
    hash = "sha256-+VytHpeG7VtwAEe2zuUERpG6vgtPCM1v9jYTVqPXq6c=";
  };

  # The crate has no dependencies at all, so the lock file pins nothing and nothing is vendored.
  cargoLock.lockFile = "${src}/Cargo.lock";

  # The crate builds a cdylib, a staticlib and an rlib; the emulator only uses the cdylib.
  installPhase = ''
    runHook preInstall

    library=${
      if stdenv.hostPlatform.isDarwin then "librenhdrum.dylib" else "librenhdrum.so"
    }
    # cargoBuildHook builds for an explicit --target, so artifacts land under
    # target/<triple>/release rather than target/release.
    built=$(find target -name "$library" -path '*/release/*' -type f | head -1)
    test -n "$built"
    install -Dm755 "$built" "$out/lib/$library"
    install -Dm644 include/renhdrum.h "$out/include/renhdrum.h"

    runHook postInstall
  '';

  # Its own suite is a research harness for the engine, not a check of this package.
  doCheck = false;

  meta = {
    description = "HD Rumble and DualSense haptics engine for Switch emulation";
    homepage = "https://github.com/trzz1214/RenHDrum";
    license = lib.licenses.asl20;
    platforms = lib.platforms.linux ++ lib.platforms.darwin;
  };
}
