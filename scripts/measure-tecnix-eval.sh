#!/usr/bin/env bash
# Evaluate Tecnix targets. The Tecnix SQLite eval cache is disabled by default;
# pass --eval-cache to measure the pure-eval cached path.
#
# By default this measures builtins.tecnixTargets by producing target records
# with drvPaths. Other modes are available for target values, dependency
# tracking, and target-name discovery.

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(cd "$script_dir/.." && pwd -P)

default_world_git="$HOME/world/git"

usage() {
    cat >&2 <<'EOF'
Usage: measure-tecnix-eval.sh [options]

Options:
  --git-dir PATH       World git directory (default: $GIT_DIR, ~/world/git if it exists, or WORLD/.git)
  --world PATH         World checkout path, used only to derive git-dir/rev unless --checkout-path is set
  --checkout-path PATH Pass checkoutPath to Tecnix args for dirty-overlay/source-available eval
  --resolver PATH      Repo-relative resolver file (default: $RESOLVER or system/tectonix/resolve.nix)
  --system SYSTEM      System string (default: $SYSTEM or builtins.currentSystem)
  --platform SYSTEM    Platform/system for --mode platform-target-dependencies or
                       platform-target-dependency-paths. May be repeated.
  --rev REV            Git revision to evaluate (default: $REV or git HEAD from git-dir/world)
  --target TARGET      Target to evaluate. May be repeated. If omitted, discovers all target names.
  --include-dependencies
                       Add a dependencies attr to target-records output
  --mode MODE          target-records-jsonl, target-records, target-drvs, targets,
                       target-dependencies, target-dependency-paths,
                       platform-target-dependencies,
                       platform-target-dependency-paths, or target-names
                       (default: target-records-jsonl)
  --nix PATH           nix executable (default: $NIX_BIN, ./build/src/nix/nix, or nix)
  --eval-cache         Enable the Tecnix SQLite eval cache (implies --pure)
  --no-eval-cache      Disable the Tecnix SQLite eval cache (default)
  --parallel           Enable parallel eval workers with --eval-cores 0
  --eval-cores N       Pass --eval-cores N (0 means auto; implies --parallel)
  -v, --verbose        Pass --verbose to nix (may be repeated)
  --debug              Pass --debug to nix
  --log-format FORMAT  Pass --log-format FORMAT to nix
  --pure               Use --pure-eval instead of --impure
  --impure             Use --impure (default; matches tec eval)
  --print-result       Print the final eval result to stdout (default)
  --output PATH        Write the final eval result to PATH instead of stdout
  --discard-output     Force the result but redirect stdout to /dev/null
  -h, --help           Show this help

Default behavior measures builtins.tecnixTargets by producing one JSON target
record per line ({ target, drvPath }) for each selected target. Pass
--include-dependencies to add source dependencies to each record. Dependency
modes use builtins.tecnixTargets with includeDependencies enabled. Use --mode
target-dependency-paths for the compact target -> [repo-relative path] graph, or
--mode target-dependencies for the target -> { path = fingerprint; } graph:
  nix eval --raw --lazy-trees --impure \
    --extra-experimental-features 'nix-command parallel-eval wasm-builtin' \
    --option tecnix-eval-cache false

By default the final expression deepSeqs the discovered target list and selected
result, then prints the result. Use --discard-output for timing-only runs or
--output PATH to save it. The Tecnix eval cache is disabled by default, but normal
in-process evaluator caches remain enabled where the evaluator chooses to use
them. Pass --eval-cache to enable the Tecnix SQLite eval cache and run with
--pure-eval, which is required for cache hits.
EOF
}

