#if LSMKV_HAVE_URING

#include "io/uring.hpp"

#include <sys/resource.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace lsm {

// ---------------------------------------------------------------------------
// FixedBufferPool

FixedBufferPool::FixedBufferPool(size_t count, size_t buf_size) : buf_size_(AlignUp(buf_size)) {
  void* p = nullptr;
  if (::posix_memalign(&p, kIoAlignment, count * buf_size_) != 0) throw std::bad_alloc();
  slab_ = static_cast<char*>(p);
  std::memset(slab_, 0, count * buf_size_);  // Fault pages in before pinning.
  iovecs_.resize(count);
  free_.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    iovecs_[i] = iovec{slab_ + i * buf_size_, buf_size_};
    free_.push_back(static_cast<int>(count - 1 - i));
  }
}

FixedBufferPool::~FixedBufferPool() { std::free(slab_); }

AlignedBuffer FixedBufferPool::Acquire(size_t capacity, const std::shared_ptr<FixedBufferPool>& self) {
  if (AlignUp(capacity) > buf_size_) return {};
  int idx;
  {
    std::lock_guard lk(mu_);
    if (free_.empty()) return {};
    idx = free_.back();
    free_.pop_back();
  }
  return AlignedBuffer::FromRecycler(static_cast<char*>(iovecs_[static_cast<size_t>(idx)].iov_base),
                                     buf_size_, idx, self);
}

void FixedBufferPool::Recycle(char* /*data*/, int index) noexcept {
  std::lock_guard lk(mu_);
  free_.push_back(index);
}

// ---------------------------------------------------------------------------
// UringBackend

namespace {

struct InFlight {
  size_t done = 0;
  int slot = -1;
  bool finished = false;
  iovec iov{};
};

// Per-thread scratch so the hot path never allocates (the ring mutex is
// held while it is used, but one thread can only be in one Submit at a time).
thread_local std::vector<InFlight> tl_state;
thread_local std::vector<uint32_t> tl_pending;

std::atomic<unsigned> g_thread_counter{0};
thread_local const unsigned tl_thread_index = g_thread_counter.fetch_add(1, std::memory_order_relaxed);

}  // namespace

UringBackend::UringBackend(const Options& o)
    : queue_depth_(o.uring_queue_depth == 0 ? 64 : o.uring_queue_depth),
      num_rings_(o.uring_num_rings == 0 ? 1 : o.uring_num_rings),
      fixed_buffer_count_(o.uring_fixed_buffers),
      fixed_buffer_size_(o.uring_fixed_buffer_size),
      want_fixed_files_(o.uring_register_files),
      sqpoll_(o.uring_sqpoll) {}

std::shared_ptr<UringBackend> UringBackend::Create(const Options& options, Status* status) {
  std::shared_ptr<UringBackend> b(new UringBackend(options));
  *status = b->Init();
  if (!status->ok()) return nullptr;
  return b;
}

Status UringBackend::Init() {
  rings_.reserve(num_rings_);
  for (unsigned i = 0; i < num_rings_; ++i) {
    auto r = std::make_unique<Ring>();
    io_uring_params p{};
    if (sqpoll_) {
      p.flags |= IORING_SETUP_SQPOLL;
      p.sq_thread_idle = 2000;
    }
#ifdef IORING_SETUP_COOP_TASKRUN
    p.flags |= IORING_SETUP_COOP_TASKRUN;  // 5.19+: avoid IPIs for task_work.
#endif
    int rc = io_uring_queue_init_params(queue_depth_, &r->ring, &p);
#ifdef IORING_SETUP_COOP_TASKRUN
    if (rc == -EINVAL) {  // Older kernel: retry without the optional flag.
      p = io_uring_params{};
      if (sqpoll_) {
        p.flags |= IORING_SETUP_SQPOLL;
        p.sq_thread_idle = 2000;
      }
      rc = io_uring_queue_init_params(queue_depth_, &r->ring, &p);
    }
#endif
    if (rc < 0) {
      if (rc == -EPERM || rc == -ENOSYS) {
        return Status::NotSupported(
            "io_uring_setup rejected (kernel.io_uring_disabled, or a seccomp profile such as "
            "Docker's default; see docs/io_uring-notes.md): " + std::string(std::strerror(-rc)));
      }
      return Status::IOError("io_uring_queue_init_params", -rc);
    }
    r->initialized = true;
    rings_.push_back(std::move(r));
  }

  if (io_uring_probe* probe = io_uring_get_probe_ring(&rings_[0]->ring)) {
    use_rw_ops_ = io_uring_opcode_supported(probe, IORING_OP_READ) &&
                  io_uring_opcode_supported(probe, IORING_OP_WRITE);
    io_uring_free_probe(probe);
  } else {
    use_rw_ops_ = false;  // No probe support => pre-5.6 kernel.
  }

  SetupFixedBuffers();
  if (want_fixed_files_) SetupFixedFiles();
  return Status::OK();
}

