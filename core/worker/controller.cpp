#include "core/worker/controller.hpp"

#include "core/scheduler/control.hpp"

#include "core/worker/plan_controller.hpp"

#include "base/color.hpp"

namespace xyz {

void Controller::Process(Message msg) {
  // cmd, plan_id, content
  SArrayBinStream ctrl_bin, plan_bin, bin;
  ctrl_bin.FromSArray(msg.data[0]);
  plan_bin.FromSArray(msg.data[1]);
  bin.FromSArray(msg.data[2]);
  ControllerFlag flag;
  ctrl_bin >> flag;
  int plan_id;
  plan_bin >> plan_id;

  // LOG(INFO) << "flag: " << static_cast<int>(flag);
  if (flag != ControllerFlag::kSetup) {
    // CHECK(plan_controllers_.find(plan_id) != plan_controllers_.end());
    if (plan_controllers_.find(plan_id) == plan_controllers_.end()) {
      LOG(INFO) << RED("[Controller::Process] Ignoring message ControllerFlag: " + 
          ControllerFlagName[static_cast<int>(flag)] +
          " for plan: " + std::to_string(plan_id))
        << " meta: " << msg.meta.DebugString();
      return;
    }
  }

  if (flag != ControllerFlag::kSetup) {
    if (plan_timer_.find(plan_id) == plan_timer_.end()) {
      plan_timer_[plan_id].start_time = std::chrono::steady_clock::now();
    }
  }
  auto start_time = std::chrono::steady_clock::now();

  switch (flag) {
  case ControllerFlag::kSetup: {
    Setup(bin);
    break;
  }
  case ControllerFlag::kTerminatePlan: {
    TerminatePlan(plan_id);
    break;  
  }
  case ControllerFlag::kStart: {
    plan_controllers_[plan_id]->StartPlan();
    break;
  }
  case ControllerFlag::kFinishMap: {
    plan_controllers_[plan_id]->FinishMap(bin);
    break;
  }
  case ControllerFlag::kFinishJoin: {
    plan_controllers_[plan_id]->FinishJoin(bin);
    break;
  }
  case ControllerFlag::kUpdateVersion: {
    plan_controllers_[plan_id]->UpdateVersion(bin);
    break;
  }
  case ControllerFlag::kReceiveJoin: {
    plan_controllers_[plan_id]->ReceiveJoin(msg);
    break;
  }
  case ControllerFlag::kFetchRequest: {
    plan_controllers_[plan_id]->ReceiveFetchRequest(msg);
    break;
  }
  case ControllerFlag::kFinishFetch: {
    plan_controllers_[plan_id]->FinishFetch(bin);
    break;  
  }
  case ControllerFlag::kFinishCheckpoint: {
    plan_controllers_[plan_id]->FinishCheckpoint(bin);
    break;  
  }
  case ControllerFlag::kMigratePartition: {
    plan_controllers_[plan_id]->MigratePartition(msg);
    break;  
  }
  case ControllerFlag::kFinishLoadWith: {
    plan_controllers_[plan_id]->FinishLoadWith(bin);
    break;                                      
  }
  case ControllerFlag::kReassignMap: {
    plan_controllers_[plan_id]->ReassignMap(bin);
    break;                                      
  }
  default: CHECK(false);
  }

  auto end_time = std::chrono::steady_clock::now();
  if (flag != ControllerFlag::kSetup) {
    CHECK(plan_timer_.find(plan_id) != plan_timer_.end());
    plan_timer_[plan_id].control_time += std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    if (flag == ControllerFlag::kTerminatePlan) {
      plan_timer_[plan_id].plan_time += std::chrono::duration_cast<std::chrono::microseconds>(end_time - plan_timer_[plan_id].start_time);
      LOG(INFO) << "[Controller] plan " << plan_id 
        << " plan time(ms): " << plan_timer_[plan_id].plan_time.count()/1000
        << " control time(ms): " << plan_timer_[plan_id].control_time.count()/1000;
    }
  }
}

void Controller::Setup(SArrayBinStream bin) {
  SpecWrapper spec;
  bin >> spec;

  int plan_id = spec.id;
  auto plan_controller = std::make_shared<PlanController>(this);
  plan_controllers_.insert({plan_id, plan_controller});
  plan_controllers_[plan_id]->Setup(spec);
}

// terminate a plan
// TODO: there may be some tasks still running or pending in the executors.
// For map (without fetch), update and fetch, should be ok. 
// The only possible remaining task is map (with fetch).
void Controller::TerminatePlan(int plan_id) {
  CHECK(plan_controllers_.find(plan_id) != plan_controllers_.end());
  // CANNOT erase if BGCPing
  boost::unique_lock<boost::shared_mutex> lk(erase_mu_);
  erased[plan_id] = true;
  LOG(INFO) << "[Controller] Terminating plan " << plan_id << " on node: " << engine_elem_.node.id;
  // plan_controllers_[plan_id]->DisplayTime();
  plan_controllers_.erase(plan_id);
  LOG(INFO) << "[Controller] Done terminating plan " << plan_id << " on node: " << engine_elem_.node.id;
  /*
  while (engine_elem_.executor->GetNumPendingTask()) {
    LOG(INFO) << "[Controller] sleep, executor size: " << engine_elem_.executor->GetNumPendingTask();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  */
  // LOG(INFO) << "[Controller] executor size: " << engine_elem_.executor->GetNumPendingTask();

  SArrayBinStream bin;
  ControllerMsg ctrl;
  ctrl.flag = ControllerMsg::Flag::kFinish;
  ctrl.version = -1;
  ctrl.node_id = engine_elem_.node.id;
  ctrl.plan_id = plan_id;
  bin << ctrl;
  SendMsgToScheduler(bin);
}

void Controller::SendMsgToScheduler(SArrayBinStream bin) {
  Message msg;
  msg.meta.sender = Qid();
  msg.meta.recver = 0;
  msg.meta.flag = Flag::kOthers;
  SArrayBinStream ctrl_bin;
  ctrl_bin << ScheduleFlag::kControl;
  msg.AddData(ctrl_bin.ToSArray());
  msg.AddData(bin.ToSArray());
  engine_elem_.sender->Send(std::move(msg));
}


}  // namespace xyz

