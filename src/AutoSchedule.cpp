#include<algorithm>

#include "AutoSchedule.h"
#include "RealizationOrder.h"
#include "FindCalls.h"
#include "Simplify.h"
#include "Substitute.h"
#include "Target.h"
#include "Function.h"
#include "Bounds.h"
#include "Var.h"
#include "IRPrinter.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;
using std::map;
using std::set;
using std::deque;
using std::pair;
using std::make_pair;


void set_schedule_defaults(map<string, Function> &env) {
  // Changing the default to compute root.

  // TODO: This ignores existing schedules specified by
  // the user atm and needs to be addressed when we have
  // decided on a mechanism to inform the auto scheduler
  // not to mess with an user specified schedule.
  for (auto& kv : env) {
    // The shedule is marked touched when a user modifies
    // the schedule. The idea is to keep the user specified
    // schedule intact as much as possible.
    // TODO: However, user specified schedules can have
    // non-local effects and will not be captured by touched.
    // For example:
    // f.compute_at(g, y) now specifies constraints on the
    // schedule of f as well as the schedule of g i.e., the
    // variable y in g cannot be split or reordered since that
    // may change user intent.
    //
    // Open question is how to deal with the constraints induced
    // by user specified schedules.
    kv.second.schedule().store_level().func = "";
    kv.second.schedule().store_level().var = "__root";
    kv.second.schedule().compute_level().func = "";
    kv.second.schedule().compute_level().var = "__root";

    // Initializing the schedules for update definitions
    for (size_t u = 0; u < kv.second.updates().size(); u++) {
      kv.second.update_schedule(u).store_level().func = "";
      kv.second.update_schedule(u).store_level().var = "__root";
      kv.second.update_schedule(u).compute_level().func = "";
      kv.second.update_schedule(u).compute_level().var = "__root";
    }
  }
}

bool check_estimates_on_outputs(const vector<Function> &outputs) {
  bool estimates_avail = true;
  for (auto &out : outputs) {
    const vector<Bound> &estimates = out.schedule().estimates();
    if (estimates.size() != out.args().size()) {
      estimates_avail = false;
      break;
    }
    vector<string> vars = out.args();

    for (unsigned int i = 0; i < estimates.size(); i++) {
      if (std::find(vars.begin(), vars.end(), estimates[i].var) == vars.end()
          || !((estimates[i].min.as<IntImm>()) &&
               (estimates[i].extent.as<IntImm>())))  {
        estimates_avail = false;
        break;
      }
    }
  }
  return estimates_avail;
}

struct CostModel {

  /* Visitor for computing the arithmetic cost of a single value of a function*/
  class ExprCost : public IRVisitor {
   public:
    int ops;
    int loads;

    ExprCost() {
      ops = 0; loads = 0;
    }

    using IRVisitor::visit;

    void visit(const IntImm *) {}
    void visit(const UIntImm *) {}
    void visit(const FloatImm *) {}
    void visit(const StringImm *) {}
    void visit(const Cast * op) {
      op->value.accept(this);
      ops+=1;
    }
    void visit(const Variable *) {}

    template<typename T>
        void visit_binary_operator(const T *op, int cost) {
          op->a.accept(this);
          op->b.accept(this);
          ops += cost;
        }

    // TODO: Figure out the right costs
    void visit(const Add *op) {visit_binary_operator(op, 1);}
    void visit(const Sub *op) {visit_binary_operator(op, 1);}
    void visit(const Mul *op) {visit_binary_operator(op, 1);}
    void visit(const Div *op) {visit_binary_operator(op, 1);}
    void visit(const Mod *op) {visit_binary_operator(op, 1);}
    void visit(const Min *op) {visit_binary_operator(op, 1);}
    void visit(const Max *op) {visit_binary_operator(op, 1);}
    void visit(const EQ *op) {visit_binary_operator(op, 1);}
    void visit(const NE *op) {visit_binary_operator(op, 1);}
    void visit(const LT *op) {visit_binary_operator(op, 1);}
    void visit(const LE *op) {visit_binary_operator(op, 1);}
    void visit(const GT *op) {visit_binary_operator(op, 1);}
    void visit(const GE *op) {visit_binary_operator(op, 1);}
    void visit(const And *op) {visit_binary_operator(op, 1);}
    void visit(const Or *op) {visit_binary_operator(op, 1);}

    void visit(const Not *op) {
      op->a.accept(this);
      ops+=1;
    }

    void visit(const Select *op) {
      op->condition.accept(this);
      op->true_value.accept(this);
      op->false_value.accept(this);
      ops+=1;
    }

    // TODO: Figure out the right costs
    void visit(const Call * call) {
      if (call->call_type == Call::Halide) {
        loads+=1;
      } else if (call->call_type == Call::Extern) {
        ops+=1;
      } else if (call->call_type == Call::Image) {
        loads+=1;
      } else if (call->call_type == Call::Intrinsic) {
        ops+=1;
      }
      for (size_t i = 0; (i < call->args.size()); i++)
        call->args[i].accept(this);
    }

