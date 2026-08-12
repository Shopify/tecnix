# shellcheck shell=bash

# Common setup for tecnix functional tests

set -eu -o pipefail

if [[ -z "${TECNIX_COMMON_SH_SOURCED-}" ]]; then

TECNIX_COMMON_SH_SOURCED=1

# Source the main test framework
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

requireGit

# Create a minimal repo for testing the public tecnix builtins.
create_tecnix_builtin_test_world() {
    local dir="$1"

    git init "$dir"
    cd "$dir"

    mkdir -p .meta
    mkdir -p system/tectonix
    mkdir -p areas/app/web/targets
    mkdir -p areas/lib/shared/targets

    cat > .meta/manifest.json << 'MANIFEST_EOF'
{
    "//areas/app/web": { "id": "W-100001" },
    "//areas/lib/shared": { "id": "W-100002" }
}
MANIFEST_EOF

    cat > system/tectonix/resolve.nix << 'RESOLVE_EOF'
{ system ? builtins.currentSystem }:
let
  parseTarget = target:
    let match = builtins.match "(//.*):(.*)" target;
    in if match == null then throw "invalid target: ${target}" else {
      zone = builtins.elemAt match 0;
      name = builtins.elemAt match 1;
    };

  repoRoot = ../..;
  zoneSource = zonePath:
    let
      relPath =
        if builtins.match "//.*" zonePath != null
        then builtins.substring 2 (builtins.stringLength zonePath) zonePath
        else throw "invalid zone path: ${zonePath}";
    in repoRoot + "/${relPath}";

  loadTarget = target:
    let
      parsed = parseTarget target;
      src = zoneSource parsed.zone;
      targets = import (src + "/targets.nix") { inherit system; };
    in targets.${parsed.name};

  resolverModule = import ./resolver-module.nix;
  manifest = builtins.fromJSON (builtins.readFile ../../.meta/manifest.json);

  copyOnlyText = builtins.replaceStrings [ "\n" ] [ "" ] (builtins.readFile ./copy-only-marker.txt);
  copyOnlyTargets = {
    copyOnlyPrewarm = {
      name = "copyOnlyPrewarm";
      marker = "copyOnlyPrewarm:${copyOnlyText}:${system}";
      drvPath = "/nix/store/00000000000000000000000000000000-copy-only-prewarm-${copyOnlyText}.drv";
    };
    copyOnlyConsumer = {
      name = "copyOnlyConsumer";
      marker = "copyOnlyConsumer:${builtins.break copyOnlyText}:${system}";
      drvPath = "/nix/store/00000000000000000000000000000000-copy-only-consumer-${builtins.break copyOnlyText}.drv";
    };
  };
in
{
  resolve = target:
    let
      parsed = parseTarget target;
      base = if parsed.zone == "//system/tectonix-copy-value" then copyOnlyTargets.${parsed.name} else loadTarget target;
      moduleAttrs = if parsed.name == "resolverModuleUser" then {
        resolverModule = resolverModule.marker;
        drvPath = "/nix/store/00000000000000000000000000000000-resolver-module-user-${resolverModule.marker}.drv";
      } else { };
    in base // moduleAttrs // {
      target = target;
      resolvedBy = "clean-resolver";
    };

  allTargetNames = builtins.seq (builtins.attrNames manifest) [
    "//areas/app/web:alpha"
    "//areas/app/web:beta"
    "//areas/app/web:srcdir"
    "//areas/app/web:symlinked"
    "//areas/app/web:optional"
    "//areas/app/web:existsCheck"
    "//areas/app/web:fileTypeCheck"
    "//areas/app/web:readDirCheck"
    "//areas/app/web:readFileCheck"
    "//areas/app/web:readFileSharedA"
    "//areas/app/web:readFileSharedB"
    "//areas/app/web:sharedExistsA"
    "//areas/app/web:sharedExistsB"
    "//areas/app/web:sharedReadDirA"
    "//areas/app/web:sharedReadDirB"
    "//areas/app/web:closureMiddleUser"
    "//areas/app/web:closureChainA"
    "//areas/app/web:closureChainB"
    "//areas/app/web:resolverModuleUser"
    "//areas/app/web:nestedOptional"
    "//areas/app/web:treeShaCheck"
    "//areas/lib/shared:gamma"
  ];
}
RESOLVE_EOF

    cat > system/tectonix/resolver-module.nix << 'RESOLVER_MODULE_EOF'
{
  marker = "clean-resolver-module";
}
RESOLVER_MODULE_EOF

    cat > system/tectonix/copy-only-marker.txt << 'COPY_ONLY_MARKER_EOF'
