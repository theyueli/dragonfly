#include "server/multi_command_squasher.h"

#include <absl/container/inlined_vector.h>

#include "facade/dragonfly_connection.h"
#include "server/cluster/unique_slot_checker.h"
#include "server/command_registry.h"
#include "server/conn_context.h"
#include "server/engine_shard_set.h"
#include "server/transaction.h"

namespace dfly {

using namespace std;
using namespace facade;
using namespace util;

namespace {

template <typename F> void IterateKeys(CmdArgList args, KeyIndex keys, F&& f) {
  for (unsigned i = keys.start; i < keys.end; i += keys.step)
    f(args[i]);

  if (keys.bonus)
    f(args[*keys.bonus]);
}

void CheckConnStateClean(const ConnectionState& state) {
  DCHECK_EQ(state.exec_info.state, ConnectionState::ExecInfo::EXEC_INACTIVE);
  DCHECK(state.exec_info.body.empty());
  DCHECK(!state.script_info);
  DCHECK(!state.subscribe_info);
}

}  // namespace

MultiCommandSquasher::MultiCommandSquasher(absl::Span<StoredCmd> cmds, ConnectionContext* cntx,
                                           Service* service, bool verify_commands, bool error_abort)
    : cmds_{cmds},
      cntx_{cntx},
      service_{service},
      base_cid_{nullptr},
      verify_commands_{verify_commands},
      error_abort_{error_abort} {
  auto mode = cntx->transaction->GetMultiMode();
  base_cid_ = cntx->transaction->GetCId();
  atomic_ = mode != Transaction::NON_ATOMIC;
}

MultiCommandSquasher::ShardExecInfo& MultiCommandSquasher::PrepareShardInfo(
    ShardId sid, optional<SlotId> slot_id) {
  if (sharded_.empty())
    sharded_.resize(shard_set->size());

  auto& sinfo = sharded_[sid];
  if (!sinfo.local_tx) {
    if (IsAtomic()) {
      sinfo.local_tx = new Transaction{cntx_->transaction, sid, slot_id};
    } else {
      // Non-atomic squashing does not use the transactional framework for fan out, so local
      // transactions have to be fully standalone, check locks and release them immediately.
      sinfo.local_tx = new Transaction{base_cid_};
      sinfo.local_tx->StartMultiNonAtomic();
    }
    num_shards_++;
  }

  return sinfo;
}

MultiCommandSquasher::SquashResult MultiCommandSquasher::TrySquash(StoredCmd* cmd) {
  DCHECK(cmd->Cid());

  if (!cmd->Cid()->IsTransactional() || (cmd->Cid()->opt_mask() & CO::BLOCKING) ||
      (cmd->Cid()->opt_mask() & CO::GLOBAL_TRANS))
    return SquashResult::NOT_SQUASHED;

  cmd->Fill(&tmp_keylist_);
  auto args = absl::MakeSpan(tmp_keylist_);

  auto keys = DetermineKeys(cmd->Cid(), args);
  if (!keys.ok())
    return SquashResult::ERROR;

  // Check if all commands belong to one shard
  bool found_more = false;
  UniqueSlotChecker slot_checker;
  ShardId last_sid = kInvalidSid;
  IterateKeys(args, *keys, [&last_sid, &found_more, &slot_checker](MutableSlice key) {
    if (found_more)
      return;

    string_view key_sv = facade::ToSV(key);

    slot_checker.Add(key_sv);

    ShardId sid = Shard(key_sv, shard_set->size());
    if (last_sid == kInvalidSid || last_sid == sid) {
      last_sid = sid;
      return;
    }
    found_more = true;
  });

  if (found_more || last_sid == kInvalidSid)
    return SquashResult::NOT_SQUASHED;

  auto& sinfo = PrepareShardInfo(last_sid, slot_checker.GetUniqueSlotId());

  sinfo.had_writes |= cmd->Cid()->IsWriteOnly();
  sinfo.cmds.push_back(cmd);
  order_.push_back(last_sid);

  num_squashed_++;

  // Because the squashed hop is currently blocking, we cannot add more than the max channel size,
  // otherwise a deadlock occurs.
  bool need_flush = sinfo.cmds.size() >= kMaxSquashing - 1;
  return need_flush ? SquashResult::SQUASHED_FULL : SquashResult::SQUASHED;
}

bool MultiCommandSquasher::ExecuteStandalone(StoredCmd* cmd) {
  DCHECK(order_.empty());  // check no squashed chain is interrupted

  cmd->Fill(&tmp_keylist_);
  auto args = absl::MakeSpan(tmp_keylist_);

  if (verify_commands_) {
    if (auto err = service_->VerifyCommandState(cmd->Cid(), args, *cntx_); err) {
      cntx_->SendError(std::move(*err));
      return !error_abort_;
    }
  }

  auto* tx = cntx_->transaction;
  tx->MultiSwitchCmd(cmd->Cid());
  cntx_->cid = cmd->Cid();

  if (cmd->Cid()->IsTransactional())
    tx->InitByArgs(cntx_->conn_state.db_index, args);
  service_->InvokeCmd(cmd->Cid(), args, cntx_);

  return true;
}

OpStatus MultiCommandSquasher::SquashedHopCb(Transaction* parent_tx, EngineShard* es) {
  auto& sinfo = sharded_[es->shard_id()];
  DCHECK(!sinfo.cmds.empty());

  auto* local_tx = sinfo.local_tx.get();
  facade::CapturingReplyBuilder crb;
  ConnectionContext local_cntx{cntx_, local_tx, &crb};
  if (cntx_->conn()) {
    local_cntx.skip_acl_validation = cntx_->conn()->IsPrivileged();
  }
  absl::InlinedVector<MutableSlice, 4> arg_vec;

  for (auto* cmd : sinfo.cmds) {
    arg_vec.resize(cmd->NumArgs());
    auto args = absl::MakeSpan(arg_vec);
    cmd->Fill(args);

    if (verify_commands_) {
      // The shared context is used for state verification, the local one is only for replies
      if (auto err = service_->VerifyCommandState(cmd->Cid(), args, *cntx_); err) {
        crb.SendError(std::move(*err));
        sinfo.replies.emplace_back(crb.Take());
        continue;
      }
    }

    local_tx->MultiSwitchCmd(cmd->Cid());
    local_cntx.cid = cmd->Cid();
    crb.SetReplyMode(cmd->ReplyMode());

    local_tx->InitByArgs(local_cntx.conn_state.db_index, args);
    service_->InvokeCmd(cmd->Cid(), args, &local_cntx);

    sinfo.replies.emplace_back(crb.Take());

    // Assert commands made no persistent state changes to stub context state
    const auto& local_state = local_cntx.conn_state;
    DCHECK_EQ(local_state.db_index, cntx_->conn_state.db_index);
    CheckConnStateClean(local_state);
  }

  // ConnectionContext deletes the reply builder upon destruction, so
  // remove our local pointer from it.
  local_cntx.Inject(nullptr);

  reverse(sinfo.replies.begin(), sinfo.replies.end());
  return OpStatus::OK;
}

bool MultiCommandSquasher::ExecuteSquashed() {
  DCHECK(!cntx_->conn_state.exec_info.IsCollecting());

  if (order_.empty())
    return true;

  for (auto& sd : sharded_)
    sd.replies.reserve(sd.cmds.size());

  Transaction* tx = cntx_->transaction;
  ServerState::tlocal()->stats.multi_squash_executions++;
  ProactorBase* proactor = ProactorBase::me();
  uint64_t start = proactor->GetMonotonicTimeNs();

  // Atomic transactions (that have all keys locked) perform hops and run squashed commands via
  // stubs, non-atomic ones just run the commands in parallel.
  if (IsAtomic()) {
    cntx_->cid = base_cid_;
    auto cb = [this](ShardId sid) { return !sharded_[sid].cmds.empty(); };
    tx->PrepareSquashedMultiHop(base_cid_, cb);
    tx->ScheduleSingleHop([this](auto* tx, auto* es) { return SquashedHopCb(tx, es); });
  } else {
    shard_set->RunBlockingInParallel([this, tx](auto* es) { SquashedHopCb(tx, es); },
                                     [this](auto sid) { return !sharded_[sid].cmds.empty(); });
  }

  uint64_t after_hop = proactor->GetMonotonicTimeNs();
  bool aborted = false;

  RedisReplyBuilder* rb = static_cast<RedisReplyBuilder*>(cntx_->reply_builder());
  for (auto idx : order_) {
    auto& replies = sharded_[idx].replies;
    CHECK(!replies.empty());

    aborted |= error_abort_ && CapturingReplyBuilder::GetError(replies.back());

    CapturingReplyBuilder::Apply(std::move(replies.back()), rb);
    replies.pop_back();

    if (aborted)
      break;
  }
  uint64_t after_reply = proactor->GetMonotonicTimeNs();
  ServerState::SafeTLocal()->stats.multi_squash_exec_hop_usec += (after_hop - start) / 1000;
  ServerState::SafeTLocal()->stats.multi_squash_exec_reply_usec += (after_reply - after_hop) / 1000;

  for (auto& sinfo : sharded_)
    sinfo.cmds.clear();

  order_.clear();
  return !aborted;
}

void MultiCommandSquasher::Run() {
  DVLOG(1) << "Trying to squash " << cmds_.size() << " commands for transaction "
           << cntx_->transaction->DebugId();

  for (auto& cmd : cmds_) {
    auto res = TrySquash(&cmd);

    if (res == SquashResult::ERROR)
      break;

    if (res == SquashResult::NOT_SQUASHED || res == SquashResult::SQUASHED_FULL) {
      if (!ExecuteSquashed())
        break;
    }

    if (res == SquashResult::NOT_SQUASHED) {
      if (!ExecuteStandalone(&cmd))
        break;
    }
  }

  ExecuteSquashed();  // Flush leftover

  // Set last txid.
  cntx_->last_command_debug.clock = cntx_->transaction->txid();

  if (!sharded_.empty())
    cntx_->transaction->ReportWritesSquashedMulti(
        [this](ShardId sid) { return sharded_[sid].had_writes; });

  // UnlockMulti is a no-op for non-atomic multi transactions,
  // still called for correctness and future changes
  if (!IsAtomic()) {
    for (auto& sd : sharded_) {
      if (sd.local_tx)
        sd.local_tx->UnlockMulti();
    }
  }

  VLOG(1) << "Squashed " << num_squashed_ << " of " << cmds_.size()
          << " commands, max fanout: " << num_shards_ << ", atomic: " << atomic_;
}

bool MultiCommandSquasher::IsAtomic() const {
  return atomic_;
}

}  // namespace dfly
