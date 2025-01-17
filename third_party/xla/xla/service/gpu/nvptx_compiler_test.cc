/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/service/gpu/nvptx_compiler.h"

#include <memory>

#include <gtest/gtest.h>
#include "absl/strings/string_view.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/service/backend.h"
#include "xla/service/buffer_assignment.h"
#include "xla/statusor.h"
#include "xla/tests/hlo_test_base.h"
#include "xla/util.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {

class NVPTXCompilerTest : public HloTestBase {
 public:
  StatusOr<std::unique_ptr<BufferAssignment>> AssignBuffers(HloModule* module) {
    Backend& test_backend = backend();
    NVPTXCompiler compiler;
    return compiler.AssignBuffers(module,
                                  test_backend.default_stream_executor());
  }
};

TEST_F(NVPTXCompilerTest, AllReducePerformedInplace) {
  const absl::string_view hlo_string = R"(
HloModule Module, input_output_alias={ {}: (0, {}, may-alias) }

summit {
  lhs = f32[] parameter(0)
  rhs = f32[] parameter(1)
  ROOT add = f32[] add(lhs, rhs)
}

ENTRY entry {
  param0 = f32[128] parameter(0)
  ROOT allreduce = f32[128] all-reduce(param0),
    replica_groups={}, to_apply=summit
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  TF_ASSERT_OK_AND_ASSIGN(auto buffer_assignment, AssignBuffers(module.get()));

  HloInstruction* all_reduce = module->entry_computation()->root_instruction();
  EXPECT_TRUE(buffer_assignment->SharesTopLevelSlice(all_reduce,
                                                     all_reduce->operand(0)));
}

TEST_F(NVPTXCompilerTest, AllReducePerformedInplaceTwoOperands) {
  const absl::string_view hlo_string = R"(
HloModule Module,
  input_output_alias={ {0}: (0, {}, may-alias), {1}: (1, {}, may-alias) }

summit {
  lhs = f32[] parameter(0)
  rhs = f32[] parameter(1)
  ROOT add = f32[] add(lhs, rhs)
}

ENTRY entry {
  param0 = f32[128] parameter(0)
  param1 = f32[128] parameter(1)
  ROOT allreduce = (f32[128], f32[128]) all-reduce(param0, param1),
    replica_groups={}, to_apply=summit
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  TF_ASSERT_OK_AND_ASSIGN(auto buffer_assignment, AssignBuffers(module.get()));

  HloInstruction* all_reduce = module->entry_computation()->root_instruction();
  EXPECT_TRUE(buffer_assignment->SharesSliceAtIndex(
      all_reduce, {0}, all_reduce->operand(0), {}));
  EXPECT_TRUE(buffer_assignment->SharesSliceAtIndex(
      all_reduce, {1}, all_reduce->operand(1), {}));
}

TEST_F(NVPTXCompilerTest,
       DotDimensionAreSortedBeforePaddingForCublasEnablingTritonFusion) {
  MatchOptimizedHlo(R"(
ENTRY e {
 p0 = f16[11,22,33,44] parameter(0)
 p1 = s8[11,22,33,44] parameter(1)
 p1c = f16[11,22,33,44] convert(p1)
 ROOT d = f16[11,22,44,44] dot(p0, p1c),
  lhs_batch_dims={0,1}, lhs_contracting_dims={2},
  rhs_batch_dims={0,1}, rhs_contracting_dims={2}
})",
                    R"(
; CHECK: ENTRY
; CHECK-NEXT: parameter
; CHECK-NEXT: parameter
; CHECK-NEXT: __triton_gemm
  )");
}

}  // namespace gpu
}  // namespace xla