clean-copy-only
COPY_ONLY_MARKER_EOF

    mkdir -p system/repo-root-read-dir
    cat > system/repo-root-read-dir/resolve.nix << 'ROOT_READ_DIR_RESOLVE_EOF'
{ system ? builtins.currentSystem }:
let
  rootEntries = builtins.attrNames (builtins.readDir ../..);
  rootEntriesSlug = builtins.concatStringsSep "-" rootEntries;
in
{
  allTargetNames = [ "//repo:rootReadDir" ];
  resolve = target: {
    name = "rootReadDir";
    inherit target system;
    drvPath = "/nix/store/00000000000000000000000000000000-root-read-dir-${rootEntriesSlug}.drv";
  };
}
ROOT_READ_DIR_RESOLVE_EOF

    cat > areas/app/web/zone.nix << 'ZONE_EOF'
{ }
ZONE_EOF

    cat > areas/app/web/common.nix << 'COMMON_EOF'
{ system }:
{
  marker = "clean-web-common";
  inherit system;
}
COMMON_EOF

    mkdir -p areas/app/web/src-dir
    cat > areas/app/web/src-dir/file-001.txt << 'SRC_EOF'
clean source file 001
SRC_EOF
    cat > areas/app/web/src-dir/file-002.txt << 'SRC_EOF'
clean source file 002
SRC_EOF
    # Symlinks inside materialized trees are serialized via readLink during
    # dumpPath; like file reads, they are covered by the directory-level
    # fingerprint and must not add per-symlink closure entries.
    ln -s file-001.txt areas/app/web/src-dir/link-001
    cat > areas/app/web/shared-read-file.txt << 'READ_FILE_EOF'
clean-shared-read-file-text
READ_FILE_EOF
    cat > areas/app/web/closure-leaf.txt << 'CLOSURE_LEAF_EOF'
clean-closure-leaf-text
CLOSURE_LEAF_EOF
    cat > areas/app/web/owner-collision-a.txt << 'OWNER_COLLISION_A_EOF'
clean-owner-collision-a
OWNER_COLLISION_A_EOF
    cat > areas/app/web/owner-collision-b.txt << 'OWNER_COLLISION_B_EOF'
clean-owner-collision-b
OWNER_COLLISION_B_EOF
    cat > areas/app/web/shared-owner.txt << 'SHARED_OWNER_EOF'
clean-shared-owner
SHARED_OWNER_EOF
    cat > areas/app/web/shared-builder-marker.txt << 'SHARED_BUILDER_MARKER_EOF'
clean-shared-builder
SHARED_BUILDER_MARKER_EOF

    cat > areas/app/web/parallel-only-marker.txt << 'PARALLEL_ONLY_MARKER_EOF'
clean-parallel-only-marker
PARALLEL_ONLY_MARKER_EOF

    cat > areas/app/web/filter-ignore.txt << 'FILTER_IGNORE_EOF'
file-002.txt
FILTER_IGNORE_EOF

    cat > areas/app/web/shared-builder.nix << 'SHARED_BUILDER_EOF'
{ common }:
let
  marker = builtins.replaceStrings [ "\n" ] [ "" ] (builtins.readFile ./shared-builder-marker.txt);