world=${WORLD:-}
checkout_path=${CHECKOUT_PATH:-}
git_dir=${GIT_DIR:-}
resolver=${RESOLVER:-system/tectonix/resolve.nix}
system=${SYSTEM:-}
rev=${REV:-}
nix_bin=${NIX_BIN:-}
mode=${MODE:-target-records-jsonl}
eval_cores=${EVAL_CORES:-}
pure_eval=0
print_result=1
output_path=${OUTPUT:-}
include_dependencies=${INCLUDE_DEPENDENCIES:-0}
tecnix_eval_cache=${TECNIX_EVAL_CACHE:-0}
targets=()
platforms=()
nix_log_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --git-dir)
            git_dir=$2
            shift 2
            ;;
        --world)
            world=$2
            shift 2
            ;;
        --checkout-path)
            checkout_path=$2
            shift 2
            ;;
        --resolver)
            resolver=$2
            shift 2
            ;;
        --system)
            system=$2
            shift 2
            ;;
        --platform)
            platforms+=("$2")
            shift 2
            ;;
        --rev)
            rev=$2
            shift 2
            ;;
        --target)
            targets+=("$2")
            shift 2
            ;;
        --mode)
            mode=$2
            shift 2
            ;;
        --include-dependencies)
            include_dependencies=1
            shift
            ;;
        --nix)
            nix_bin=$2
            shift 2
            ;;
        --eval-cache)
            tecnix_eval_cache=1
            pure_eval=1
            shift
            ;;
        --no-eval-cache)
            tecnix_eval_cache=0
            shift
            ;;
        --parallel)
            eval_cores=0
            shift
            ;;
        --eval-cores)
            eval_cores=$2
            shift 2
            ;;
        -v|--verbose)
            nix_log_args+=(--verbose)
            shift
            ;;
        --debug)
            nix_log_args+=(--debug)
            shift
            ;;
        --log-format)
            nix_log_args+=(--log-format "$2")
            shift 2
            ;;
        --pure)
            pure_eval=1
            shift
            ;;
        --impure)
            pure_eval=0
            shift
            ;;
        --print-result)
            print_result=1
            output_path=
            shift
            ;;
        --output)
            output_path=$2
            print_result=0
            shift 2
            ;;
        --discard-output)
            print_result=0
            output_path=
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage
            exit 2
            ;;
    esac
done

case "$mode" in
    target-records|target-records-jsonl|target-dependencies|target-dependency-paths|platform-target-dependencies|platform-target-dependency-paths|targets|target-drvs|target-names) ;;
    *)
        echo "invalid --mode '$mode' (expected target-records, target-records-jsonl, target-dependencies, target-dependency-paths, platform-target-dependencies, platform-target-dependency-paths, targets, target-drvs, or target-names)" >&2
        exit 2
        ;;
esac

if [[ ${#platforms[@]} -gt 0 && "$mode" != platform-target-dependencies && "$mode" != platform-target-dependency-paths ]]; then
    echo "--platform only applies to --mode platform-target-dependencies or platform-target-dependency-paths" >&2
    exit 2
fi

if [[ "$include_dependencies" == 1 ]]; then
    case "$mode" in
        target-records|target-records-jsonl) ;;
        *)
            echo "--include-dependencies only applies to target-records and target-records-jsonl modes" >&2
            exit 2
            ;;
    esac
fi

case "$tecnix_eval_cache" in
    1|true|yes|on)
        tecnix_eval_cache=1
        ;;
    0|false|no|off)
        tecnix_eval_cache=0
        ;;
    *)
        echo "invalid TECNIX_EVAL_CACHE/--eval-cache value '$tecnix_eval_cache'" >&2
        exit 2
        ;;
esac

if [[ "$tecnix_eval_cache" == 1 && "$pure_eval" != 1 ]]; then
    echo "--eval-cache requires pure evaluation; pass --pure or let --eval-cache imply it, and do not also pass --impure" >&2
    exit 2
fi

if [[ -z "$nix_bin" ]]; then
    if [[ -x "$repo_root/build/src/nix/nix" ]]; then
        nix_bin="$repo_root/build/src/nix/nix"
    else
        nix_bin=nix
    fi
fi

if [[ -n "$world" ]]; then
    world=$(cd "$world" && pwd -P)
elif [[ -d "$HOME/world" ]]; then
    world=$(cd "$HOME/world" && pwd -P)
fi

if [[ -z "$git_dir" ]]; then
    if [[ -d "$default_world_git" ]]; then
        git_dir=$default_world_git
    elif [[ -n "$world" ]]; then
        git_dir=$(git -C "$world" rev-parse --absolute-git-dir)
    else
        echo "could not determine git dir; pass --git-dir or --world" >&2
        exit 2
    fi
fi

git_dir=$(cd "$git_dir" && pwd -P)

