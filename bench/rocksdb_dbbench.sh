#!/usr/bin/env bash
# Run identical db_bench workloads against RocksDB and lsmkv, and report
# throughput plus write amplification for each.
#
# Write amplification = device bytes written / user payload bytes, measured
# two ways:
#   * per process: /proc/<pid>/io write_bytes, sampled while the benchmark
#     runs (the last sample before exit is kept);
#   * per device:  sectors written from /sys/block/$DEVICE/stat (the same
#     counter iostat reports) when DEVICE is set, e.g. DEVICE=nvme0n1.
#     This includes everything on the device, so run on an otherwise idle
#     disk.
#
# Usage:
#   bench/rocksdb_dbbench.sh                  # build RocksDB if needed, run both
#   DB_ROOT=/mnt/nvme/bench DEVICE=nvme0n1 NUM=10000000 bench/rocksdb_dbbench.sh
#   ENGINES=lsmkv bench/rocksdb_dbbench.sh    # only one engine
#
# Environment (defaults in brackets):
#   ROCKSDB_VERSION [v11.8.1]   git tag to build db_bench from
#   ROCKSDB_SRC     [./third_party/rocksdb]
#   DB_BENCH        path to an existing db_bench (skips the build)
#   KVBENCH         [./build/kvbench]
#   DB_ROOT         [/tmp/lsmkv-bench]   directory for both databases
#   BENCHMARKS      [fillrandom,readrandom,overwrite]
#   NUM [1000000] KEY_SIZE [16] VALUE_SIZE [100] THREADS [1] SEED [42]
#   SYNC [0] CACHE_SIZE [268435456] WRITE_BUFFER_SIZE [67108864]
#   ENGINES [rocksdb lsmkv]  DEVICE []
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROCKSDB_VERSION="${ROCKSDB_VERSION:-v11.8.1}"
ROCKSDB_SRC="${ROCKSDB_SRC:-$ROOT/third_party/rocksdb}"
KVBENCH="${KVBENCH:-$ROOT/build/kvbench}"
DB_ROOT="${DB_ROOT:-/tmp/lsmkv-bench}"
BENCHMARKS="${BENCHMARKS:-fillrandom,readrandom,overwrite}"
NUM="${NUM:-1000000}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-100}"
THREADS="${THREADS:-1}"
SEED="${SEED:-42}"
SYNC="${SYNC:-0}"
CACHE_SIZE="${CACHE_SIZE:-268435456}"
WRITE_BUFFER_SIZE="${WRITE_BUFFER_SIZE:-67108864}"
ENGINES="${ENGINES:-rocksdb lsmkv}"
DEVICE="${DEVICE:-}"
RESULTS="${RESULTS:-$DB_ROOT/results}"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

# Flags understood identically by db_bench and kvbench. Same seed + same key
# generator + same value generator => same workload.
common_flags() {
  printf '%s ' \
    --benchmarks="$BENCHMARKS" \
    --num="$NUM" \
    --key_size="$KEY_SIZE" \
    --value_size="$VALUE_SIZE" \
    --threads="$THREADS" \
    --seed="$SEED" \
    --sync="$SYNC" \
    --compression_ratio=0.5 \
    --cache_size="$CACHE_SIZE" \
    --write_buffer_size="$WRITE_BUFFER_SIZE" \
    --target_file_size_base=67108864 \
    --max_bytes_for_level_base=268435456 \
    --level0_file_num_compaction_trigger=4 \
    --bloom_bits=10 \
    --histogram=1
}

build_db_bench() {
  if [[ -n "${DB_BENCH:-}" ]]; then
    [[ -x "$DB_BENCH" ]] || { log "DB_BENCH=$DB_BENCH is not executable"; exit 1; }
    return
  fi
  DB_BENCH="$ROCKSDB_SRC/db_bench"
  if [[ -x "$DB_BENCH" ]]; then return; fi
  if [[ ! -d "$ROCKSDB_SRC/.git" ]]; then
    log "cloning RocksDB $ROCKSDB_VERSION into $ROCKSDB_SRC"
    git clone --depth 1 --branch "$ROCKSDB_VERSION" https://github.com/facebook/rocksdb.git "$ROCKSDB_SRC"
  fi
  log "building db_bench (release, PORTABLE=0); this takes a while"
  make -C "$ROCKSDB_SRC" -j"$(nproc)" DEBUG_LEVEL=0 PORTABLE=0 USE_RTTI=1 db_bench
}

