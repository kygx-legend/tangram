#pragma once
#include "base/sarray_binstream.hpp"

namespace xyz {

struct PlanSpec {
  int plan_id;
  int map_collection_id;
  int join_collection_id;

  int with_collection_id;

  int map_partition_num;
  int join_partition_num;
  int with_partition_num;

  PlanSpec() = default;
  PlanSpec(int pid, int mid, int jid, int wid)
      : plan_id(pid), map_collection_id(mid), join_collection_id(jid), with_collection_id(wid)
  {}

  friend SArrayBinStream& operator<<(xyz::SArrayBinStream& stream, const PlanSpec& p) {
    stream << p;
  	return stream;
  }
  
  friend SArrayBinStream& operator>>(xyz::SArrayBinStream& stream, PlanSpec& p) {
    stream >> p;
  	return stream;
  }
};

}  // namespace xyz