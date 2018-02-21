#pragma once

#include <sstream>

#include "base/sarray_binstream.hpp"

namespace xyz {

struct AssignedBlock {
  std::string url;
  size_t offset;
  int id;
  int collection_id;

  std::string DebugString() const {
    std::stringstream ss;
    ss << "url: " << url;
    ss << ", offset: " << offset;
    ss << ", id: " << id;
    ss << ", collection_id: " << collection_id;
    return ss.str();
  }

  friend SArrayBinStream& operator<<(xyz::SArrayBinStream& stream, const AssignedBlock& b) {
    stream << b.url << b.offset << b.id << b.collection_id;
    return stream;
  }
  friend SArrayBinStream& operator>>(xyz::SArrayBinStream& stream, AssignedBlock& b) {
    stream >> b.url >> b.offset >> b.id >> b.collection_id;
    return stream;
  }
};

struct FinishedBlock {
  int block_id;
  int node_id;
  int qid;
  std::string hostname;

  std::string DebugString() const {
    std::stringstream ss;
    ss << "block_id: " << block_id;
    ss << ", node_id: " << node_id;
    ss << ", qid: " << qid;
    ss << ", hostname: " << hostname;
    return ss.str();
  }

  friend SArrayBinStream& operator<<(xyz::SArrayBinStream& stream, const FinishedBlock& b) {
    stream << b.block_id << b.node_id << b.qid << b.hostname;
    return stream;
  }
  friend SArrayBinStream& operator>>(xyz::SArrayBinStream& stream, FinishedBlock& b) {
    stream >> b.block_id >> b.node_id >> b.qid >> b.hostname;
    return stream;
  }
};

}  // namespace xyz
