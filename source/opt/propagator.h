// Copyright (c) 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LIBSPIRV_OPT_PROPAGATOR_H_
#define LIBSPIRV_OPT_PROPAGATOR_H_

#include <functional>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cfg.h"
#include "ir_context.h"
#include "module.h"

namespace spvtools {
namespace opt {

// Represents a CFG control edge.
struct Edge {
  explicit Edge(ir::BasicBlock* b1, ir::BasicBlock* b2)
      : source(b1), dest(b2) {}
  ir::BasicBlock* source;
  ir::BasicBlock* dest;
  bool operator<(const Edge& o) const {
    return source < o.source || dest < o.dest;
  }
};

// This class implements a generic value propagation algorithm based on the
// conditional constant propagation algorithm proposed in
//
//      Constant propagation with conditional branches,
//      Wegman and Zadeck, ACM TOPLAS 13(2):181-210.
//
//      A Propagation Engine for GCC
//      Diego Novillo, GCC Summit 2005
//      http://ols.fedoraproject.org/GCC/Reprints-2005/novillo-Reprint.pdf
//
// The purpose of this implementation is to act as a common framework for any
// transformation that needs to propagate values from statements producing new
// values to statements using those values.  Simulation proceeds as follows:
//
// 1- Initially, all edges of the CFG are marked not executable and the CFG
//    worklist is seeded with all the statements in the entry basic block.
//
// 2- Every instruction I is simulated by calling a pass-provided function
//    |visit_fn|. This function is responsible for three things:
//
//    (a) Keep a value table of interesting values.  This table maps SSA IDs to
//        their values.  For instance, when implementing constant propagation,
//        given a store operation 'OpStore %f %int_3', |visit_fn| should assign
//        the value 3 to the table slot for %f.
//
//        In general, |visit_fn| will need to use the value table to replace its
//        operands, fold the result and decide whether a new value needs to be
//        stored in the table. |visit_fn| should only create a new mapping in
//        the value table if all the operands in the instruction are known and
//        present in the value table.
//
//    (b) Return a status indicator to direct the propagator logic.  Once the
//        instruction is simulated, the propagator needs to know whether this
//        instruction produced something interesting.  This is indicated via
//        |visit_fn|'s return value:
//
//         SSAPropagator::kNotInteresting: Instruction I produces nothing of
//             interest and does not affect any of the work lists.  The
//             propagator will visit the statement again if any of its operands
//             produce an interesting value in the future.
//
//             |visit_fn| should always return this value when it is not sure
//             whether the instruction will produce an interesting value in the
//             future or not.  For instance, for constant propagation, an OpIAdd
//             instruction may produce a constant if its two operands are
//             constant, but the first time we visit the instruction, we still
//             may not have its operands in the value table.
//
//         SSAPropagator::kVarying: The value produced by I cannot be determined
//             at compile time.  Further simulation of I is not required.  The
//             propagator will not visit this instruction again.  Additionally,
//             the propagator will add all the instructions at the end of SSA
//             def-use edges to be simulated again.
//
//             If I is a basic block terminator, it will mark all outgoing edges
//             as executable so they are traversed one more time.  Eventually
//             the kVarying attribute will be spread out to all the data and
//             control dependents for I.
//
//             It is important for propagation to use kVarying as a bottom value
//             for the propagation lattice.  It should never be possible for an
//             instruction to return kVarying once and kInteresting on a second
//             visit.  Otherwise, propagation would not stabilize.
//
//         SSAPropagator::kInteresting: Instruction I produces a value that can
//             be computed at compile time.  In this case, |visit_fn| should
//             create a new mapping between I's result ID and the produced
//             value.  Much like the kNotInteresting case, the propagator will
//             visit this instruction again if any of its operands changes.
//             This is useful when the statement changes from one interesting
//             state to another.
//
//    (c) For conditional branches, |visit_fn| may decide which edge to take out
//        of I's basic block.  For example, if the operand for an OpSwitch is
//        known to take a specific constant value, |visit_fn| should figure out
//        the destination basic block and pass it back by setting the second
//        argument to |visit_fn|.
//
//    At the end of propagation, values in the value table are guaranteed to be
//    stable and can be replaced in the IR.
//
// 3- The propagator keeps two work queues.  Instructions are only added to
//    these queues if they produce an interesting or varying value. None of this
//    should be handled by |visit_fn|. The propagator keeps track of this
//    automatically (see SSAPropagator::Simulate for implementation).
//
//      CFG blocks: contains the queue of blocks to be simulated.
//             Blocks are added to this queue if their incoming edges are
//             executable.
//
//      SSA Edges: An SSA edge is a def-use edge between a value-producing
//              instruction and its use instruction.  The SSA edges list
//              contains the statements at the end of a def-use edge that need
//              to be re-visited when an instruction produces a kVarying or
//              kInteresting result.
//
// 4- Simulation terminates when all work queues are drained.
//
//
// EXAMPLE: Basic constant store propagator.
//
// Suppose we want to propagate all constant assignments of the form "OpStore
// %id %cst" where "%id" is some variable and "%cst" an OpConstant.  The
// following code builds a table |values| where every id that was assigned a
// constant value is mapped to the constant value it was assigned.
//
//   auto ctx = spvtools::BuildModule(...);
//   std::map<uint32_t, uint32_t> values;
//   const auto visit_fn = [&ctx, &values](ir::Instruction* instr,
//                                         ir::BasicBlock** dest_bb) {
//     if (instr->opcode() == SpvOpStore) {
//       uint32_t rhs_id = instr->GetSingleWordOperand(1);
//       ir::Instruction* rhs_def = ctx->get_def_use_mgr()->GetDef(rhs_id);
//       if (rhs_def->opcode() == SpvOpConstant) {
//         uint32_t val = rhs_def->GetSingleWordOperand(2);
//         values[rhs_id] = val;
//         return opt::SSAPropagator::kInteresting;
//       }
//     }
//     return opt::SSAPropagator::kVarying;
//   };
//   opt::SSAPropagator propagator(ctx.get(), &cfg, visit_fn);
//   propagator.Run(&fn);
//
// Given the code:
//
//       %int_4 = OpConstant %int 4
//       %int_3 = OpConstant %int 3
//       %int_1 = OpConstant %int 1
//                OpStore %x %int_4
//                OpStore %y %int_3
//                OpStore %z %int_1
//
// After SSAPropagator::Run returns, the |values| map will contain the entries:
// values[%x] = 4, values[%y] = 3, and, values[%z] = 1.
class SSAPropagator {
 public:
  // Lattice values used for propagation. See class documentation for
  // a description.
  enum PropStatus { kNotInteresting, kInteresting, kVarying };

