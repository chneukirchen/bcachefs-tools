{ stdenv }:

stdenv.mkDerivation {
  name = "bcachefs-tools-git";

  builder = builtins.toFile "builder.sh" ''
    source $stdenv/setup
    make
    make install
  '';

  src = "./.";
}