device_sectors_written() {
  # Field 7 of /sys/block/<dev>/stat = sectors written (512 B units).
  [[ -n "$DEVICE" && -r "/sys/block/$DEVICE/stat" ]] || { echo 0; return; }
  awk '{print $7}' "/sys/block/$DEVICE/stat"
}

# run_engine NAME OUTFILE CMD... — runs CMD, samples /proc/<pid>/io until it
# exits, and appends a write-amplification summary to OUTFILE.
run_engine() {
  local name="$1" out="$2"
  shift 2
  local sectors_before io_sample=0 rchar_sample=0
  sectors_before="$(device_sectors_written)"
  log "running $name: $*"
  "$@" >"$out" 2>&1 &
  local pid=$!
  while kill -0 "$pid" 2>/dev/null; do
    if [[ -r "/proc/$pid/io" ]]; then
      local wb
      wb="$(awk '/^write_bytes:/ {print $2}' "/proc/$pid/io" 2>/dev/null || true)"
      [[ -n "$wb" ]] && io_sample="$wb"
      wb="$(awk '/^wchar:/ {print $2}' "/proc/$pid/io" 2>/dev/null || true)"
      [[ -n "$wb" ]] && rchar_sample="$wb"
    fi
    sleep 0.2
  done
  local rc=0
  wait "$pid" || rc=$?
  local sectors_after
  sectors_after="$(device_sectors_written)"

  # User payload = (key + value) bytes of every write in the run.
  local writes=0 b
  IFS=',' read -ra bs <<<"$BENCHMARKS"
  for b in "${bs[@]}"; do
    case "$b" in fillrandom | fillseq | overwrite) writes=$((writes + NUM)) ;; esac
  done
  local user=$((writes * (KEY_SIZE + VALUE_SIZE)))
  {
    echo
    echo "== $name write amplification (user payload $user bytes) =="
    if ((user > 0)); then
      awk -v d="$io_sample" -v u="$user" 'BEGIN { printf "  /proc/<pid>/io write_bytes : %d  (WA %.2fx, last sample)\n", d, d / u }'
      awk -v d="$rchar_sample" -v u="$user" 'BEGIN { printf "  /proc/<pid>/io wchar       : %d  (%.2fx; excludes io_uring)\n", d, d / u }'
      if [[ -n "$DEVICE" ]]; then
        awk -v a="$sectors_before" -v z="$sectors_after" -v u="$user" \
          'BEGIN { d = (z - a) * 512; printf "  /sys/block/'"$DEVICE"' (iostat): %d  (device WA %.2fx)\n", d, d / u }'
      fi
    fi
    echo "  exit status: $rc"
  } >>"$out"
  return "$rc"
}

main() {
  mkdir -p "$DB_ROOT" "$RESULTS"
  local flags
  flags="$(common_flags)"
  for engine in $ENGINES; do
    case "$engine" in
      rocksdb)
        build_db_bench
        rm -rf "$DB_ROOT/rocksdb"
        # shellcheck disable=SC2086
        run_engine "RocksDB $ROCKSDB_VERSION" "$RESULTS/rocksdb.txt" "$DB_BENCH" $flags \
          --db="$DB_ROOT/rocksdb" --use_existing_db=0 \
          --use_direct_reads=1 --use_direct_io_for_flush_and_compaction=1 \
          --compression_type=none --max_background_jobs=2 --statistics=0
        ;;
      lsmkv)
        [[ -x "$KVBENCH" ]] || { log "build kvbench first: cmake --build build --target kvbench"; exit 1; }
        rm -rf "$DB_ROOT/lsmkv"
        # shellcheck disable=SC2086
        run_engine "lsmkv" "$RESULTS/lsmkv.txt" "$KVBENCH" $flags \
          --db="$DB_ROOT/lsmkv" --use_existing_db=0 --use_direct_io=1 --io_backend=auto
        ;;
      *)
        log "unknown engine '$engine'"
        exit 1
        ;;
    esac
  done

  echo
  for engine in $ENGINES; do
    local f="$RESULTS/$engine.txt"
    [[ -f "$f" ]] || continue
    echo "################ $engine"
    grep -E '^(fillrandom|fillseq|readrandom|readseq|overwrite) +:|Microseconds per op|P50|== |WA|device' "$f" || true
    echo
  done
  log "full logs in $RESULTS"
}

main "$@"
