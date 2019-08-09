#include "oneflow/core/actor/op_actor_context.h"
#include "oneflow/core/actor/regst_handler.h"

namespace oneflow {

namespace actor {

OpActorCtx::OpActorCtx(MsgHandler msg_handler, const std::vector<RegstHandlerIf*>& regst_handlers,
                       const std::shared_ptr<void>& other_ptr) {
  initial_msg_handler_ = msg_handler;
  for (RegstHandlerIf* regst_handler : regst_handlers) {
    handlers_.emplace(regst_handler->type(), std::unique_ptr<RegstHandlerIf>(regst_handler));
  }
  kernel_ctx_->other = other_ptr;
}

void OpActorCtx::InitDeviceCtx(const ThreadCtx& thread_ctx) {
  int64_t local_work_stream_id = Global<IDMgr>::Get()->LocalWorkStreamId4ActorId(actor_id_);
  switch (GetDeviceType()) {
    case DeviceType::kCPU: {
      CHECK_EQ(local_work_stream_id, 0);
      device_ctx_.reset(new CpuDeviceCtx());
      break;
    }
    case DeviceType::kGPU: {
      CudaStreamHandle* cuda_handle = nullptr;
      CHECK_EQ(local_work_stream_id, 0);
      cuda_handle = thread_ctx.g_cuda_stream.get();
      device_ctx_.reset(new CudaDeviceCtx(cuda_handle));
      break;
    }
    default: { UNIMPLEMENTED(); }
  }
}

DeviceType OpActorCtx::GetDeviceType() const {
  return Global<IDMgr>::Get()->GetDeviceTypeFromActorId(actor_id_);
}

void OpActorCtx::Init(const TaskProto& task_proto, const ThreadCtx& thread_ctx) {
  actor_id_ = task_proto.task_id();
  act_id_ = -1;
  InitDeviceCtx(thread_ctx);
  kernel_ctx_->device_ctx = device_ctx_.get();
  if (task_proto.has_parallel_ctx()) {
    parallel_ctx_.reset(new ParallelContext(task_proto.parallel_ctx()));
  }
  for (const ExecNodeProto& node : task_proto.exec_sequence().exec_node()) {
    ExecKernel ek;
    ek.kernel = ConstructKernel(parallel_ctx(), node.kernel_conf(), device_ctx_.get());
    ek.bn_in_op2regst_desc_id = PbMap2HashMap(node.bn_in_op2regst_desc_id());
    exec_kernel_vec_.push_back(std::move(ek));
  }

  for (const auto& pair : task_proto.produced_regst_desc()) {
    Global<RegstMgr>::Get()->NewRegsts(pair.second, [this](Regst* regst) {
      produced_regsts_[regst->regst_desc_id()].emplace_back(regst);
    });
    int64_t regst_desc_id = pair.second.regst_desc_id();
    CHECK(name2regst_desc_id_.insert({pair.first, {regst_desc_id}}).second);
  }
  for (const auto& pair : task_proto.consumed_regst_desc_id()) {
    CHECK(name2regst_desc_id_.find(pair.first) == name2regst_desc_id_.end());
    std::vector<int64_t>& regst_desc_id_vec = name2regst_desc_id_[pair.first];
    for (int64_t regst_desc_id : pair.second.regst_desc_id()) {
      regst_desc_id_vec.push_back(regst_desc_id);
    }
  }
}

void OpActorCtx::InitRegstHandlers(const TaskProto& task_proto) {
  HashSet<int64_t> consumed_ids;
  HashSet<int64_t> produced_ids;
  auto ProcessRegstDescId = [&](const RegstHandlerProto& handler_proto, bool is_consumed) {
    const RegstDescIdSet& ids = is_consumed ? handler_proto.consumed_regst_desc_ids()
                                            : handler_proto.produced_regst_desc_ids();
    HashSet<int64_t>& id_set = is_consumed ? consumed_ids : produced_ids;
    for (int64_t id : ids.regst_desc_id()) {
      regst_desc_id2handler_.emplace(id, handlers_.at(handler_proto.type()).get());
      CHECK(id_set.insert(id).second);
    }
  };

  for (const RegstHandlerProto& handler_proto : task_proto.regst_handlers()) {
    const std::string& type = handler_proto.type();
    CHECK(IsKeyFound(handlers_, type))
        << "OpActorCtx does not register RegstHandler with type = " << type;
    handlers_.at(type)->Init(handler_proto, produced_regsts_,
                             new MsgDeliveryCtx(actor_id_, device_ctx_.get()));

    ProcessRegstDescId(handler_proto, true);
    ProcessRegstDescId(handler_proto, false);
  }

  // make sure task_proto is self-consistent
  for (const auto& pair : task_proto.produced_regst_desc()) {
    CHECK(IsKeyFound(produced_ids, pair.second.regst_desc_id()));
  }
  CHECK_EQ(task_proto.produced_regst_desc().size(), produced_ids.size());

  size_t consumed_cnt = 0;
  for (const auto& pair : task_proto.consumed_regst_desc_id()) {
    consumed_cnt += pair.second.regst_desc_id().size();
    for (int regst_desc_id : pair.second.regst_desc_id()) {
      CHECK(IsKeyFound(consumed_ids, regst_desc_id));
    }
  }
  CHECK_EQ(consumed_cnt, consumed_ids.size());
}

void OpActorCtx::UpdateWithRegstMsg(const ActorMsg& msg) {
  regst_desc_id2handler_.at(msg.regst()->regst_desc_id())->UpdateWithRegstMsg(msg);
}

void OpActorCtx::UpdateWithProducedRegstMsg(const ActorMsg& msg) {
  CHECK(IsKeyFound(produced_regsts_, msg.regst()->regst_desc_id()));
  UpdateWithRegstMsg(msg);
}

void OpActorCtx::UpdateWithEordMsg(const ActorMsg& msg) {
  regst_desc_id2handler_.at(msg.regst()->regst_desc_id())->UpdateWithEordMsg(msg);
}

void OpActorCtx::UpdateWithCmdMsg(const ActorMsg& msg) {
  CHECK_EQ(msg.actor_cmd(), ActorCmd::kStart);
}

bool OpActorCtx::IsReady() const {
  for (const auto& pair : handlers_) {
    if (pair.second->IsReady() == false) { return false; }
  }
  return true;
}

void OpActorCtx::Act() {
  for (const ExecKernel& ek : exec_kernel_vec_) {
    // TODO(niuchong): BnInOp2Blob return nullptr or failed?
    ek.kernel->Launch(*kernel_ctx_, [&](const std::string& bn_in_op) -> Blob* {
      int64_t regst_desc_id = ek.bn_in_op2regst_desc_id.at(bn_in_op);
      RegstHandlerIf* handler = regst_desc_id2handler_.at(regst_desc_id);
      Regst* regst = handler->GetRegstByRegstDescId(regst_desc_id);
      const LogicalBlobId& lbi = ek.kernel->BnInOp2Lbi(bn_in_op);
      return regst->GetBlobByLbi(lbi);
    });
  }
}

void OpActorCtx::HandleRegstMsgAfterAct() {
  for (auto& pair : handlers_) { pair.second->HandleRegstMsgAfterAct(); }
}

bool OpActorCtx::NoLongerConsumeRegst() const {
  for (const auto& pair : handlers_) {
    if (pair.second->NoLongerConsumeRegst() == false) { return false; }
  }
  return true;
}

MsgHandler OpActorCtx::initial_msg_handler() const { return initial_msg_handler_; }

bool OpActorCtx::NoLongerConsumedByOthers() const {
  for (const auto& pair : handlers_) {
    if (pair.second->NoLongerConsumedByOthers() == false) { return false; }
  }
  return true;
}

OpActorCtx* CreateOpActorCtx(const TaskType&) {
  UNIMPLEMENTED();
  return nullptr;
}

}  // namespace actor

}  // namespace oneflow