if [[ -z "$rev" ]]; then
    if rev=$(git --git-dir "$git_dir" rev-parse HEAD 2>/dev/null); then
        :
    elif [[ -n "$world" ]]; then
        rev=$(git -C "$world" rev-parse HEAD)
    else
        echo "could not determine rev; pass --rev" >&2
        exit 2
    fi
fi

if ! git --git-dir "$git_dir" cat-file -e "$rev:$resolver" 2>/dev/null; then
    cat >&2 <<EOF
selected revision does not contain resolver '$resolver': $rev

Pass a revision with the Tecnix resolver, for example:
  $0 --rev 9b43b548315d8d448a72fc6f14cb8b206507e85c

Or set:
  REV=9b43b548315d8d448a72fc6f14cb8b206507e85c $0
EOF
    exit 2
fi

if [[ -z "$system" ]]; then
    system=$("$nix_bin" eval --raw --impure --extra-experimental-features 'nix-command' --expr 'builtins.currentSystem')
fi

nix_string() {
    python3 -c 'import json, sys; print(json.dumps(sys.stdin.read()))'
}

to_nix_string() {
    printf '%s' "$1" | nix_string
}

target_list_expr() {
    local items=()
    local target
    for target in "$@"; do
        items+=("$(to_nix_string "$target")")
    done
    printf '[ %s ]' "${items[*]}"
}

target_ref_list_expr_for_system() {
    shift
    target_list_expr "$@"
}

target_ref_list_expr_for_platforms() {
    target_list_expr "${targets[@]}"
}

git_dir_nix=$(to_nix_string "$git_dir")
resolver_nix=$(to_nix_string "$resolver")
system_nix=$(to_nix_string "$system")
rev_nix=$(to_nix_string "$rev")

checkout_attr=""
if [[ -n "$checkout_path" ]]; then
    checkout_path=$(cd "$checkout_path" && pwd -P)
    checkout_path_nix=$(to_nix_string "$checkout_path")
    checkout_attr="    checkoutPath = $checkout_path_nix;"
fi

tecnix_eval_cache_nix=false
if [[ "$tecnix_eval_cache" == 1 ]]; then
    tecnix_eval_cache_nix=true
fi

discover_target_names_for_system() {
    local discover_system=$1
    local discover_system_nix
    discover_system_nix=$(to_nix_string "$discover_system")
    local discover_expr
    discover_expr=$(cat <<EOF
let
  baseArgs = {
    gitDir = $git_dir_nix;
    resolver = $resolver_nix;
    args = { systems = [ $discover_system_nix ]; };
    rev = $rev_nix;
$checkout_attr
  };
in
  builtins.tecnixTargetNames baseArgs
EOF
)
    local discover_args=(
        eval
        --json
        --lazy-trees
        --extra-experimental-features 'nix-command parallel-eval wasm-builtin'
        --option tecnix-eval-cache "$tecnix_eval_cache_nix"
        --option tectonix-git-dir "$git_dir"
        --option tectonix-git-sha "$rev"
    )
    if [[ -n "$checkout_path" ]]; then
        discover_args+=(--option tectonix-checkout-path "$checkout_path")
    fi
    if [[ -n "$eval_cores" ]]; then
        discover_args+=(--eval-cores "$eval_cores")
    fi
    if [[ "$pure_eval" == 1 ]]; then
        discover_args+=(--pure-eval)
    else
        discover_args+=(--impure)
    fi
    discover_args+=(--expr "$discover_expr")

    local target_names_json
    target_names_json=$(mktemp -t tecnix-target-names.XXXXXX.json)
    "$nix_bin" "${discover_args[@]}" >"$target_names_json"
    python3 - "$target_names_json" <<'PY'
import json
import sys
with open(sys.argv[1]) as f:
    for target in json.load(f):
        print(target)
PY
    rm -f "$target_names_json"
}