void UringBackend::SetupFixedBuffers() {
  // Registered buffers are pinned and (before 5.12-ish, and still for many
  // setups) charged against RLIMIT_MEMLOCK, so registration is best effort:
  // halve the count until it fits, else run without fixed buffers.
  for (size_t count = fixed_buffer_count_; count > 0; count /= 2) {
    auto pool = std::make_shared<FixedBufferPool>(count, fixed_buffer_size_);
    size_t registered = 0;
    int rc = 0;
    for (; registered < rings_.size(); ++registered) {
      rc = io_uring_register_buffers(&rings_[registered]->ring, pool->iovecs().data(),
                                     static_cast<unsigned>(pool->iovecs().size()));
      if (rc < 0) break;
    }
    if (rc >= 0) {
      pool_ = std::move(pool);
      return;
    }
    for (size_t i = 0; i < registered; ++i) io_uring_unregister_buffers(&rings_[i]->ring);
    if (rc != -ENOMEM && rc != -EPERM && rc != -EFAULT) break;  // Not a sizing problem.
  }
}

void UringBackend::SetupFixedFiles() {
  rlimit rl{};
  unsigned table = 1024;
  if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur / 2 < table) {
    table = static_cast<unsigned>(rl.rlim_cur / 2);
  }
  if (table == 0) return;
  // A table of -1 entries is a sparse set (5.5+); slots are filled with
  // io_uring_register_files_update as files are opened.
  std::vector<int> fds(table, -1);
  size_t registered = 0;
  for (; registered < rings_.size(); ++registered) {
    if (io_uring_register_files(&rings_[registered]->ring, fds.data(), table) < 0) break;
  }
  if (registered != rings_.size()) {
    for (size_t i = 0; i < registered; ++i) io_uring_unregister_files(&rings_[i]->ring);
    return;
  }
  fixed_files_ = true;
  file_table_size_ = table;
  free_slots_.reserve(table);
  for (unsigned i = table; i-- > 0;) free_slots_.push_back(static_cast<int>(i));
}

UringBackend::~UringBackend() {
  for (auto& r : rings_) {
    if (r->initialized) io_uring_queue_exit(&r->ring);  // Also drops registrations.
  }
}

std::string UringBackend::Name() const {
  std::string n = "io_uring(rings=" + std::to_string(rings_.size()) + ",qd=" + std::to_string(queue_depth_);
  n += ",fixed_files=" + (fixed_files_ ? std::to_string(file_table_size_) : std::string("off"));
  if (pool_) {
    n += ",fixed_buffers=" + std::to_string(pool_->iovecs().size()) + "x" +
         std::to_string(pool_->buffer_size() / 1024) + "KiB";
  } else {
    n += ",fixed_buffers=off";
  }
  n += use_rw_ops_ ? "" : ",readv";
  n += sqpoll_ ? ",sqpoll)" : ")";
  return n;
}

void UringBackend::RegisterFile(int fd) {
  if (!fixed_files_ || fd < 0) return;
  std::unique_lock lk(files_mu_);
  if (fd_to_slot_.contains(fd) || free_slots_.empty()) return;
  const int slot = free_slots_.back();
  for (auto& r : rings_) {
    std::lock_guard rl(r->mu);
    int f = fd;
    if (io_uring_register_files_update(&r->ring, static_cast<unsigned>(slot), &f, 1) < 0) {
      // Roll back already-updated rings; the fd simply stays unregistered.
      int neg = -1;
      for (auto& u : rings_) {
        if (u.get() == r.get()) break;
        std::lock_guard ul(u->mu);
        (void)io_uring_register_files_update(&u->ring, static_cast<unsigned>(slot), &neg, 1);
      }
      return;
    }
  }
  free_slots_.pop_back();
  fd_to_slot_.emplace(fd, slot);
}

