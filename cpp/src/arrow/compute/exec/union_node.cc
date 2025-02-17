// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <mutex>

#include "arrow/api.h"
#include "arrow/compute/api.h"
#include "arrow/compute/exec/exec_plan.h"
#include "arrow/compute/exec/options.h"
#include "arrow/compute/exec/util.h"
#include "arrow/util/bitmap_ops.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/future.h"
#include "arrow/util/logging.h"
#include "arrow/util/thread_pool.h"

namespace arrow {

using internal::checked_cast;

namespace compute {

namespace {
std::vector<std::string> GetInputLabels(const ExecNode::NodeVector& inputs) {
  std::vector<std::string> labels(inputs.size());
  for (size_t i = 0; i < inputs.size(); i++) {
    labels[i] = "input_" + std::to_string(i) + "_label";
  }
  return labels;
}
}  // namespace
struct UnionNode : ExecNode {
  UnionNode(ExecPlan* plan, std::vector<ExecNode*> inputs)
      : ExecNode(plan, inputs, GetInputLabels(inputs),
                 /*output_schema=*/inputs[0]->output_schema(),
                 /*num_outputs=*/1) {
    bool counter_completed = input_count_.SetTotal(static_cast<int>(inputs.size()));
    ARROW_DCHECK(counter_completed == false);
  }

  const char* kind_name() override { return "UnionNode"; }

  static Result<ExecNode*> Make(ExecPlan* plan, std::vector<ExecNode*> inputs,
                                const ExecNodeOptions& options) {
    RETURN_NOT_OK(ValidateExecNodeInputs(plan, inputs, static_cast<int>(inputs.size()),
                                         "UnionNode"));
    if (inputs.size() < 1) {
      return Status::Invalid("Constructing a `UnionNode` with inputs size less than 1");
    }
    auto schema = inputs.at(0)->output_schema();
    for (auto input : inputs) {
      if (!input->output_schema()->Equals(schema)) {
        return Status::Invalid(
            "UnionNode input schemas must all match, first schema was: ",
            schema->ToString(), " got schema: ", input->output_schema()->ToString());
      }
    }
    return plan->EmplaceNode<UnionNode>(plan, std::move(inputs));
  }

  void InputReceived(ExecNode* input, ExecBatch batch) override {
    ARROW_DCHECK(std::find(inputs_.begin(), inputs_.end(), input) != inputs_.end());

    if (finished_.is_finished()) {
      return;
    }
    outputs_[0]->InputReceived(this, std::move(batch));
    if (batch_count_.Increment()) {
      finished_.MarkFinished();
    }
  }

  void ErrorReceived(ExecNode* input, Status error) override {
    DCHECK_EQ(input, inputs_[0]);
    outputs_[0]->ErrorReceived(this, std::move(error));

    StopProducing();
  }

  void InputFinished(ExecNode* input, int total_batches) override {
    ARROW_DCHECK(std::find(inputs_.begin(), inputs_.end(), input) != inputs_.end());

    total_batches_.fetch_add(total_batches);

    if (input_count_.Increment()) {
      outputs_[0]->InputFinished(this, total_batches_.load());
      if (batch_count_.SetTotal(total_batches_.load())) {
        finished_.MarkFinished();
      }
    }
  }

  Status StartProducing() override {
    finished_ = Future<>::Make();
    return Status::OK();
  }

  void PauseProducing(ExecNode* output) override {}

  void ResumeProducing(ExecNode* output) override {}

  void StopProducing(ExecNode* output) override {
    DCHECK_EQ(output, outputs_[0]);
    if (batch_count_.Cancel()) {
      finished_.MarkFinished();
    }
    for (auto&& input : inputs_) {
      input->StopProducing(this);
    }
  }

  void StopProducing() override {
    if (batch_count_.Cancel()) {
      finished_.MarkFinished();
    }
    for (auto&& input : inputs_) {
      input->StopProducing(this);
    }
  }

  Future<> finished() override { return finished_; }

 private:
  AtomicCounter batch_count_;
  AtomicCounter input_count_;
  std::atomic<int> total_batches_{0};
  Future<> finished_ = Future<>::MakeFinished();
};

ExecFactoryRegistry::AddOnLoad kRegisterUnion("union", UnionNode::Make);

}  // namespace compute
}  // namespace arrow