targets_discovered_separately=0
if [[ ${#targets[@]} -eq 0 ]]; then
    case "$mode" in
        target-dependencies|target-dependency-paths)
            # Discover target names in a separate Nix process for dependency
            # modes. Discovering names in the same evaluator can pre-force broad
            # resolver/module values and distort the per-target dependency graph
            # that the second phase is trying to measure.
            while IFS= read -r target; do
                targets+=("$target")
            done < <(discover_target_names_for_system "$system")
            targets_discovered_separately=1
            ;;
    esac
fi

include_dependencies_nix=false
if [[ "$include_dependencies" == 1 ]]; then
    include_dependencies_nix=true
fi

system_label=$system

if [[ "$mode" == platform-target-dependencies || "$mode" == platform-target-dependency-paths ]]; then
    if [[ ${#platforms[@]} -eq 0 ]]; then
        platforms=("$system")
    fi

    platforms_expr=$(target_list_expr "${platforms[@]}")

    if [[ ${#targets[@]} -gt 0 ]]; then
        target_refs_expr=$(target_ref_list_expr_for_platforms)
        targets_label="${#targets[@]} explicit target ref(s)"
    else
        target_refs_expr='builtins.tecnixTargetNames baseArgs'
        targets_label="all discovered target refs for platforms (${platforms[*]})"
    fi

    if [[ "$mode" == platform-target-dependencies ]]; then
        result_expr='targetDependencyPathSets'
    else
        result_expr='targetDependencyPaths'
    fi
    seq_expr='builtins.deepSeq platforms (builtins.deepSeq targetRefs (builtins.deepSeq result result))'
    discard_seq_expr='builtins.deepSeq platforms (builtins.deepSeq targetRefs (builtins.deepSeq result "ok"))'

    if [[ "$print_result" == 1 || -n "$output_path" ]]; then
        final_expr=$seq_expr
        eval_output_flag=--json
    else
        final_expr=$discard_seq_expr
        eval_output_flag=--raw
    fi

    expr=$(cat <<EOF
let
  baseArgs = {
    gitDir = $git_dir_nix;
    resolver = $resolver_nix;
    rev = $rev_nix;
    args = { systems = $platforms_expr; };
$checkout_attr
  };

  platforms = $platforms_expr;
  targetRefs = $target_refs_expr;
  targetDependencyRecords = builtins.tecnixTargets (baseArgs // {
    targets = targetRefs;
    includeDependencies = true;
    includeTargets = false;
  });
  targetDependencyPaths = builtins.listToAttrs (map (record: {
    name = record.target;
    value = builtins.attrNames record.dependencies;
  }) targetDependencyRecords);
  targetDependencyPathSets = builtins.listToAttrs (map (record: {
    name = record.target;
    value = record.dependencies;
  }) targetDependencyRecords);
  result = $result_expr;
in
  $final_expr
EOF
)

    system_label="${platforms[*]}"
    if [[ "$mode" == platform-target-dependencies ]]; then
        dependencies_label="fingerprints by platform"
    else
        dependencies_label="paths only by platform"
    fi
else
    if [[ ${#targets[@]} -eq 0 ]]; then
        targets_expr='builtins.tecnixTargetNames baseArgs'
        targets_label='all discovered target refs'
    else
        targets_expr=$(target_ref_list_expr_for_system "$system" "${targets[@]}")
        if [[ "$targets_discovered_separately" == 1 ]]; then
            targets_label="${#targets[@]} discovered target ref(s) from separate discovery"
        else
            targets_label="${#targets[@]} explicit target ref(s)"
        fi
    fi

    result_is_raw=0
    case "$mode" in
        target-records-jsonl)
            result_expr='targetRecordsJsonl'
            result_is_raw=1
            ;;
        target-records)
            result_expr='targetRecords'
            ;;
        target-dependencies)
            result_expr='targetDependencyPathSets'
            ;;
        target-dependency-paths)
            result_expr='targetDependencyPaths'
            ;;
        targets)
            result_expr='targetValues'
            ;;
        target-drvs)
            result_expr='builtins.mapAttrs (_target: target: target.drvPath) targetValues'
            ;;
        target-names)
            result_expr='targetNames'
            ;;
    esac

    if [[ "$print_result" == 1 || -n "$output_path" ]]; then
        final_expr='builtins.deepSeq targetNames (builtins.deepSeq result result)'
        if [[ "$result_is_raw" == 1 ]]; then
            eval_output_flag=--raw
        else
            eval_output_flag=--json
        fi
    else
        final_expr='builtins.deepSeq targetNames (builtins.deepSeq result "ok")'
        eval_output_flag=--raw
    fi

    expr=$(cat <<EOF
let
  baseArgs = {
    gitDir = $git_dir_nix;
    resolver = $resolver_nix;
    args = { systems = [ $system_nix ]; };
    rev = $rev_nix;
$checkout_attr
  };

  targetNames = $targets_expr;

  targetValues = builtins.tecnixTargets (baseArgs // { targets = targetNames; });

  targetDependencyRecords = builtins.tecnixTargets (baseArgs // {
    targets = targetNames;
    includeDependencies = true;
    includeTargets = false;
  });
  targetDependencyPaths = builtins.listToAttrs (map (record: {
    name = record.target;
    value = builtins.attrNames record.dependencies;
  }) targetDependencyRecords);
  targetDependencyPathSets = builtins.listToAttrs (map (record: {
    name = record.target;
    value = record.dependencies;
  }) targetDependencyRecords);

  targetRecords = map (target:
    {
      inherit target;
      drvPath = (builtins.getAttr target targetValues).drvPath;
    }
    // (if $include_dependencies_nix then {
      dependencies = builtins.getAttr target targetDependencyPathSets;
    } else { }))
    targetNames;

  targetRecordsJsonl = builtins.concatStringsSep "\n" (map builtins.toJSON targetRecords) + "\n";

  result = $result_expr;
in
  $final_expr
EOF
)

    case "$mode" in
        target-records|target-records-jsonl)
            dependencies_label=$([[ "$include_dependencies" == 1 ]] && echo included || echo omitted)
            ;;
        target-dependencies)
            dependencies_label="fingerprints"
            ;;
        target-dependency-paths)
            dependencies_label="paths only"
            ;;
        *)
            dependencies_label="omitted"
            ;;
    esac