in
{
  value = "${marker}:${common.marker}";
}
SHARED_BUILDER_EOF

    cat > areas/app/web/targets.nix << 'TARGETS_EOF'
{ system }:
let
  common = import ./common.nix { inherit system; };
  sharedReadFile = builtins.readFile ./shared-read-file.txt;
  sharedExists = builtins.pathExists ./targets/exists-marker;
  sharedDirEntries = builtins.attrNames (builtins.readDir ./src-dir);
  closureLeaf = builtins.readFile ./closure-leaf.txt;
  closureMiddle = builtins.replaceStrings [ "\n" ] [ "" ] closureLeaf;
  closureOuter = "${closureMiddle}";
  sharedOwnerText = builtins.tecnixInternalSourceDepsScope (builtins.readFile ./shared-owner.txt);
  sharedBuilder = import ./shared-builder.nix { inherit common; };
  callPackage = f: args: f (args // { inherit common; });
  sharedCallPackageBuilder = callPackage (import ./shared-builder.nix) { };
in
{
  alpha = import ./targets/alpha.nix { inherit common system; };
  beta = import ./targets/beta.nix { inherit common system; };
  srcdir = import ./targets/srcdir.nix { inherit system; };
  symlinked = import ./targets/current.nix { inherit common system; };
  optional = import ./targets/optional.nix { inherit common system; };
  existsCheck = import ./targets/exists-check.nix { inherit common system; };
  fileTypeCheck = import ./targets/file-type-check.nix { inherit common system; };
  readDirCheck = import ./targets/read-dir-check.nix { inherit common system; };
  readFileCheck = import ./targets/read-file-check.nix { inherit common system; };
  readFileSharedA = import ./targets/read-file-shared.nix { targetName = "readFileSharedA"; drvName = "read-file-shared-a"; inherit common sharedReadFile system; };
  readFileSharedB = import ./targets/read-file-shared.nix { targetName = "readFileSharedB"; drvName = "read-file-shared-b"; inherit common sharedReadFile system; };
  sharedExistsA = import ./targets/shared-exists.nix { targetName = "sharedExistsA"; drvName = "shared-exists-a"; inherit common sharedExists system; };
  sharedExistsB = import ./targets/shared-exists.nix { targetName = "sharedExistsB"; drvName = "shared-exists-b"; inherit common sharedExists system; };
  sharedReadDirA = import ./targets/shared-read-dir.nix { targetName = "sharedReadDirA"; drvName = "shared-read-dir-a"; inherit common sharedDirEntries system; };
  sharedReadDirB = import ./targets/shared-read-dir.nix { targetName = "sharedReadDirB"; drvName = "shared-read-dir-b"; inherit common sharedDirEntries system; };
  closureMiddleUser = import ./targets/closure-middle.nix { targetName = "closureMiddleUser"; drvName = "closure-middle"; inherit closureMiddle common system; };
  closureChainA = import ./targets/closure-chain.nix { targetName = "closureChainA"; drvName = "closure-chain-a"; inherit closureOuter common system; };
  closureChainB = import ./targets/closure-chain.nix { targetName = "closureChainB"; drvName = "closure-chain-b"; inherit closureOuter common system; };
  resolverModuleUser = import ./targets/resolver-module-user.nix { inherit common system; };
  nestedOptional = import ./targets/nested-optional.nix { inherit common system; };
  parallelPrefetch = import ./targets/parallel-prefetch.nix { inherit common system; };
  jsonCheck = import ./targets/json-check.nix { inherit common system; };
  filteredSrc = import ./targets/filtered-src.nix { inherit common system; };
  treeShaCheck = import ./targets/tree-sha-check.nix { inherit common system; treeSha = builtins.unsafeTectonixInternalTreeSha "//areas/lib/shared"; };
  ownerCollisionA = import ./targets/owner-collision.nix {
    targetName = "ownerCollisionA";
    drvName = "owner-collision-a";
    ownerText = builtins.tecnixInternalSourceDepsScope (builtins.readFile ./owner-collision-a.txt);
    inherit common system;
  };
  ownerCollisionB = import ./targets/owner-collision.nix {
    targetName = "ownerCollisionB";
    drvName = "owner-collision-b";
    ownerText = builtins.tecnixInternalSourceDepsScope (builtins.readFile ./owner-collision-b.txt);
    inherit common system;
  };
  sharedOwnerA = import ./targets/shared-owner.nix { targetName = "sharedOwnerA"; drvName = "shared-owner-a"; inherit common sharedOwnerText system; };
  sharedOwnerB = import ./targets/shared-owner.nix { targetName = "sharedOwnerB"; drvName = "shared-owner-b"; inherit common sharedOwnerText system; };
  sharedBuilderA = import ./targets/shared-builder-user.nix { targetName = "sharedBuilderA"; drvName = "shared-builder-a"; inherit common sharedBuilder system; };
  sharedBuilderB = import ./targets/shared-builder-user.nix { targetName = "sharedBuilderB"; drvName = "shared-builder-b"; inherit common sharedBuilder system; };
  indirectBuilderA = import ./targets/indirect-builder-user.nix { targetName = "indirectBuilderA"; drvName = "indirect-builder-a"; inherit common system; };
  indirectBuilderB = import ./targets/indirect-builder-user.nix { targetName = "indirectBuilderB"; drvName = "indirect-builder-b"; inherit common system; };
  callPackageBuilderA = import ./targets/shared-builder-user.nix { targetName = "callPackageBuilderA"; drvName = "callpackage-builder-a"; common = common; sharedBuilder = sharedCallPackageBuilder; inherit system; };
  callPackageBuilderB = import ./targets/shared-builder-user.nix { targetName = "callPackageBuilderB"; drvName = "callpackage-builder-b"; common = common; sharedBuilder = sharedCallPackageBuilder; inherit system; };
}
TARGETS_EOF

    cat > areas/app/web/targets/alpha.nix << 'ALPHA_EOF'
{ common, system }:
{
  name = "alpha";
  marker = "alpha:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-alpha-${common.marker}.drv";
}
ALPHA_EOF

    cat > areas/app/web/targets/beta.nix << 'BETA_EOF'
{ common, system }:
{
  name = "beta";
  marker = "beta:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-beta-${common.marker}.drv";
}
BETA_EOF

    cat > areas/app/web/targets/srcdir.nix << 'SRCDIR_EOF'
{ system }:
derivation {
  name = "srcdir";
  inherit system;
  builder = "/bin/sh";
  args = [ "-c" "echo unused > $out" ];
  src = ../src-dir;
}
SRCDIR_EOF

    ln -s alpha.nix areas/app/web/targets/current.nix

    cat > areas/app/web/targets/optional.nix << 'OPTIONAL_EOF'
{ common, system }:
let
  optional = if builtins.pathExists ./optional-marker.nix then import ./optional-marker.nix else "missing";
in
{
  name = "optional";
  marker = "optional:${optional}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-optional-${optional}.drv";
}
OPTIONAL_EOF

    cat > areas/app/web/targets/parallel-prefetch.nix << 'PARALLEL_PREFETCH_EOF'
{ common, system }:
let
  # builtins.parallel only exists when the parallel-eval experimental feature
  # is enabled. Its first argument is prefetch-only: the result never consumes
  # it, so it must never appear in this target's source closure.
  parallel = xs: x: if builtins ? parallel then builtins.parallel xs x else x;
in
{
  name = "parallelPrefetch";
  # The target's forced result (drvPath) goes through builtins.parallel so the
  # prefetch list is offered during tracked evaluation.
  drvPath = parallel [ (builtins.readFile ../parallel-only-marker.txt) ]
    "/nix/store/00000000000000000000000000000000-parallel-prefetch-${common.marker}.drv";
}
PARALLEL_PREFETCH_EOF

    cat > areas/app/web/targets/filtered-src.nix << 'FILTERED_SRC_EOF'
{ common, system }:
let
  # The ignore list is read (readFile, not import: imports are also covered by
  # the parse-cache replay hook) for the first time from inside the path
  # filter, which runs during NAR serialization where dump-internal tracking
  # is suppressed. It is a real dependency and must appear in the closure.
  ignored = builtins.replaceStrings [ "\n" ] [ "" ] (builtins.readFile ../filter-ignore.txt);
  src = builtins.path {
    path = ../src-dir;
    name = "filtered-src";
    filter = path: type: baseNameOf path != ignored;
  };
in
{
  name = "filteredSrc";
  drvPath = builtins.seq src "/nix/store/00000000000000000000000000000000-filtered-src-${common.marker}.drv";
}
FILTERED_SRC_EOF

    cat > areas/app/web/targets/json-check.nix << 'JSON_CHECK_EOF'
{ common, system }:
let
  # builtins.toJSON deep-forces its argument; with a parallel executor this
  # prefetches attribute forcing, which must not spawn under tracking.
  json = builtins.toJSON {
    marker = common.marker;
    shared = builtins.readFile ../shared-read-file.txt;
  };
in
{
  name = "jsonCheck";
  # Forced via drvPath so the toJSON deep-force runs under tracking.
  drvPath = "/nix/store/00000000000000000000000000000000-json-check-${builtins.hashString "sha256" json}.drv";
}
JSON_CHECK_EOF

    touch areas/app/web/targets/exists-marker
    cat > areas/app/web/targets/exists-check.nix << 'EXISTS_CHECK_EOF'
{ common, system }:
let
  exists = if builtins.pathExists ./exists-marker then "present" else "missing";
in
{
  name = "existsCheck";
  marker = "exists:${exists}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-exists-${exists}.drv";
}
EXISTS_CHECK_EOF

    cat > areas/app/web/targets/file-type-check.nix << 'FILE_TYPE_CHECK_EOF'
{ common, system }:
let
  fileType = builtins.readFileType ./exists-marker;
in
{
  name = "fileTypeCheck";
  marker = "fileType:${fileType}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-file-type-${fileType}.drv";
}
FILE_TYPE_CHECK_EOF

    cat > areas/app/web/targets/read-dir-check.nix << 'READ_DIR_CHECK_EOF'
{ common, system }:
let
  entries = builtins.attrNames (builtins.readDir ../src-dir);
in
{
  name = "readDirCheck";
  marker = "readDir:${builtins.concatStringsSep "," entries}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-read-dir-${builtins.concatStringsSep "-" entries}.drv";
}
READ_DIR_CHECK_EOF

    cat > areas/app/web/targets/read-file-marker.txt << 'READ_FILE_MARKER_EOF'
clean-read-file-text
READ_FILE_MARKER_EOF

    cat > areas/app/web/targets/read-file-check.nix << 'READ_FILE_CHECK_EOF'
{ common, system }:
let
  text = builtins.replaceStrings [ "\n" ] [ "" ] (builtins.readFile ./read-file-marker.txt);
in
{
  name = "readFileCheck";
  marker = "readFile:${text}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-read-file-${text}.drv";
}
READ_FILE_CHECK_EOF

    cat > areas/app/web/targets/read-file-shared.nix << 'READ_FILE_SHARED_EOF'
{ common, drvName, sharedReadFile, system, targetName }:
let
  text = builtins.replaceStrings [ "\n" ] [ "" ] sharedReadFile;
in
{
  name = targetName;
  marker = "${targetName}:${text}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-${drvName}-${text}.drv";
}
READ_FILE_SHARED_EOF

    cat > areas/app/web/targets/shared-exists.nix << 'SHARED_EXISTS_EOF'
{ common, drvName, sharedExists, system, targetName }:
let
  exists = if sharedExists then "present" else "missing";
in
{
  name = targetName;
  marker = "${targetName}:${exists}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-${drvName}-${exists}.drv";
}
SHARED_EXISTS_EOF

    cat > areas/app/web/targets/shared-read-dir.nix << 'SHARED_READ_DIR_EOF'
{ common, drvName, sharedDirEntries, system, targetName }:
let
  entries = builtins.concatStringsSep "-" sharedDirEntries;
in
{
  name = targetName;
  marker = "${targetName}:${entries}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-${drvName}-${entries}.drv";
}
SHARED_READ_DIR_EOF

    cat > areas/app/web/targets/closure-middle.nix << 'CLOSURE_MIDDLE_EOF'
{ closureMiddle, common, drvName, system, targetName }:
{
  name = targetName;
  marker = "${targetName}:${closureMiddle}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-${drvName}-${closureMiddle}.drv";
}
CLOSURE_MIDDLE_EOF

    cat > areas/app/web/targets/closure-chain.nix << 'CLOSURE_CHAIN_EOF'
{ closureOuter, common, drvName, system, targetName }:
{
  name = targetName;
  marker = "${targetName}:${closureOuter}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-${drvName}-${closureOuter}.drv";
}
CLOSURE_CHAIN_EOF

    cat > areas/app/web/targets/resolver-module-user.nix << 'RESOLVER_MODULE_USER_EOF'
{ common, system }:
{
  name = "resolverModuleUser";
  marker = "resolverModuleUser:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-resolver-module-user-${common.marker}.drv";
}
RESOLVER_MODULE_USER_EOF

    cat > areas/app/web/targets/owner-collision.nix << 'OWNER_COLLISION_EOF'
{ common, system, targetName, drvName, ownerText }:
let
  cleanOwnerText = builtins.replaceStrings [ "\n" ] [ "" ] ownerText;
in
{
  name = targetName;
  marker = "${targetName}:${cleanOwnerText}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-${drvName}-${cleanOwnerText}-${common.marker}.drv";
}
OWNER_COLLISION_EOF

    cat > areas/app/web/targets/shared-owner.nix << 'SHARED_OWNER_TARGET_EOF'
{ common, system, targetName, drvName, sharedOwnerText }:
let
  cleanSharedOwnerText = builtins.replaceStrings [ "\n" ] [ "" ] sharedOwnerText;
in
{
  name = targetName;
  marker = "${targetName}:${cleanSharedOwnerText}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-${drvName}-${cleanSharedOwnerText}-${common.marker}.drv";
}
SHARED_OWNER_TARGET_EOF

    cat > areas/app/web/targets/shared-builder-user.nix << 'SHARED_BUILDER_USER_EOF'
{ common, system, targetName, drvName, sharedBuilder }:
{
  name = targetName;
  marker = "${targetName}:${sharedBuilder.value}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-${drvName}-${sharedBuilder.value}-${common.marker}.drv";
}
SHARED_BUILDER_USER_EOF

    cat > areas/app/web/targets/indirect-builder-user.nix << 'INDIRECT_BUILDER_USER_EOF'
{ common, system, targetName, drvName }:
let
  sharedBuilder = import ../shared-builder.nix { inherit common; };
in
{
  name = targetName;
  marker = "${targetName}:${sharedBuilder.value}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-${drvName}-${sharedBuilder.value}-${common.marker}.drv";
}
INDIRECT_BUILDER_USER_EOF

    cat > areas/app/web/targets/nested-optional.nix << 'NESTED_OPTIONAL_EOF'
{ common, system }:
let
  optional = if builtins.pathExists ../untracked-dir/nested-marker.nix then "present" else "missing";
in
{
  name = "nestedOptional";
  marker = "nested:${optional}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-nested-${optional}.drv";
}
NESTED_OPTIONAL_EOF

    cat > areas/app/web/targets/tree-sha-check.nix << 'TREE_SHA_CHECK_EOF'
{ common, system, treeSha }:
{
  name = "treeShaCheck";
  marker = "treeSha:${treeSha}:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-tree-sha-${treeSha}.drv";
}
TREE_SHA_CHECK_EOF

    cat > areas/lib/shared/zone.nix << 'ZONE_EOF'
{ }
ZONE_EOF

    cat > areas/lib/shared/common.nix << 'COMMON_EOF'
{ system }:
{
  marker = "clean-shared-common";
  inherit system;
}
COMMON_EOF

    cat > areas/lib/shared/targets.nix << 'TARGETS_EOF'
{ system }:
let
  common = import ./common.nix { inherit system; };
in
{
  gamma = import ./targets/gamma.nix { inherit common system; };
}
TARGETS_EOF

    cat > areas/lib/shared/targets/gamma.nix << 'GAMMA_EOF'
{ common, system }:
{
  name = "gamma";
  marker = "gamma:${common.marker}:${system}";
  common = common.marker;
  drvPath = "/nix/store/00000000000000000000000000000000-gamma-${common.marker}.drv";
}
GAMMA_EOF

    git config user.email "test@example.com"
    git config user.name "Test User"

    mkdir -p .git/info
    cat > .git/info/sparse-checkout-roots << 'SPARSE_EOF'
W-100001
W-100002
SPARSE_EOF

    git add -A
    git commit -m "Initial tecnix builtin test world"

    cd - > /dev/null
}

# Get the HEAD SHA of a repo
get_head_sha() {
    local dir="$1"
    git -C "$dir" rev-parse HEAD
}

fi # TECNIX_COMMON_SH_SOURCED