  using VisitFunction =
      std::function<PropStatus(ir::Instruction*, ir::BasicBlock**)>;

  SSAPropagator(ir::IRContext* context, ir::CFG* cfg,
                const VisitFunction& visit_fn)
      : ctx_(context), cfg_(cfg), visit_fn_(visit_fn) {}

  // Run the propagator on function |fn|. Returns true if changes were made to
  // the function. Otherwise, it returns false.
  bool Run(ir::Function* fn);

 private:
  // Initialize processing.
  void Initialize(ir::Function* fn);

  // Simulate the execution |block| by calling |visit_fn_| on every instruction
  // in it.
  bool Simulate(ir::BasicBlock* block);

  // Simulate the execution of |instr| by replacing all the known values in
  // every operand and determining whether the result is interesting for
  // propagation. This invokes the callback function |visit_fn_| to determine
  // the value computed by |instr|.
  bool Simulate(ir::Instruction* instr);

  // Returns true if |instr| should be simulated again.
  bool ShouldSimulateAgain(ir::Instruction* instr) const {
    return do_not_simulate_.find(instr) == do_not_simulate_.end();
  }

  // Add |instr| to the set of instructions not to simulate again.
  void DontSimulateAgain(ir::Instruction* instr) {
    do_not_simulate_.insert(instr);
  }

  // Returns true if |block| has been simulated already.
  bool BlockHasBeenSimulated(ir::BasicBlock* block) {
    return simulated_blocks_.find(block) != simulated_blocks_.end();
  }

  // Marks block |block| as simulated.
  void MarkBlockSimulated(ir::BasicBlock* block) {
    simulated_blocks_.insert(block);
  }

  // Marks |edge| as executable.  Returns false if the edge was already marked
  // as executable.
  bool MarkEdgeExecutable(const Edge& edge) {
    return executable_edges_.insert(edge).second;
  }

  // Returns true if |edge| has been marked as executable.
  bool IsEdgeExecutable(const Edge& edge) {
    return executable_edges_.find(edge) != executable_edges_.end();
  }

  // Returns a pointer to the def-use manager for |ctx_|.
  analysis::DefUseManager* get_def_use_mgr() const {
    return ctx_->get_def_use_mgr();
  }

  // If the CFG edge |e| has not been executed, add its destination block to the
  // work list.
  void AddControlEdge(const Edge& e);

  // Add all the instructions that use |id| to the SSA edges work list.
  void AddSSAEdges(uint32_t id);

  // IR context to use.
  ir::IRContext* ctx_;

  // CFG to propagate values on. TODO(dnovillo): The CFG should be part of
  // IRContext.
  ir::CFG* cfg_;

  // Function that visits instructions during simulation. The output of this
  // function is used to determine if the simulated instruction produced a value
  // interesting for propagation. The function is responsible for keeping
  // track of interesting values by storing them in some user-provided map.
  VisitFunction visit_fn_;

  // SSA def-use edges to traverse. Each entry is a destination statement for an
  // SSA def-use edge as returned by |def_use_manager_|.
  std::queue<ir::Instruction*> ssa_edge_uses_;

  // Blocks to simulate.
  std::queue<ir::BasicBlock*> blocks_;

  // Blocks simulated during propagation.
  std::unordered_set<ir::BasicBlock*> simulated_blocks_;

  // Set of instructions that should not be simulated again because they have
  // been found to be in the kVarying state.
  std::unordered_set<ir::Instruction*> do_not_simulate_;

  // Map between a basic block and its predecessor edges.
  // TODO(dnovillo): Move this to ir::CFG and always build them. Alternately,
  // move it to IRContext and build CFG preds/succs on-demand.
  std::unordered_map<ir::BasicBlock*, std::vector<Edge>> bb_preds_;

  // Map between a basic block and its successor edges.
  // TODO(dnovillo): Move this to ir::CFG and always build them. Alternately,
  // move it to IRContext and build CFG preds/succs on-demand.
  std::unordered_map<ir::BasicBlock*, std::vector<Edge>> bb_succs_;

  // Set of executable CFG edges.
  std::set<Edge> executable_edges_;
};

}  // namespace opt
}  // namespace spvtools

#endif  // LIBSPIRV_OPT_PROPAGATOR_H_