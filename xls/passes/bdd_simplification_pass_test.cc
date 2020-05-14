// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/passes/bdd_simplification_pass.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/status/statusor.h"
#include "xls/ir/function.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/package.h"

namespace m = ::xls::op_matchers;

namespace xls {
namespace {

using status_testing::IsOkAndHolds;

class BddSimplificationPassTest : public IrTestBase {
 protected:
  xabsl::StatusOr<bool> Run(Function* f) {
    PassResults results;
    XLS_ASSIGN_OR_RETURN(bool changed,
                         BddSimplificationPass(/*split_ops=*/true)
                             .RunOnFunction(f, PassOptions(), &results));
    return changed;
  }
};

TEST_F(BddSimplificationPassTest, ReplaceAllKnownValues) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue x = fb.Param("x", p->GetBitsType(4));
  BValue y = fb.Param("y", p->GetBitsType(4));
  BValue x_or_not_x = fb.Or(x, fb.Not(x));
  BValue y_and_not_y = fb.And(y, fb.Not(y));
  fb.Concat({x_or_not_x, y_and_not_y});
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  EXPECT_THAT(Run(f), IsOkAndHolds(true));

  EXPECT_THAT(f->return_value(), m::Literal(0b11110000));
}

TEST_F(BddSimplificationPassTest, ReplaceKnownPrefix) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue x = fb.Param("x", p->GetBitsType(16));
  BValue y = fb.Param("y", p->GetBitsType(9));
  fb.And(x, fb.Concat({fb.Literal(UBits(0, 7)), y}));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  EXPECT_THAT(Run(f), IsOkAndHolds(true));

  EXPECT_THAT(f->return_value(),
              m::Concat(m::Literal(0), m::BitSlice(m::And())));
}

TEST_F(BddSimplificationPassTest, ReplaceKnownSuffix) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue x = fb.Param("x", p->GetBitsType(32));
  BValue y = fb.Param("y", p->GetBitsType(31));
  fb.Or(x, fb.Concat({y, fb.Literal(UBits(1, 1))}));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  EXPECT_THAT(Run(f), IsOkAndHolds(true));

  EXPECT_THAT(f->return_value(),
              m::Concat(m::BitSlice(m::Or()), m::Literal(1)));
}

TEST_F(BddSimplificationPassTest, KnownSuffixButNotReplaced) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue x = fb.Param("x", p->GetBitsType(32));
  // The suffix (least-significant bits) of the expression is known the
  // expression is not simplified because the "simplification" is the same as
  // the expression itself (concat of a literal).
  fb.Concat({x, fb.Literal(UBits(123, 10))});
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  EXPECT_THAT(Run(f), IsOkAndHolds(false));

  EXPECT_THAT(f->return_value(), m::Concat(m::Param("x"), m::Literal(123)));
}

TEST_F(BddSimplificationPassTest, RemoveRedundantOneHot) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue x = fb.Param("x", p->GetBitsType(8));
  BValue x_eq_0 = fb.Eq(x, fb.Literal(UBits(0, 8)));
  BValue x_eq_42 = fb.Eq(x, fb.Literal(UBits(42, 8)));
  BValue x_gt_123 = fb.UGt(x, fb.Literal(UBits(123, 8)));
  fb.OneHot(fb.Concat({x_eq_0, x_eq_42, x_gt_123}), LsbOrMsb::kLsb);
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  EXPECT_THAT(Run(f), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(), m::Concat(m::Eq(), m::Concat()));
}

TEST_F(BddSimplificationPassTest, ConvertTwoWayOneHotSelect) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
     fn f(p: bits[1], x: bits[32], y: bits[32]) -> bits[32] {
       not.1: bits[1] = not(p)
       concat.2: bits[2] = concat(p, not.1)
       ret one_hot_sel.3: bits[32] = one_hot_sel(concat.2, cases=[x, y])
     }
  )",
                                                       p.get()));
  EXPECT_THAT(Run(f), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(), m::Select(m::BitSlice(), /*cases=*/{
                                               m::Param("y"), m::Param("x")}));
}

TEST_F(BddSimplificationPassTest, SelectChainOneHot) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue s = fb.Param("s", p->GetBitsType(2));
  BValue pred0 = fb.Eq(s, fb.Literal(UBits(0, 2)));
  BValue pred1 = fb.Eq(s, fb.Literal(UBits(1, 2)));
  BValue pred2 = fb.Eq(s, fb.Literal(UBits(2, 2)));
  BValue pred3 = fb.Eq(s, fb.Literal(UBits(3, 2)));
  auto param = [&](absl::string_view s) {
    return fb.Param(s, p->GetBitsType(8));
  };
  fb.Select(pred3, param("x3"),
            fb.Select(pred2, param("x2"),
                      fb.Select(pred1, param("x1"),
                                fb.Select(pred0, param("x0"), param("y")))));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  ASSERT_THAT(Run(f), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(),
              m::OneHotSelect(m::Concat(m::Eq(m::Param("s"), m::Literal(3)),
                                        m::Eq(m::Param("s"), m::Literal(2)),
                                        m::Eq(m::Param("s"), m::Literal(1)),
                                        m::Eq(m::Param("s"), m::Literal(0))),
                              {m::Param("x0"), m::Param("x1"), m::Param("x2"),
                               m::Param("x3")}));
}

TEST_F(BddSimplificationPassTest, SelectChainOneHotOrZeroSelectors) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue s = fb.Param("s", p->GetBitsType(8));
  BValue pred0 = fb.UGt(s, fb.Literal(UBits(42, 8)));
  BValue pred1 = fb.Eq(s, fb.Literal(UBits(11, 8)));
  BValue pred2 = fb.ULt(s, fb.Literal(UBits(7, 8)));
  auto param = [&](absl::string_view s) {
    return fb.Param(s, p->GetBitsType(8));
  };
  fb.Select(
      pred2, param("x2"),
      fb.Select(pred1, param("x1"), fb.Select(pred0, param("x0"), param("y"))));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  ASSERT_THAT(Run(f), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(),
              m::OneHotSelect(m::Concat(m::ULt(m::Param("s"), m::Literal(7)),
                                        m::Eq(m::Param("s"), m::Literal(11)),
                                        m::UGt(m::Param("s"), m::Literal(42)),
                                        m::Nor(m::ULt(), m::Eq(), m::UGt())),
                              {m::Param("y"), m::Param("x0"), m::Param("x1"),
                               m::Param("x2")}));
}

}  // namespace
}  // namespace xls