fi

if [[ -n "$output_path" ]]; then
    stdout_label="result -> $output_path"
elif [[ "$print_result" == 1 ]]; then
    stdout_label="result"
else
    stdout_label="discarded"
fi

cat >&2 <<EOF
Evaluating Tecnix $mode
  gitDir:       $git_dir
  rev:          $rev
  resolver:     $resolver
  system(s):    $system_label
  targets:      $targets_label
  nix:          $nix_bin
  pureEval:     $([[ "$pure_eval" == 1 ]] && echo true || echo false)
  eval cores:   ${eval_cores:-default}
  eval cache:   tecnix-eval-cache=$tecnix_eval_cache_nix
  dependencies: $dependencies_label
  stdout:       $stdout_label
EOF

if [[ ${#nix_log_args[@]} -gt 0 ]]; then
    printf '  nix logging:  ' >&2
    printf '%q ' "${nix_log_args[@]}" >&2
    printf '\n' >&2
fi

if [[ -n "$checkout_path" ]]; then
    echo "  checkoutPath: $checkout_path" >&2
fi

nix_args=(
    eval
    "$eval_output_flag"
    --lazy-trees
    --extra-experimental-features 'nix-command parallel-eval wasm-builtin'
    --option tecnix-eval-cache "$tecnix_eval_cache_nix"
    --option tectonix-git-dir "$git_dir"
    --option tectonix-git-sha "$rev"
)

if [[ ${#nix_log_args[@]} -gt 0 ]]; then
    nix_args+=("${nix_log_args[@]}")
fi

nix_args+=(--expr "$expr")

if [[ -n "$checkout_path" ]]; then
    nix_args+=(--option tectonix-checkout-path "$checkout_path")
fi

if [[ -n "$eval_cores" ]]; then
    nix_args+=(--eval-cores "$eval_cores")
fi

if [[ "$pure_eval" == 1 ]]; then
    nix_args+=(--pure-eval)
else
    nix_args+=(--impure)
fi

if [[ -n "$output_path" ]]; then
    mkdir -p "$(dirname "$output_path")"
    exec /usr/bin/time -p "$nix_bin" "${nix_args[@]}" >"$output_path"
elif [[ "$print_result" == 1 ]]; then
    exec /usr/bin/time -p "$nix_bin" "${nix_args[@]}"
else
    exec /usr/bin/time -p "$nix_bin" "${nix_args[@]}" >/dev/null
fi
