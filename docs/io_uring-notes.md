# io_uring in lsmkv: deployment notes

## What lsmkv needs from the kernel

| Feature | Used for | Minimum kernel |
|---|---|---|
| `io_uring_setup/enter/register` | everything | 5.1 |
| `IORING_OP_READ` / `WRITE` | data and WAL I/O (falls back to `READV`/`WRITEV` when the probe says no) | 5.6 |
| `IORING_REGISTER_BUFFERS` + `READ_FIXED` / `WRITE_FIXED` | block-cache fills, table and WAL staging buffers | 5.1 |
| `IORING_REGISTER_FILES` (table of `-1` slots) + `FILES_UPDATE` | fixed-file slots per open SSTable/WAL (`uring_register_files`) | 5.5; if registration fails, files aren't registered |
| `O_DIRECT` on the filesystem | all data paths | ext4 and xfs: yes. tmpfs: no (buffered fallback) |

The rings are sharded: `uring_num_rings` rings (default 4). Each thread has a home
shard and steals a free shard with `try_lock`. Submission returns
`-EAGAIN`/`-EBUSY` when the CQ is full; the backend then reaps completions and
resubmits. `-EINTR` from `io_uring_enter` is retried. A CQE with `-EAGAIN` is
resubmitted, and a short read or write is continued from where it stopped.
`-EIO` and every other error is returned as `Status::IOError` with the errno
attached, never retried.

Durability barriers (`fsync`/`fdatasync`) deliberately do **not** use
`IORING_OP_FSYNC`. They go through libc, are called exactly once, and a
failure aborts the process (see "fsyncgate" in `design.md`).

With `Options::io_backend = kAuto` (the default), if `io_uring_queue_init`
fails with `EPERM`/`ENOSYS` lsmkv logs one line and uses the preadv2/pwritev2
backend:

```
lsmkv: io_uring unavailable (IOError: io_uring_queue_init (errno 1: Operation not permitted)); using preadv2/pwritev2 backend
```

`kUring` makes that condition a hard `Open()` error. Use it in production if
you'd rather fail loudly than silently run the slower path.

## Docker: the default seccomp profile blocks io_uring

Since Docker Engine 25.0 (moby PR #46762, following Google's 2023
"io_uring is a major source of kernel exploits" findings), the default seccomp
profile no longer allows `io_uring_setup`, `io_uring_enter` or
`io_uring_register`. Inside a default container they fail with `EPERM`, so
lsmkv falls back to pread. containerd, Podman's default profile, and
Kubernetes `RuntimeDefault` (which inherits the runtime's profile) behave the
same way.

Check from inside the container:

```sh
grep Seccomp /proc/self/status        # 2 = filter active
./build/kvbench --benchmarks=fillrandom --num=1000 --io_backend=uring --db=/tmp/x
# -> "Open failed: ... io_uring_queue_init (errno 1: Operation not permitted)" when blocked
```

### Option A (recommended): the default profile plus the three syscalls

Start from the profile of the Docker version you actually run, so you don't
lose any of its other restrictions:

```sh
curl -fsSLo seccomp-default.json \
  https://raw.githubusercontent.com/moby/moby/v27.3.1/profiles/seccomp/default.json
```

Then add one rule to `syscalls`. A minimal patch with `jq`:

```sh
jq '.syscalls += [{
      "names": ["io_uring_setup", "io_uring_enter", "io_uring_register"],
      "action": "SCMP_ACT_ALLOW",
      "comment": "lsmkv: io_uring backend"
    }]' seccomp-default.json > seccomp-lsmkv.json
```

The added rule on its own:

```json
{
  "names": ["io_uring_setup", "io_uring_enter", "io_uring_register"],
  "action": "SCMP_ACT_ALLOW",
  "comment": "lsmkv: io_uring backend"
}
```

Run with it:

```sh
docker run --rm -it \
  --security-opt seccomp=$PWD/seccomp-lsmkv.json \
  --ulimit memlock=-1:-1 \
  -v /mnt/nvme/lsmkv:/data \
  lsmkv:latest kvbench --benchmarks=fillrandom,readrandom --db=/data/db --io_backend=uring
```

`docker-compose.yml`:

```yaml
services:
  lsmkv:
    image: lsmkv:latest
    security_opt:
      - seccomp=./seccomp-lsmkv.json
    ulimits:
      memlock: { soft: -1, hard: -1 }
    volumes:
      - /mnt/nvme/lsmkv:/data
```

Kubernetes (1.19+). Put the JSON on each node under
`<kubelet-root>/seccomp/profiles/` and reference it:

```yaml
securityContext:
  seccompProfile:
    type: Localhost
    localhostProfile: profiles/seccomp-lsmkv.json
```

### Option B: no seccomp at all (testing only)

```sh
docker run --security-opt seccomp=unconfined ...
```

This removes every syscall filter, not just the io_uring block. Don't ship it.

### Other knobs that also block io_uring

* **`kernel.io_uring_disabled`** (Linux 6.6+): `0` = allowed, `1` = only
  processes in the group `kernel.io_uring_group` (or with `CAP_SYS_ADMIN`),
  `2` = disabled for everyone. Check with `sysctl kernel.io_uring_disabled`.
  ChromeOS, Android and some hardened distros set it to 1 or 2.
* **AppArmor/SELinux** policies on some distributions deny io_uring
  separately (for example SELinux `io_uring { sqpoll override_creds }`
  permissions).
* **gVisor** implements only a subset of io_uring and is disabled by default
  (`--iouring`). Expect the pread fallback there.

## RLIMIT_MEMLOCK and registered buffers

Before Linux 5.12, registered buffers were charged against `RLIMIT_MEMLOCK`.
On 5.12+ they're charged to the memory cgroup, though some kernels and
container setups still enforce the rlimit. The common default limit is
64 KiB, smaller than a single 256 KiB fixed buffer (`uring_fixed_buffer_size`). lsmkv therefore
registers buffers best-effort: it tries `uring_fixed_buffers` ×
`uring_fixed_buffer_size`, halves the count on `ENOMEM`/`EPERM`/`EFAULT`, and
if nothing can be registered it uses plain `READ`/`WRITE` with unregistered
buffers. Correctness is unaffected; you only lose the
per-I/O page-pinning saving.

To get fixed buffers:

```sh
ulimit -l unlimited                         # shell
docker run --ulimit memlock=-1:-1 ...       # Docker
# systemd unit: LimitMEMLOCK=infinity
```

## Building with io_uring

lsmkv uses the system liburing (`pkg-config liburing`, ≥ 2.2). Without it:

* `-DLSMKV_FETCH_DEPS=ON` builds liburing 2.7 from source via ExternalProject;
* otherwise the build continues *without* the uring backend
  (`LSMKV_HAVE_URING=0`, and `NewUringBackend` returns `NotSupported`).
  Pass `-DLSMKV_REQUIRE_URING=ON` to make that a configure error.

```sh
sudo apt install liburing-dev          # Debian/Ubuntu
sudo dnf install liburing-devel        # Fedora/RHEL
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLSMKV_REQUIRE_URING=ON
```

## Measuring the device floor

`bench/fio/nvme_randread_qd1.fio` and `nvme_randread_qd32.fio` measure raw
4 KiB `O_DIRECT` random reads through io_uring with fixed buffers and files.
That's the same I/O shape as a cache-missing lsmkv point lookup. Compare
kvbench `readrandom` P50/P99 against the QD1 run: the difference is engine
overhead (index lookup, bloom probe, block parse, cache insertion).
