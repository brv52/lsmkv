#include "memtable/memtable.hpp"

#include <cstring>

namespace lsm {

int MemTable::KeyComparator::operator()(const char* a, const char* b) const noexcept {
  return InternalKeyComparator::Compare(GetLengthPrefixed(a), GetLengthPrefixed(b));
}

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
  // varint32(ikey len) | user key | tag | varint32(value len) | value
  const size_t ikey_len = key.size() + 8;
  const size_t encoded_len = static_cast<size_t>(VarintLength(ikey_len)) + ikey_len +
                             static_cast<size_t>(VarintLength(value.size())) + value.size();
  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(ikey_len));
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  EncodeFixed64(p, PackTag(seq, type));
  p += 8;
  p = EncodeVarint32(p, static_cast<uint32_t>(value.size()));
  std::memcpy(p, value.data(), value.size());
  table_.Insert(buf);
  entries_.fetch_add(1, std::memory_order_relaxed);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) const {
  Table::Iterator it(&table_);
  it.Seek(key.memtable_key().data());
  if (!it.Valid()) return false;
  const char* entry = it.key();
  const Slice ikey = GetLengthPrefixed(entry);
  if (ExtractUserKey(ikey) != key.user_key()) return false;
  switch (ExtractValueType(ikey)) {
    case ValueType::kValue: {
      const Slice v = GetLengthPrefixed(ikey.data() + ikey.size());
      value->assign(v.data(), v.size());
      *s = Status::OK();
      return true;
    }
    case ValueType::kDeletion:
      *s = Status::NotFound();
      return true;
  }
  return false;
}

class MemTableIterator final : public Iterator {
 public:
  explicit MemTableIterator(const MemTable::Table* t) : it_(t) {}

  [[nodiscard]] bool Valid() const override { return it_.Valid(); }
  void SeekToFirst() override { it_.SeekToFirst(); }
  void Seek(const Slice& target) override {
    // The skiplist compares length-prefixed keys.
    tmp_.clear();
    PutVarint32(&tmp_, static_cast<uint32_t>(target.size()));
    tmp_.append(target.data(), target.size());
    it_.Seek(tmp_.data());
  }
  void Next() override { it_.Next(); }
  [[nodiscard]] Slice key() const override { return GetLengthPrefixed(it_.key()); }
  [[nodiscard]] Slice value() const override {
    const Slice k = GetLengthPrefixed(it_.key());
    return GetLengthPrefixed(k.data() + k.size());
  }
  [[nodiscard]] Status status() const override { return Status::OK(); }

 private:
  MemTable::Table::Iterator it_;
  std::string tmp_;
};

std::unique_ptr<Iterator> MemTable::NewIterator() const { return std::make_unique<MemTableIterator>(&table_); }

}  // namespace lsm
