#pragma once

#include "core/scheduler/scheduler_elem.hpp"

#include "core/plan/spec_wrapper.hpp"

namespace xyz {

class DistributeManager {
 public:
  DistributeManager(std::shared_ptr<SchedulerElem> elem)
      : elem_(elem) {}
  void Distribute(SpecWrapper spec);
  void FinishDistribute(SArrayBinStream bin);

 private:
  std::map<int, int> part_expected_map_;
  std::shared_ptr<SchedulerElem> elem_;
  // plan_id, collection_id, part_id, node_id
  std::map<int, std::map<int, std::map<int, int>>> distribute_map_;

};

} // namespace xyz
