// Copyright 2015 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "common/status.h"
#include "exec/unnest-node.h"
#include "exec/subplan-node.h"
#include "exprs/expr-context.h"
#include "runtime/runtime-state.h"
#include "util/runtime-profile.h"

namespace impala {

UnnestNode::UnnestNode(ObjectPool* pool, const TPlanNode& tnode,
    const DescriptorTbl& descs)
  : ExecNode(pool, tnode, descs),
    item_byte_size_(0),
    array_expr_ctx_(NULL),
    array_val_(ArrayVal::null()),
    item_idx_(0),
    num_collections_(0),
    total_collection_size_(0),
    max_collection_size_(-1),
    min_collection_size_(-1),
    avg_collection_size_counter_(NULL),
    max_collection_size_counter_(NULL),
    min_collection_size_counter_(NULL),
    num_collections_counter_(NULL) {
}

Status UnnestNode::Init(const TPlanNode& tnode) {
  RETURN_IF_ERROR(ExecNode::Init(tnode));
  DCHECK(tnode.__isset.unnest_node);
  Expr::CreateExprTree(pool_, tnode.unnest_node.collection_expr, &array_expr_ctx_);
  return Status::OK();
}

Status UnnestNode::Prepare(RuntimeState* state) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  DCHECK(containing_subplan_ != NULL) << "set_containing_subplan() must be called";
  RETURN_IF_ERROR(ExecNode::Prepare(state));

  avg_collection_size_counter_ =
      ADD_COUNTER(runtime_profile_, "AvgCollectionSize", TUnit::DOUBLE_VALUE);
  max_collection_size_counter_ =
      ADD_COUNTER(runtime_profile_, "MaxCollectionSize", TUnit::UNIT);
  min_collection_size_counter_ =
      ADD_COUNTER(runtime_profile_, "MinCollectionSize", TUnit::UNIT);
  num_collections_counter_ =
      ADD_COUNTER(runtime_profile_, "NumCollections", TUnit::UNIT);

  DCHECK_EQ(1, row_desc().tuple_descriptors().size());
  const TupleDescriptor* item_tuple_desc = row_desc().tuple_descriptors()[0];
  DCHECK_NOTNULL(item_tuple_desc);
  item_byte_size_ = item_tuple_desc->byte_size();
  RETURN_IF_ERROR(array_expr_ctx_->Prepare(
      state, containing_subplan_->child(0)->row_desc(), expr_mem_tracker()));
  return Status::OK();
}

Status UnnestNode::Open(RuntimeState* state) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  DCHECK(containing_subplan_->current_row() != NULL);
  RETURN_IF_ERROR(ExecNode::Open(state));
  RETURN_IF_ERROR(array_expr_ctx_->Open(state));
  // Set the array value to be unnested.
  array_val_ = array_expr_ctx_->GetArrayVal(containing_subplan_->current_row());

  ++num_collections_;
  COUNTER_SET(num_collections_counter_, num_collections_);
  total_collection_size_ += array_val_.num_tuples;
  COUNTER_SET(avg_collection_size_counter_,
      static_cast<double>(total_collection_size_) / num_collections_);
  if (max_collection_size_ == -1 || array_val_.num_tuples > max_collection_size_) {
    max_collection_size_ = array_val_.num_tuples;
    COUNTER_SET(max_collection_size_counter_, max_collection_size_);
  }
  if (min_collection_size_ == -1 || array_val_.num_tuples < min_collection_size_) {
    min_collection_size_ = array_val_.num_tuples;
    COUNTER_SET(min_collection_size_counter_, min_collection_size_);
  }
  return Status::OK();
}

Status UnnestNode::GetNext(RuntimeState* state, RowBatch* row_batch, bool* eos) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  RETURN_IF_CANCELLED(state);
  RETURN_IF_ERROR(QueryMaintenance(state));
  *eos = false;

  // Populate the output row_batch with tuples from the array.
  while (item_idx_ < array_val_.num_tuples) {
    Tuple* item = reinterpret_cast<Tuple*>(array_val_.ptr + item_idx_ * item_byte_size_);
    ++item_idx_;
    int row_idx = row_batch->AddRow();
    TupleRow* row = row_batch->GetRow(row_idx);
    row->SetTuple(0, item);
    // TODO: Ideally these should be evaluated by the parent scan node.
    if (EvalConjuncts(&conjunct_ctxs_[0], conjunct_ctxs_.size(), row)) {
      row_batch->CommitLastRow();
      // The limit is handled outside of this loop.
      if (row_batch->AtCapacity()) break;
    }
  }
  num_rows_returned_ += row_batch->num_rows();

  // Checking the limit here is simpler/cheaper than doing it in the loop above.
  if (ReachedLimit()) {
    *eos = true;
    row_batch->set_num_rows(row_batch->num_rows() - (num_rows_returned_ - limit_));
    num_rows_returned_ = limit_;
  }
  if (item_idx_ == array_val_.num_tuples) *eos = true;
  COUNTER_SET(rows_returned_counter_, num_rows_returned_);
  return Status::OK();
}

Status UnnestNode::Reset(RuntimeState* state) {
  item_idx_ = 0;
  return ExecNode::Reset(state);
}

void UnnestNode::Close(RuntimeState* state) {
  if (is_closed()) return;
  DCHECK_NOTNULL(array_expr_ctx_);
  array_expr_ctx_->Close(state);
  ExecNode::Close(state);
}

}