    void visit(const Let * let) {
      let->value.accept(this);
      let->body.accept(this);
    }
    // Should not hit any of these IR nodes at this
    // stage of compilation
    void visit(const Load *) { assert(0); }
    void visit(const Ramp *) { assert(0); }
    void visit(const Broadcast *) { assert(0); }
    void visit(const LetStmt *) { assert(0); }
    void visit(const AssertStmt *) {}
    void visit(const ProducerConsumer *) { assert(0); }
    void visit(const For *) { assert(0); }
    void visit(const Store *) { assert(0); }
    void visit(const Provide *) { assert(0); }
    void visit(const Allocate *) { assert(0); }
    void visit(const Free *) { assert(0); }
    void visit(const Realize *) { assert(0); }
    void visit(const Block *) { assert(0); }
    void visit(const IfThenElse *) { assert(0); }
    void visit(const Evaluate *) { assert(0); }
  };

  pair<size_t, size_t> get_expr_cost(Expr e) {
    ExprCost cost_visitor;
    e.accept(&cost_visitor);
    return make_pair(cost_visitor.ops, cost_visitor.loads);
  }

  CostModel() {}
};

struct DependenceAnalysis {

  const map<string, Function> &env;
  const FuncValueBounds &func_val_bounds;

  // TODO: Build a cache for bounds queries

  DependenceAnalysis(map<string, Function> &_env,
                     const FuncValueBounds &_func_val_bounds):
                     env(_env), func_val_bounds(_func_val_bounds) {}

  void simplify_box(Box& b) {
    for (unsigned int i = 0; i < b.size(); i++) {
      b[i].min = simplify(b[i].min);
      b[i].max = simplify(b[i].max);
    }
  }

  /* Compute the regions of producers required to compute a region of the function
     'f' given concrete sizes of the tile in each dimension. */
  map<string, Box> regions_required(Function f,
                                    const vector<Interval> &conc_bounds){

    map<string, Box> regions;
    // Add the function and its region to the queue
    deque< pair<Function, vector<Interval> > > f_queue;
    f_queue.push_back(make_pair(f, conc_bounds));

    // Recursively compute the regions required
    while(!f_queue.empty()) {

      Function curr_f = f_queue.front().first;
      vector<Interval> curr_bounds = f_queue.front().second;
      f_queue.pop_front();

      for (auto &val: curr_f.values()) {
        map<string, Box> curr_regions;
        Scope<Interval> curr_scope;
        int interval_index = 0;

        for (auto& arg: curr_f.args()) {
          Interval simple_bounds = Interval(simplify(curr_bounds[interval_index].min),
                                            simplify(curr_bounds[interval_index].max));
          curr_scope.push(arg, simple_bounds);
          interval_index++;
        }

        curr_regions = boxes_required(val, curr_scope, func_val_bounds);
        for (auto& reg: curr_regions) {
          // Merge region with an existing region for the function in
          // the global map
          if (regions.find(reg.first) == regions.end())
            regions[reg.first] = reg.second;
          else
            merge_boxes(regions[reg.first], reg.second);

          if (env.find(reg.first) != env.end())
            f_queue.push_back(make_pair(env.at(reg.first),
                                        reg.second.bounds));
        }
      }
      // TODO: Currently not handling updates
    }

    // Simplify
    map<string, Box> concrete_regions;

    for (auto &f_reg : regions) {
      simplify_box(f_reg.second);

      Box concrete_box;
      for (unsigned int i = 0; i < f_reg.second.size(); i++) {
        Expr lower = f_reg.second[i].min;
        Expr upper = f_reg.second[i].max;

        // Use the estimates if the lower and upper bounds cannot be
        // determined
        if (!lower.as<IntImm>()) {
          const Function &curr_f = env.at(f_reg.first);
          for (auto &b: curr_f.schedule().estimates()) {
            unsigned int num_pure_args = curr_f.args().size();
            if (i < num_pure_args && b.var == curr_f.args()[i])
              lower = Expr(b.min.as<IntImm>()->value);
          }
        }

        if (!upper.as<IntImm>()) {
          const Function &curr_f = env.at(f_reg.first);
          for (auto &b: curr_f.schedule().estimates()) {
            unsigned int num_pure_args = curr_f.args().size();
            if (i < num_pure_args && b.var == curr_f.args()[i]) {
              const IntImm * bmin = b.min.as<IntImm>();
              const IntImm * bextent = b.extent.as<IntImm>();
              upper = Expr(bmin->value + bextent->value - 1);
            }
          }
        }

        Interval concrete_bounds = Interval(lower, upper);
        concrete_box.push_back(concrete_bounds);
      }
      concrete_regions[f_reg.first] = concrete_box;
    }
    return concrete_regions;
  }

