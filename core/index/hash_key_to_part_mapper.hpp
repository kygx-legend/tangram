#pragma once

#include "core/index/abstract_key_to_part_mapper.hpp"

#include <functional>

namespace xyz {

template <typename KeyT>
class HashKeyToPartMapper : public TypedKeyToPartMapper<KeyT> {
 public:
  HashKeyToPartMapper(size_t num_partition):TypedKeyToPartMapper<KeyT>(num_partition) {}

  virtual size_t Get(const KeyT& key) const override {
    return std::hash<KeyT>()(key) % this->GetNumPart();
  }
};


template <typename KeyT>
class RoundRobinKeyToPartMapper: public TypedKeyToPartMapper<KeyT> {
 public:
  RoundRobinKeyToPartMapper(size_t num_partition):TypedKeyToPartMapper<KeyT>(num_partition) {}

  virtual size_t Get(const KeyT& key) const override {
    return key % this->GetNumPart();
  }
};

}  // namespace xyz
