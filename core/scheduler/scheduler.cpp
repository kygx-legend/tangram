#include <algorithm>

#include "comm/simple_sender.hpp"
#include "core/queue_node_map.hpp"
#include "core/scheduler/scheduler.hpp"

#include "base/color.hpp"

namespace xyz {

// make the scheduler ready and start receiving RegisterProgram
void Scheduler::Ready(std::vector<Node> nodes) {
  start = std::chrono::system_clock::now();
  LOG(INFO) << "[Scheduler] Ready";
  for (auto& node : nodes) {
    CHECK(elem_->nodes.find(node.id) == elem_->nodes.end());
    NodeInfo n;
    n.node = node;
    elem_->nodes[node.id] = n;
  }
  Start();
  start_ = true;
}

void Scheduler::Process(Message msg) {
  CHECK_EQ(msg.data.size(), 2); // cmd, content
  int node_id = GetNodeId(msg.meta.sender);
  SArrayBinStream ctrl_bin, bin;
  ctrl_bin.FromSArray(msg.data[0]);
  bin.FromSArray(msg.data[1]);
  ScheduleFlag flag;
  ctrl_bin >> flag;
  switch (flag) {
  case ScheduleFlag::kRegisterProgram: {
    RegisterProgram(node_id, bin);
    break;
  }
  case ScheduleFlag::kUpdateCollectionReply: {
    collection_manager_->FinishUpdate(bin);
    break;
  }
  case ScheduleFlag::kFinishBlock: {
    block_manager_->FinishBlock(bin);
    break;
  }
  case ScheduleFlag::kFinishDistribute: {
    distribute_manager_->FinishDistribute(bin);
    break;
  }
  case ScheduleFlag::kFinishCheckpoint: {
    checkpoint_manager_->FinishCheckpoint(bin);
    break;
  }
  case ScheduleFlag::kFinishLoadCheckpoint: {
    checkpoint_loader_->FinishLoadCheckpoint(bin);
    break;
  }
  case ScheduleFlag::kFinishWritePartition: {
    write_manager_->FinishWritePartition(bin);
    break;
  }
  case ScheduleFlag::kControl: {
    control_manager_->Control(bin);
    break;
  }
  case ScheduleFlag::kFinishPlan: {
    int plan_id;
    bin >> plan_id;
    LOG(INFO) << "[Scheduler] " << YELLOW("Finish plan " + std::to_string(plan_id));
    if (IsRecoverPlan(plan_id)) {
      plan_id = GetRealId(plan_id);
      LOG(INFO) << "[Scheduler] " << RED("Finish a recovery plan");
    }
    dag_runner_->Finish(plan_id);
    collection_status_->FinishPlan(plan_id);
    // LOG(INFO) << collection_status_->DebugString();
    TryRunPlan();
    break;
  }
  case ScheduleFlag::kRecovery: {
    Recovery(bin);
    break;
  }
  default:
    CHECK(false) << ScheduleFlagName[static_cast<int>(flag)];
  }
}

void Scheduler::RegisterProgram(int node_id, SArrayBinStream bin) {
  WorkerInfo info;
  bin >> info;
  CHECK(elem_->nodes.find(node_id) != elem_->nodes.end());
  elem_->nodes[node_id].num_local_threads = info.num_local_threads;
  if (!init_program_) {
    init_program_ = true;
    bin >> program_;
    LOG(INFO) << "set dag_runner: " << dag_runner_type_;
    if (dag_runner_type_ == "sequential") {
      dag_runner_.reset(new SequentialDagRunner(program_.dag));
    } else if (dag_runner_type_ == "wide") {
      dag_runner_.reset(new WideDagRunner(program_.dag));
    } else {
      CHECK(false);
    }
    LOG(INFO) << "[Scheduler] Receive program: " << program_.DebugString();
  }
  register_program_count_ += 1;
  if (register_program_count_ == elem_->nodes.size()) {
    LOG(INFO)
        << "[Scheduler] all workers registerred, start the scheduling thread";
    TryRunPlan();
  }
}

void Scheduler::TryRunPlan() {
  if (dag_runner_->GetNumRemainingPlans() == 0) {
    Exit();
  } else {
    auto plans = dag_runner_->GetRunnablePlans();
    for (auto plan_id : plans) {
      RunPlan(plan_id);
    }
  }
}

void Scheduler::RunPlan(int plan_id) {
  CHECK_LT(plan_id, program_.specs.size());
  auto spec = program_.specs[plan_id];
  LOG(INFO) << "[Scheduler] " << YELLOW("Running plan "+std::to_string(spec.id)+" "+spec.name+" ") << spec.DebugString();
  auto rw = spec.GetReadWrite();
  collection_status_->AddPlan(plan_id, rw);
  if (spec.type == SpecWrapper::Type::kDistribute) {
    LOG(INFO) << "[Scheduler] Distributing: " << spec.DebugString();
    distribute_manager_->Distribute(spec);
  } else if (spec.type == SpecWrapper::Type::kLoad) {
    LOG(INFO) << "[Scheduler] Loading: " << spec.DebugString();
    block_manager_->Load(spec);
  } else if (spec.type == SpecWrapper::Type::kMapJoin
          || spec.type == SpecWrapper::Type::kMapWithJoin) {
    int id = spec.id;
    control_manager_->RunPlan(spec, [this, id]() {
      SArrayBinStream reply_bin;
      reply_bin << id;
      ToScheduler(elem_, ScheduleFlag::kFinishPlan, reply_bin);
    });
  } else if (spec.type == SpecWrapper::Type::kWrite) {
    LOG(INFO) << "[Scheduler] Writing: " << spec.DebugString();
    write_manager_->Write(spec);
  } else if (spec.type == SpecWrapper::Type::kCheckpoint) {
    LOG(INFO) << "[Scheduler] Checkpointing: " << spec.DebugString();
    checkpoint_manager_->Checkpoint(spec);
  } else if (spec.type == SpecWrapper::Type::kLoadCheckpoint) {
    LOG(INFO) << "[Scheduler] Loading checkpoint: " << spec.DebugString();
    checkpoint_manager_->LoadCheckpoint(spec);
  } else {
    CHECK(false) << spec.DebugString();
  }
}

void Scheduler::FinishRecovery() {
  LOG(INFO) << "[Scheduler] FinishRecovery";
  auto cur_plans = collection_status_->GetCurrentPlans();
  CHECK_EQ(cur_plans.size(), 1);
  for (auto pid: cur_plans) {
      LOG(INFO) << "[FinishRecovery] pid: " << pid;
    collection_status_->FinishPlan(pid);
    CHECK_LT(pid, program_.specs.size());
    auto& spec = program_.specs[pid];
    // TODO: now I assert it must be mj or mwj
    CHECK(spec.type == SpecWrapper::Type::kMapJoin
       || spec.type == SpecWrapper::Type::kMapWithJoin);

    auto* mapupdate_spec = program_.specs[pid].GetMapJoinSpec();
    int cur_version = control_manager_->GetCurVersion(pid);
    LOG(INFO) << mapupdate_spec->DebugString();
    CHECK(mapupdate_spec->checkpoint_interval);
    int new_iter = mapupdate_spec->num_iter - 
        (cur_version / mapupdate_spec->checkpoint_interval * mapupdate_spec->checkpoint_interval);
    // directly update the version
    mapupdate_spec->num_iter = new_iter;

    spec.id = GetRecoverPlanId(pid);  // setting a new id
    LOG(INFO) << RED("rerunning plan for recovery: " + std::to_string(spec.id) 
            + " remaining iters: " + std::to_string(new_iter));
    // relaunch the plan
    RunPlan(pid);
  }
}

bool Scheduler::LostWriteCollection(const std::set<int>& dead_nodes) {
  auto writes = collection_status_->GetWrites();
  for (auto w : writes) {
    const auto& collection_view = elem_->collection_map->Get(w);
    const auto& part_to_node = collection_view.mapper.Get();
    for (int i = 0; i < part_to_node.size(); ++ i) {
      int node_id = part_to_node[i];
      if (dead_nodes.find(node_id) != dead_nodes.end()) {
        // the node_id is in deadnodes
        return true;
      }
    }
  }
  return false;
}

void Scheduler::Recovery(SArrayBinStream bin) {
  // remove dead_nodes
  std::set<int> dead_nodes;
  bin >> dead_nodes;
  for (auto node : dead_nodes)
    elem_->nodes.erase(node);

  // terminate plan
  auto cur_plans = collection_status_->GetCurrentPlans();
  CHECK_EQ(cur_plans.size(), 1);  // TODO

  int plan_id = cur_plans[0];
  auto spec_wrapper = program_.specs[plan_id];
  CHECK(spec_wrapper.type == SpecWrapper::Type::kMapJoin
       || spec_wrapper.type == SpecWrapper::Type::kMapWithJoin);
  
  if (LostWriteCollection(dead_nodes)) {
    LOG(INFO) << RED("Some write partitions lost, aborting plan");
    control_manager_->AbortPlan(plan_id, [this, dead_nodes]() {
      // TODO: only work for 1 running plan
      // To support more plan, use a counter to record the aborted plans.
      // recover the collections and update collection map
      auto reads = collection_status_->GetReadsAndCP();
      auto writes = collection_status_->GetWritesAndCP();
      recover_manager_->Recover(dead_nodes, writes, reads, [this]() {
        FinishRecovery();
      });
    });
  } else {
    LOG(INFO) << RED("No write partition lost.");
    auto reads = collection_status_->GetReadsAndCP();
    recover_manager_->Recover(dead_nodes, {}, reads, [this, plan_id, reads]() {
      CHECK_EQ(reads.size(), 1) << "only support 1 map collection";
      control_manager_->ReassignMap(plan_id, reads.begin()->first);
    });
  }
}

void Scheduler::Exit() {
  end = std::chrono::system_clock::now();
  std::chrono::duration<double> duration = end - start;
  LOG(INFO) << "[Scheduler] Exit. Runtime: " << duration.count();
  SArrayBinStream dummy_bin;
  SendToAllWorkers(elem_, ScheduleFlag::kExit, dummy_bin);
  exit_promise_.set_value();
}

void Scheduler::Wait() {
  LOG(INFO) << "[Scheduler] waiting";
  std::future<void> f = exit_promise_.get_future();
  f.get();
}

void Scheduler::RunDummy() {
  SArrayBinStream bin;
  SendToAllWorkers(elem_, ScheduleFlag::kDummy, bin);
}

} // namespace xyz
