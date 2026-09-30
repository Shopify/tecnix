{
  nixFlake ? builtins.getFlake ("git+file://" + toString ../../..),
  system ? builtins.currentSystem,
  pkgs ? nixFlake.inputs.nixpkgs.legacyPackages.${system},
  stdenv ? "stdenv",
  componentTestsPrefix ? "",
  # Shorthand for enabling both sanitizers and coverage.
  withInstrumentation ? false,
  withSanitizers ? withInstrumentation,
  withCoverage ? withInstrumentation,
}@args:
import ./. (
  args
  // {
    getStdenv = p: p.${stdenv};
    inherit withSanitizers withCoverage;
  }
)
