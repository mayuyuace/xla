/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/multi_output_fusion.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <tuple>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_set.h"
#include "xla/debug_options_flags.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/ir/hlo_reachability.h"
#include "xla/service/gpu/gpu_device_info.h"
#include "xla/service/gpu/gpu_fusible.h"
#include "xla/service/gpu/gpu_hlo_cost_analysis.h"
#include "xla/service/gpu/gpu_performance_model.h"
#include "xla/service/hlo_graph_dumper.h"
#include "xla/service/instruction_fusion.h"
#include "xla/shape_util.h"

namespace xla {
namespace gpu {

namespace {

bool IsProfitableOperand(HloInstruction* instr) {
  // Effective scalars are not a profitable shared operand. Skip them.
  return !ShapeUtil::IsEffectiveScalar(instr->shape());
}

// Finds and returns the unique `slice` op where `parent` is used in `instr`.
// Returns `nullptr` if no such `slice` exists.
const HloSliceInstruction* FindUniqueSlice(const HloInstruction* parent,
                                           const HloInstruction* instr) {
  if (const auto* slice = DynCast<HloSliceInstruction>(instr)) {
    return slice;
  } else if (const auto* fusion = DynCast<HloFusionInstruction>(instr)) {
    const HloSliceInstruction* result = nullptr;
    for (size_t i = 0; i < fusion->operand_count(); ++i) {
      if (fusion->operand(i) == parent) {
        // Parameter used more than once -> there's no unique slice.
        if (result) return nullptr;

        auto* called_param = fusion->fused_parameter(i);
        if (called_param->user_count() != 1) return nullptr;

        result = FindUniqueSlice(called_param, called_param->users()[0]);
        if (!result) return nullptr;
      }
    }
    return result;
  } else {
    return nullptr;
  }
}

FusionDecision ParameterSlicesAreNonOverlapping(const HloInstruction& instr1,
                                                const HloInstruction& instr2,
                                                const HloInstruction* parent) {
  if (parent->shape().IsTuple()) return {};
  // Allow MOF if the parameter is small, even if there's no overlap. 1024 bytes
  // were arbitrarily chosen as the threshold.
  if (ShapeUtil::ByteSizeOfElements(parent->shape()) < 1024) return {};

  const HloSliceInstruction* slice1 = FindUniqueSlice(parent, &instr1);
  const HloSliceInstruction* slice2 = FindUniqueSlice(parent, &instr2);
  if (!slice1 || !slice2) return {};

  // TODO(jreiffers): Check strides as well.
  auto& starts1 = slice1->slice_starts();
  auto& starts2 = slice2->slice_starts();
  auto& limits1 = slice1->slice_limits();
  auto& limits2 = slice2->slice_limits();

  for (int64_t dim = 0; dim < parent->shape().rank(); ++dim) {
    bool overlap = starts1[dim] < limits2[dim] && starts2[dim] < limits1[dim];
    if (!overlap) {
      return "slices are non-overlapping";
    }
  }
  return {};
}

FusionDecision LegalToFuse(const HloInstruction& instr1,
                           const HloInstruction& instr2,
                           const GpuDeviceInfo& device_info,
                           FusionInfoCache* fusion_info_cache) {
  CHECK(instr1.opcode() == HloOpcode::kFusion);

  // The emitter only supports in-place DUS for fusions with a single DUS at the
  // root. Don't sibling fuse DUS for now.
  // TODO(b/119178699): Multi-output fusing DUS can improve performance if we
  // share the input and output buffers and add support to the emitter.
  if (instr1.fused_expression_root()->opcode() ==
          HloOpcode::kDynamicUpdateSlice ||
      (instr2.opcode() == HloOpcode::kFusion &&
       instr2.fused_expression_root()->opcode() ==
           HloOpcode::kDynamicUpdateSlice)) {
    return "can't fuse multiple DUSs";
  }

  // Do this check last, as it may be expensive.
  return FusionFitsInBudget(instr1, instr2, device_info,
                            /*is_consumer_producer_fusion=*/false,
                            fusion_info_cache);
}

// We prefer multi-output fusions over other fusions over unfused ops, because
// we want to preserve fusion opportunities if possible.
int FusionPriority(const HloInstruction* instr) {
  if (instr->IsMultiOutputFusion()) {
    return 2;
  }
  if (instr->opcode() == HloOpcode::kFusion) {
    return 1;
  }
  return 0;
}

HloInstruction* SelectPreferredFusionCandidate(
    const std::vector<HloInstruction*> candidates) {
  if (candidates.empty()) {
    return nullptr;
  }
  return *std::max_element(
      candidates.begin(), candidates.end(),
      [](const HloInstruction* a, const HloInstruction* b) {
        return FusionPriority(a) < FusionPriority(b);
      });
}

// Do not fuse a producer if the other operands of the fusion are
// reachable from the producer, this would create a cycle.
FusionDecision OperandReachableFromProducer(
    const HloInstruction& producer, const HloInstruction& consumer,
    const HloReachabilityMap& reachability) {
  for (const auto* operand : consumer.operands()) {
    // If a get-tuple-element instruction is not in the reachability
    // map, it has been created by fusion in this pass. Simply move
    // on to its operand, which is in the reachability map.
    if (!reachability.IsPresent(operand) &&
        operand->opcode() == HloOpcode::kGetTupleElement) {
      operand = operand->operand(0);
    }
    CHECK(reachability.IsPresent(operand) && reachability.IsPresent(&producer))
        << "Reachability map is incomplete. This should never "
           "happen.";
    if (&producer != operand && reachability.IsReachable(&producer, operand)) {
      return {
          absl::StrCat(producer.name(), " would introduce a cycle when fused")};
    }
  }
  return {};
}

std::vector<HloInstruction*> GetProducerConsumerMultiOutputFusionCandidates(
    const HloInstruction* producer, const HloReachabilityMap& reachability,
    FusionInfoCache* fusion_info_cache, GpuHloCostAnalysis* cost_analysis,
    const GpuDeviceInfo& device_info, se::CudaComputeCapability cc) {
  std::vector<HloInstruction*> fusion_candidates;
  const HloComputation* computation = producer->parent();
  const HloModule* module = computation->parent();
  bool dump_fusion =
      module->config().debug_options().xla_dump_fusion_visualization();

  // If the producer is not a valid candidate for MOF, no need to check any of
  // its users.
  if (!IsProducerMultiOutputFusible(*producer)) {
    return fusion_candidates;
  }

  // If there is only one user, and it is not a multi-output fusion node, this
  // fusion possibility was already considered and rejected by the FusionMerger
  // pass. No need to try again!
  if (producer->user_count() == 1 &&
      !producer->users()[0]->IsMultiOutputFusion()) {
    return fusion_candidates;
  }

  using std::placeholders::_1, std::placeholders::_2;
  std::tuple checks{
      [](const HloInstruction& producer,
         const HloInstruction& consumer) -> FusionDecision {
        return {IsFusibleAsMultiOutputFusionRoot(consumer),
                "consumer not eligible as multi-output fusion root."};
      },
      &ShapesCompatibleForMultiOutputFusion,
      std::bind(OperandReachableFromProducer, _1, _2, std::cref(reachability)),
      std::bind(FusionFitsInBudget, _1, _2, std::cref(device_info),
                /*is_consumer_producer_fusion=*/false, fusion_info_cache),
      [&](const HloInstruction& producer,
          const HloInstruction& consumer) -> FusionDecision {
        return {
            !cost_analysis->ProducerConsumerMergedTooLarge(producer, consumer),
            "will generate too large IR"};
      },
      [&](const HloInstruction& producer,
          const HloInstruction& consumer) -> FusionDecision {
        bool use_experimental_block_size =
            producer.GetModule()
                ->config()
                .debug_options()
                .xla_gpu_enable_experimental_block_size();
        GpuPerformanceModel::RunTimes t = GpuPerformanceModel::EstimateRunTimes(
            &producer, cost_analysis, device_info, use_experimental_block_size,
            cc,
            // `EstimateRunTimes`'s interface violates const correctness, so we
            // need the const cast here.
            {const_cast<HloInstruction*>(&consumer)},
            /*multi_output=*/true);
        return {t.time_fused <= t.time_unfused, "will execute slower if fused"};
      }};

  for (HloInstruction* consumer : producer->users()) {
    VLOG(3) << "Looking at producer " << producer->name()
            << " and its consumer " << consumer->name();

    if (auto decision = FusionDecision::All(checks, *producer, *consumer)) {
      fusion_candidates.push_back(consumer);
    } else if (dump_fusion) {
      RegisterFusionState(
          *computation,
          absl::StrCat("Not considering fusion of producer |", "|",
                       producer->name(), "| into consumer |", consumer->name(),
                       "| due to: ", decision.Explain()),
          *consumer, producer);
    }
  }
  return fusion_candidates;
}

bool IsSiblingFusionCandidate(const HloInstruction* instr) {
  if (instr->users().empty() || !IsFusibleAsMultiOutputFusionRoot(*instr) ||
      IsNestableVariadicReduction(*instr)) {
    return false;
  }
  // Check if the users of multioutput fusion is not a get-tuple-element.
  // If this is the case, we bail out because the transformation assumes
  // the users are get-tuple-element.
  if (instr->IsMultiOutputFusion()) {
    for (HloInstruction* user : instr->users()) {
      if (user->opcode() != HloOpcode::kGetTupleElement) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

void GpuMultiOutputFusion::RecomputeReachability() {
  reachability_ = HloReachabilityMap::Build(computation_);
}

bool GpuMultiOutputFusion::FuseSiblings(HloInstruction* parent,
                                        FusionInfoCache* fusion_info_cache,
                                        GpuHloCostAnalysis* cost_analysis) {
  const HloComputation* computation = parent->parent();
  const HloModule* module = computation->parent();
  bool dump_fusion =
      module->config().debug_options().xla_dump_fusion_visualization();

  if (!IsProfitableOperand(parent)) {
    VLOG(3) << "Operand " << parent->ToShortString() << " is not profitable";
    return false;
  }
  bool changed = false;
  std::vector<HloInstruction*> siblings;
  // Only consider siblings that are fusion candidates.
  absl::c_copy_if(parent->users(), std::back_inserter(siblings),
                  IsSiblingFusionCandidate);
  // Sort the siblings such that multi-output fusion ops occur first, followed
  // by fusion ops, followed by unfused ops.
  absl::c_stable_sort(siblings,
                      [](const HloInstruction* a, const HloInstruction* b) {
                        return FusionPriority(a) > FusionPriority(b);
                      });
  using std::placeholders::_1, std::placeholders::_2;
  std::tuple fusible_checks{
      [&](const HloInstruction& i, const HloInstruction& j) -> FusionDecision {
        return FusionDecision{
            !reachability_->IsConnected(&i, &j),
            absl::StrCat(i.name(), " and ", j.name(), " are connected")};
      },
      &ShapesCompatibleForMultiOutputFusion,
      // Technically, this check is order-dependent (e.g. siblings A, B, C where
      // {A, B} and {B, C} overlap, but {A, C} do not. If the priority order is
      // [C, A, B], only {C, B} will be fused, and A will only be fused in the
      // next iteration of the fusion pipeline, potentially requiring several
      // iterations to converge. We assume this case to be very rare in
      // practice.
      std::bind(ParameterSlicesAreNonOverlapping, _1, _2, parent),
      // This check should be last, as it may be expensive.
      std::bind(LegalToFuse, _1, _2, std::cref(device_info_),
                fusion_info_cache)};
  for (auto i = siblings.begin(); i != siblings.end(); ++i) {
    VLOG(3) << "Considering " << (*i)->name();
    if ((*i)->opcode() != HloOpcode::kFusion) {
      continue;
    }
    for (auto j = i + 1; j != siblings.end();) {
      VLOG(3) << "Considering " << (*i)->name() << " and " << (*j)->name();

      if (auto fusible = FusionDecision::All(fusible_checks, **i, **j);
          !fusible) {
        // We pick `j` arbitrarily as a consumer.
        if (dump_fusion) {
          RegisterFusionState(
              *computation,
              absl::StrCat("Not fusing siblings |", (**i).name(), "| and |",
                           (**j).name(), "| due to: ", fusible.Explain()),
              // Randomly pick one consumer.
              /*consumer=*/**i,
              /*producer=*/parent);
        }
        ++j;
        continue;
      }
      if (!ConsumeFuel(name(), [&] {
            return absl::StrFormat("Not fusing siblings %s and %s.",
                                   (*i)->name(), (*j)->name());
          })) {
        ++j;
        continue;
      }
      VLOG(2) << "Fuse siblings " << (*i)->name() << " and " << (*j)->name();
      fusion_info_cache->Invalidate(*i);
      fusion_info_cache->Invalidate(*j);
      HloInstruction* remaining = *i;
      HloInstruction* fused = *j;
      TF_CHECK_OK(cost_analysis->RemoveInstruction(remaining));
      TF_CHECK_OK(cost_analysis->RemoveInstruction(fused));

      DumpFusionState(*remaining,
                      absl::StrCat("About to fuse producer |", fused->name(),
                                   "| into consumer |", remaining->name(),
                                   "| inside GPU multi-output fusion"),
                      /*producer=*/fused);

      if (fused->opcode() == HloOpcode::kFusion) {
        remaining->MergeFusionInstructionIntoMultiOutput(fused);
        if (fused->IsInputFusion()) {
          remaining->set_fusion_kind(HloInstruction::FusionKind::kInput);
        }
      } else {
        remaining->FuseInstructionIntoMultiOutput(fused);
        CHECK_EQ(0, fused->user_count());
        TF_CHECK_OK(computation_->RemoveInstruction(fused));
      }
      DumpFusionState(*remaining,
                      absl::StrCat("Fused into consumer |", remaining->name(),
                                   "| inside GPU multi-output fusion"));
      TF_CHECK_OK(cost_analysis->RevisitInstruction(remaining));
      changed = true;
      siblings.erase(j);
      RecomputeReachability();
    }
  }
  return changed;
}

StatusOr<bool> GpuMultiOutputFusion::DoMultiOutputFusion() {
  bool changed = false;
  RecomputeReachability();
  GpuHloCostAnalysis cost_analysis({shape_size_function_,
                                    /*per_second_rates=*/{},
                                    /*count_multiple_input_accesses=*/true});
  TF_RETURN_IF_ERROR(computation_->Accept(&cost_analysis));
  std::vector<HloInstruction*> defs_before_uses =
      computation_->MakeInstructionPostOrder();

  FusionInfoCache fusion_info_cache;
  // Traverse the HLO in uses-before-defs order.
  for (auto it = defs_before_uses.rbegin(); it != defs_before_uses.rend();
       ++it) {
    auto* producer = *it;
    absl::string_view producer_name = producer->name();
    // Never multi-output fuse constants.  To the extent that we want to fuse
    // constants, that should be handled by the regular fusion pass.
    if (producer->opcode() == HloOpcode::kConstant) {
      VLOG(3) << producer->name() << " is a constant.";
      continue;
    }
    if (producer->IsCustomFusion()) {
      continue;
    }
    // First, fuse the consumer ops of the current op, which are siblings.
    if (FuseSiblings(/*parent=*/producer, &fusion_info_cache, &cost_analysis)) {
      changed = true;
    }
    // Second, perform producer-consumer multi-output fusion. This order will
    // ensure that all get-tuple-element ops inserted as a by-product of
    // multi-output fusion will occur before the current op in the order of
    // traversal, and hence, not get into the way of subsequent fusion attempts.
    const auto candidates = GetProducerConsumerMultiOutputFusionCandidates(
        producer, *reachability_, &fusion_info_cache, &cost_analysis,
        device_info_, compute_capability_);
    auto* consumer_for_fusion = SelectPreferredFusionCandidate(candidates);
    if (consumer_for_fusion == nullptr) {
      continue;
    }
    if (!ConsumeFuel(name(), [&] {
          return absl::StrFormat("Not fusing %s and %s.", producer->name(),
                                 consumer_for_fusion->name());
        })) {
      continue;
    }
    changed = true;
    fusion_info_cache.Invalidate(producer);
    fusion_info_cache.Invalidate(consumer_for_fusion);
    TF_RETURN_IF_ERROR(cost_analysis.RemoveInstruction(producer));
    TF_RETURN_IF_ERROR(cost_analysis.RemoveInstruction(consumer_for_fusion));

    if (consumer_for_fusion->opcode() == HloOpcode::kFusion) {
      VLOG(2) << "Fuse producer " << producer->name() << " into its consumer "
              << consumer_for_fusion->name();
      DumpFusionState(
          *consumer_for_fusion,
          absl::StrCat("About to fuse producer |", producer_name,
                       "| into consumer |", consumer_for_fusion->name(),
                       "| inside GPU multi-output fusion"),
          /*producer=*/producer);
      if (producer->opcode() == HloOpcode::kFusion) {
        consumer_for_fusion->MergeFusionInstructionIntoMultiOutput(producer);
      } else {
        consumer_for_fusion->FuseInstructionIntoMultiOutput(producer);
        CHECK_EQ(0, producer->user_count());
        TF_CHECK_OK(computation_->RemoveInstruction(producer));
      }
      TF_RETURN_IF_ERROR(cost_analysis.RevisitInstruction(consumer_for_fusion));

      DumpFusionState(
          *consumer_for_fusion,
          absl::StrCat("Fusing producer |", producer_name, "| into consumer |",
                       consumer_for_fusion->name(),
                       "| inside GPU multi-output fusion"));
      RecomputeReachability();
      continue;
    }
    HloInstruction* input_fusion =
        computation_->AddInstruction(HloInstruction::CreateFusion(
            consumer_for_fusion->shape(),
            ChooseFusionKind(*producer, *consumer_for_fusion),
            consumer_for_fusion));
    VLOG(2) << "Fuse producer " << producer->name() << " and its consumer "
            << consumer_for_fusion->name() << " into " << input_fusion->name();
    DumpFusionState(
        *input_fusion,
        absl::StrCat("About to fuse |", producer_name, "| into consumer |",
                     input_fusion->name(), "| inside GPU multi-output fusion"),
        /*producer=*/input_fusion);
    TF_CHECK_OK(
        computation_->ReplaceInstruction(consumer_for_fusion, input_fusion));
    if (producer->opcode() == HloOpcode::kFusion) {
      input_fusion->MergeFusionInstructionIntoMultiOutput(producer);
    } else {
      input_fusion->FuseInstructionIntoMultiOutput(producer);
      CHECK_EQ(0, producer->user_count());
      TF_CHECK_OK(computation_->RemoveInstruction(producer));
    }
    TF_RETURN_IF_ERROR(cost_analysis.RevisitInstruction(input_fusion));

    DumpFusionState(
        *input_fusion,
        absl::StrCat("Fusing producer |", producer_name, "| into consumer |",
                     input_fusion->name(), "| inside GPU multi-output fusion"));
    RecomputeReachability();
  }
  return changed;
}

void GpuMultiOutputFusion::DumpFusionState(const HloInstruction& consumer,
                                           absl::string_view label,
                                           const HloInstruction* producer) {
  if (consumer.GetModule()
          ->config()
          .debug_options()
          .xla_dump_fusion_visualization()) {
    RegisterFusionState(*computation_, label, consumer, producer);
  }
}

StatusOr<bool> GpuMultiOutputFusion::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  bool changed = false;
  for (auto* computation :
       module->MakeNonfusionComputations(execution_threads)) {
    computation_ = computation;
    TF_ASSIGN_OR_RETURN(bool fusion_changed, DoMultiOutputFusion());
    if (fusion_changed) {
      changed = true;
    }
  }
  return changed;
}

}  // namespace gpu
}  // namespace xla