void UringBackend::UnregisterFile(int fd) {
  if (!fixed_files_) return;
  std::unique_lock lk(files_mu_);
  auto it = fd_to_slot_.find(fd);
  if (it == fd_to_slot_.end()) return;
  const int slot = it->second;
  fd_to_slot_.erase(it);
  for (auto& r : rings_) {
    std::lock_guard rl(r->mu);
    int neg = -1;
    (void)io_uring_register_files_update(&r->ring, static_cast<unsigned>(slot), &neg, 1);
  }
  free_slots_.push_back(slot);
}

int UringBackend::SlotFor(int fd) const {
  if (!fixed_files_) return -1;
  std::shared_lock lk(files_mu_);
  auto it = fd_to_slot_.find(fd);
  return it == fd_to_slot_.end() ? -1 : it->second;
}

AlignedBuffer UringBackend::AllocateBuffer(size_t capacity) {
  if (pool_) {
    if (AlignedBuffer b = pool_->Acquire(capacity, pool_)) return b;
  }
  return AlignedBuffer::Allocate(capacity);
}

UringBackend::Ring& UringBackend::PickRing() noexcept {
  // Each thread has a home ring; Submit() tries the others before blocking.
  return *rings_[tl_thread_index % rings_.size()];
}

Status UringBackend::Submit(std::span<IoRequest> reqs) {
  if (reqs.empty()) return Status::OK();
  const size_t n = rings_.size();
  const size_t home = tl_thread_index % n;
  for (size_t i = 0; i < n; ++i) {
    Ring& r = *rings_[(home + i) % n];
    std::unique_lock lk(r.mu, std::try_to_lock);
    if (lk.owns_lock()) return SubmitOnRing(r, reqs);
  }
  Ring& r = PickRing();
  std::lock_guard lk(r.mu);
  return SubmitOnRing(r, reqs);
}