  /* Compute the redundant regions computed while computing a tile of the function
     'f' given sizes of the tile in each dimension. */
  map<string, Box> redundant_regions(Function f, int dir,
                                     const vector<Interval> &conc_bounds){

    map<string, Box> regions = regions_required(f, conc_bounds);

    vector<Interval> shifted_bounds;
    int num_pure_args = f.args().size();
    for (int arg = 0; arg < num_pure_args; arg++) {
      if (dir == arg) {
        Expr len = conc_bounds[arg].max - conc_bounds[arg].min + 1;
        Interval bound = Interval(conc_bounds[arg].min + len,
                                  conc_bounds[arg].max + len);
        shifted_bounds.push_back(bound);
      }
      else
        shifted_bounds.push_back(conc_bounds[arg]);
    }

    map<string, Box> regions_shifted = regions_required(f, shifted_bounds);

    map<string, Box> overalps;
    for (auto& reg: regions) {
      if (regions_shifted.find(reg.first) == regions.end()) {
        // It will be interesting to log cases where this actually
        // happens
        continue;
      } else {
        Box b = reg.second;
        Box b_shifted = regions_shifted[reg.first];
        // The boxes should be of the same size
        assert(b.size() == b_shifted.size());
        // The box used makes things complicated ignoring it for now
        Box b_intersect;
        for (unsigned int i = 0 ; i < b.size(); i++)
          b_intersect.push_back(interval_intersect(b[i], b_shifted[i]));
        // A function should appear once in the regions and therefore cannot
        // already be present in the overlaps map
        assert(overalps.find(reg.first) == overalps.end());
        overalps[reg.first] = b_intersect;
      }
    }
    // Simplify
    for (auto &f : overalps)
      simplify_box(f.second);

    return overalps;
  }


  map<string, Box>
      concrete_dep_regions(string name, vector<Interval> &bounds) {
        return regions_required(env.at(name), bounds);
      }

  vector< map<string, Box> >
      concrete_overlap_regions(string name, vector<Interval> &bounds) {

        vector< map<string, Box> > conc_overlaps;
        size_t num_args = env.at(name).args().size();

        for (size_t dir = 0; dir < num_args; dir++) {
          map<string, Box> conc_reg =
              redundant_regions(env.at(name), dir, bounds);
          conc_overlaps.push_back(conc_reg);
        }
        return conc_overlaps;
      }
};

void disp_regions(map<string, Box> &regions) {
    for (auto& reg: regions) {
        debug(0) << reg.first;
        debug(0) << reg.second;
        debug(0) << "\n";
    }
}

map<string, Box> get_pipeline_bounds(DependenceAnalysis &analy,
                                     const vector<Function> &outputs) {
  map<string, Box> pipeline_bounds;

  for (auto &out: outputs) {
    vector<Interval> bounds;
    vector<string> vars = out.args();
    for (size_t i = 0; i < vars.size(); i++) {
      for (auto &b: out.schedule().estimates())
        if (b.var == vars[i]) {
          Interval I = Interval(b.min, simplify(b.min + b.extent - 1));
          bounds.push_back(I);
        }
    }

    map<string, Box> regions =
        analy.concrete_dep_regions(out.name(), bounds);

    // Add the output region to the pipeline bounds as well
    regions[out.name()] = bounds;

    for (auto& reg: regions) {
      // Merge region with an existing region for the function in
      // the global map
      if (pipeline_bounds.find(reg.first) == pipeline_bounds.end())
        pipeline_bounds[reg.first] = reg.second;
      else
        merge_boxes(pipeline_bounds[reg.first], reg.second);
    }
  }

  return pipeline_bounds;
}

void generate_schedules(const vector<Function> &outputs,
                        const Target &target) {
  // Compute an environment
  map<string, Function> env;
  for (Function f : outputs) {
    map<string, Function> more_funcs = find_transitive_calls(f);
    env.insert(more_funcs.begin(), more_funcs.end());
  }

  // Compute a realization order
  vector<string> order = realization_order(outputs, env);

  // Set the schedule defaults for each funtion in the environment
  set_schedule_defaults(env);

  // Compute the expression costs for each function in the pipeline

  // Dependence analysis to compute all the regions of upstream functions
  // required to compute a region of the function

  FuncValueBounds func_val_bounds = compute_function_value_bounds(order, env);

  bool estimates_avail = check_estimates_on_outputs(outputs);

  // Inform the user that estimates of output sizes were not available on
  // the outputs of the pipeline.
  user_assert(estimates_avail) << "Please provide estimates for each \
                               dimension of the pipeline output functions.";

  map<string, vector<string> > update_args;
  set<string> reductions;
  DependenceAnalysis analy(env, func_val_bounds);

  // Show bounds of all the functions in the pipeline given estimates
  // on outputs. Also report fuctions where the bounds could not be inferred.
  map<string, Box> pipeline_bounds = get_pipeline_bounds(analy, outputs);
  disp_regions(pipeline_bounds);

  // Initialize the cost model
  // TODO: Build a class which encapsulates the cost model
  // TODO: Arithmetic cost model and functions which help in computing
  // foot prints go here

  // TODO: Partitioner which is capable of auto scheduling hierarchically
  // TODO: Auto scheduler modes
  // O1 Does not introduce any redundant compute but performs basic fusion
  // O2 No redundant compute basic fusion and reordering
  // O3 Trades-offs redundant work for enhancing locality and parallelism

  // TODO: Realize the generated schedule
  // CPU

  // TODO: Update definitions
  // TODO: Boundary conditions

  // TODO: Realize the generated schedule
  // GPU
  // ...

}

}
}