Status UringBackend::SubmitOnRing(Ring& rg, std::span<IoRequest> reqs) {
  io_uring* ring = &rg.ring;
  auto& st = tl_state;
  auto& pending = tl_pending;
  st.assign(reqs.size(), InFlight{});
  pending.clear();

  Status first_error;
  for (size_t i = reqs.size(); i-- > 0;) {  // Reverse so pop_back() submits in order.
    IoRequest& r = reqs[i];
    Status v = ValidateRequest(r);
    if (v.ok() && r.len > (1u << 30)) v = Status::InvalidArgument("io request larger than 1 GiB");
    if (v.ok() && r.buf_index >= 0 && !pool_) v = Status::InvalidArgument("fixed buffer without pool");
    if (!v.ok()) {
      r.result = -EINVAL;
      st[i].finished = true;
      if (first_error.ok()) first_error = v;
      continue;
    }
    st[i].slot = SlotFor(r.fd);
    pending.push_back(static_cast<uint32_t>(i));
  }

  unsigned inflight = 0;
  int consecutive_submit_errors = 0;
  while (!pending.empty() || inflight > 0) {
    while (!pending.empty()) {
      io_uring_sqe* sqe = io_uring_get_sqe(ring);
      if (sqe == nullptr) break;  // SQ full; submit what we have first.
      const uint32_t idx = pending.back();
      pending.pop_back();
      IoRequest& r = reqs[idx];
      InFlight& s = st[idx];
      char* p = static_cast<char*>(r.buf) + s.done;
      const auto len = static_cast<unsigned>(r.len - s.done);
      const uint64_t off = r.offset + s.done;
      const int target = s.slot >= 0 ? s.slot : r.fd;
      const bool rd = r.op == IoOpcode::kRead;
      if (r.buf_index >= 0) {
        if (rd) io_uring_prep_read_fixed(sqe, target, p, len, off, r.buf_index);
        else io_uring_prep_write_fixed(sqe, target, p, len, off, r.buf_index);
      } else if (use_rw_ops_) {
        if (rd) io_uring_prep_read(sqe, target, p, len, off);
        else io_uring_prep_write(sqe, target, p, len, off);
      } else {
        s.iov = iovec{p, len};
        if (rd) io_uring_prep_readv(sqe, target, &s.iov, 1, off);
        else io_uring_prep_writev(sqe, target, &s.iov, 1, off);
      }
      if (s.slot >= 0) sqe->flags |= IOSQE_FIXED_FILE;
      io_uring_sqe_set_data64(sqe, idx);
      ++inflight;
    }

    const int ret = io_uring_submit_and_wait(ring, 1);
    if (ret < 0) {
      // EINTR: signal while waiting. EAGAIN/EBUSY: kernel out of resources or
      // CQ overflow — reap below and try again. Queued SQEs stay in the SQ
      // ring and are picked up by the next io_uring_enter.
      if (ret != -EINTR && ret != -EAGAIN && ret != -EBUSY) {
        if (++consecutive_submit_errors > 64) {
          // The ring is unusable while the kernel may still own our buffers;
          // returning would hand freed memory to in-flight DMA.
          std::fprintf(stderr, "lsmkv: io_uring_enter failing persistently: %s\n", std::strerror(-ret));
          std::abort();
        }
      }
      if (inflight == 0 && pending.empty()) break;
    } else {
      consecutive_submit_errors = 0;
    }

    io_uring_cqe* cqe;
    unsigned head;
    unsigned seen = 0;
    io_uring_for_each_cqe(ring, head, cqe) {
      ++seen;
      const auto idx = static_cast<uint32_t>(io_uring_cqe_get_data64(cqe));
      const int res = cqe->res;
      --inflight;
      IoRequest& r = reqs[idx];
      InFlight& s = st[idx];
      if (res == -EAGAIN || res == -EINTR) {
        stats_.eagain_retries.fetch_add(1, std::memory_order_relaxed);
        pending.push_back(idx);
        continue;
      }
      if (res < 0) {
        r.result = res;
        s.finished = true;
        continue;
      }
      if (res == 0) {
        r.result = r.op == IoOpcode::kRead ? static_cast<int64_t>(s.done) : -EIO;
        s.finished = true;
        continue;
      }
      s.done += static_cast<size_t>(res);
      if (s.done >= r.len) {
        r.result = static_cast<int64_t>(s.done);
        s.finished = true;
      } else if (!IsAligned(static_cast<uint64_t>(res))) {
        // Short unaligned transfer: EOF for reads, out of space for writes.
        r.result = r.op == IoOpcode::kRead ? static_cast<int64_t>(s.done) : -ENOSPC;
        s.finished = true;
      } else {
        pending.push_back(idx);  // Short but aligned: continue where it stopped.
      }
    }
    io_uring_cq_advance(ring, seen);
  }

  for (size_t i = 0; i < reqs.size(); ++i) {
    const IoRequest& r = reqs[i];
    if (r.result < 0) {
      if (first_error.ok()) {
        first_error = Status::IOError(r.op == IoOpcode::kRead ? "io_uring read" : "io_uring write",
                                      static_cast<int>(-r.result));
      }
      continue;
    }
    if (r.op == IoOpcode::kRead) {
      stats_.read_ops.fetch_add(1, std::memory_order_relaxed);
      stats_.bytes_read.fetch_add(static_cast<uint64_t>(r.result), std::memory_order_relaxed);
    } else {
      stats_.write_ops.fetch_add(1, std::memory_order_relaxed);
      stats_.bytes_written.fetch_add(static_cast<uint64_t>(r.result), std::memory_order_relaxed);
    }
  }
  return first_error;
}

std::shared_ptr<IoBackend> NewUringBackend(const Options& options, Status* status) {
  return UringBackend::Create(options, status);
}

}  // namespace lsm

#else  // !LSMKV_HAVE_URING

#include "lsm/iobackend.hpp"

namespace lsm {

std::shared_ptr<IoBackend> NewUringBackend(const Options& /*options*/, Status* status) {
  *status = Status::NotSupported("lsmkv was built without liburing");
  return nullptr;
}

}  // namespace lsm

#endif  // LSMKV_HAVE_URING
