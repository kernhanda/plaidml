#include "tile/lang/gen_stripe.h"

#include "tile/lang/compile.h"

namespace vertexai {
namespace tile {
namespace lang {

using namespace stripe;  // NOLINT

namespace {

class StripeGenerator {
 public:
  StripeGenerator(const Program& parsed,  //
                  const Bindings& vars,   //
                  const ShapeMap& inputs,
                  const ShapeMap& outputs)
      : parsed_(parsed),   //
        vars_(vars),       //
        inputs_(inputs),   //
        outputs_(outputs)  //
  {}

  Block Run(const std::string& name) {
    LOG(INFO) << "Compiling " << parsed_.ops.size() << " ops";
    // The top level block represents a program.
    // A single inner block of the program respresents the entry point to the program.
    // Declarations made on the program relate to user supplied inputs and outputs.
    // Declarations made on main relate to temporaries needed for communication between kernels.
    // The list of kernels to execute are the list of blocks defined within main.
    Block program;
    program.name = name;
    auto main = std::make_shared<Block>();
    main->name = "main";
    program.stmts.push_back(main);
    // Add decls for external inputs/outputs
    AddDecls(&program, main.get(), inputs_, true);
    AddDecls(&program, main.get(), outputs_, false);
    // Add references for temporaries, reshapes may elide some
    for (const auto& item : vars_) {
      if (externals_.count(item.first) == 0) {
        const auto& binding = item.second;
        if (binding.tag == Binding::TENSOR) {
          tmps_.insert(std::make_pair(item.first, binding.shape));
        }
      }
    }
    // Add kernels to main
    for (size_t i = 0; i < parsed_.ops.size(); i++) {
      const Op& op = parsed_.ops[i];
      IVLOG(2, "Processing: " << op);
      switch (op.tag) {
        case Op::CONTRACTION:
          ProcessContraction(main.get(), op);
          break;
        case Op::FUNCTION:
          if (op.f.is_special()) {
            ProcessSpecial(main.get(), op);
          } else if (op.f.fn == "reshape") {
            ProcessReshape(main.get(), op);
          } else {
            ProcessElementwise(main.get(), op);
          }
          break;
        case Op::CONSTANT:
          // LOG(INFO) << "CONSTANT";
          break;
      }
    }
    // Add decls for temporaries that are not aliased
    for (const auto& tmp : tmps_) {
      main->decls.insert(tmp);
    }
    IVLOG(2, "Done");
    return program;
  }

 private:
  void AddDecls(Block* program, Block* main, const ShapeMap& shapes, bool is_input) {
    for (const auto& item : shapes) {
      externals_.insert(item.first);
      program->decls.insert(item);
      if (is_input) {
        main->refs.emplace_back(Refinement{RefDir::In, item.first, item.first, BufferAccess{0}});
      } else {
        main->refs.emplace_back(Refinement{RefDir::Out, item.first, item.first, BufferAccess{0}, Intrinsic::ASSIGN});
        auto ref = main->refs.back();
        ref_outs_.insert(std::make_pair(item.first, &ref));
      }
    }
  }

  void ProcessReshape(Block* main, const Op& op) {
    auto it = ref_outs_.find(op.output);
    if (it != ref_outs_.end()) {
      // Remove the temporary
      // tmps_.erase(op.inputs[0]);
      it->second->from = op.inputs[0];
      // Add an alias
      main->refs.emplace_back(Refinement{
          RefDir::In,       // dir
          op.inputs[0],     // from
          op.output,        // into
          BufferAccess{0},  // access
      });
    } else {
      // Remove the temporary
      // tmps_.erase(op.output);
      // Add an alias
      main->refs.emplace_back(Refinement{
          RefDir::In,       // dir
          op.output,        // from
          op.inputs[0],     // into
          BufferAccess{0},  // offset
      });
    }
  }

  void ProcessContraction(Block* main, const Op& op) {
    if (vars_.at(op.output).shape.byte_size() == 0) {
      IVLOG(3, "Contraction output " << op.output << " size==0; skipping");
      return;
    }
    std::vector<Polynomial> out_poly;
    FlatContraction flat = Compile(op.c, MakeTShapes(op.c), &out_poly);
    flat.output = op.output;

    if (NeedsZero(flat)) {
      Op special_op;
      special_op.tag = Op::FUNCTION;
      special_op.output = op.output;
      if (op.c.use_default.empty()) {
        special_op.f.fn = Intrinsic::ZERO;
        special_op.inputs.push_back(op.output);
      } else {
        special_op.f.fn = Intrinsic::COPY;
        special_op.inputs.push_back(op.c.use_default);
      }
      ProcessSpecial(main, special_op);
    }

    auto kernel = AddKernel(main);
    kernel->comments = to_string(op);
    assert(flat.names.size() == flat.ranges.size());
    for (int i = 0; i < flat.names.size(); i++) {
      kernel->idxs.emplace_back(Index{flat.names[i], flat.ranges[i], 0});
    }
    for (const auto& constraint : flat.constraints) {
      kernel->constraints.emplace_back(Constraint{constraint.lhs, constraint.rhs});
    }

    std::vector<std::string> scalar_inputs;
    for (size_t i = 1; i < flat.inputs.size(); i++) {
      auto scalar_name = ScalarName(flat.inputs[i]);
      scalar_inputs.push_back(scalar_name);

      kernel->refs.emplace_back(Refinement{
          RefDir::In,                                                  // dir
          flat.inputs[i],                                              // from
          flat.inputs[i],                                              // into
          BufferAccess{flat.access[i].offset, flat.access[i].strides}  // access
      });

      // LOAD
      kernel->stmts.push_back(std::make_shared<Load>(flat.inputs[i], scalar_name));
    }

    // Combination Op
    if (scalar_inputs.size() > 1) {
      switch (flat.comb_op) {
        case CombinationOp::NONE:
          break;
        case CombinationOp::MULTIPLY:
          AddIntrinsic(kernel.get(), Intrinsic::MUL, scalar_inputs, {ScalarName(flat.output)});
          break;
        case CombinationOp::PLUS:
          AddIntrinsic(kernel.get(), Intrinsic::ADD, scalar_inputs, {ScalarName(flat.output)});
          break;
        case CombinationOp::EQ:
          AddIntrinsic(kernel.get(), Intrinsic::EQ, scalar_inputs, {ScalarName(flat.output)});
          break;
        case CombinationOp::COND:
          AddIntrinsic(kernel.get(), Intrinsic::COND, scalar_inputs, {ScalarName(flat.output)});
          break;
      }
    }

    std::string agg_op;
    switch (flat.agg_op) {
      case AggregationOp::SUM:
        agg_op = Intrinsic::SUM;
        break;
      case AggregationOp::MAX:
        agg_op = Intrinsic::MAX;
        break;
      case AggregationOp::MIN:
        agg_op = Intrinsic::MIN;
        break;
      case AggregationOp::PROD:
        agg_op = Intrinsic::PROD;
        break;
      case AggregationOp::ASSIGN:
        agg_op = Intrinsic::ASSIGN;
        break;
      case AggregationOp::NONE:
        break;
    }
    kernel->refs.emplace_back(Refinement{
        RefDir::Out,                                                  // dir
        flat.output,                                                  // from
        flat.output,                                                  // into
        BufferAccess{flat.access[0].offset, flat.access[0].strides},  // access
        agg_op                                                        // agg_op
    });

    // STORE
    kernel->stmts.push_back(std::make_shared<Store>(ScalarName(flat.output), flat.output));
  }

  void ProcessElementwise(Block* main, const Op& op) {
    auto kernel = AddKernel(main);
    kernel->comments = to_string(op);

    const TensorShape& out_shape = vars_.at(op.output).shape;
    for (std::size_t i = 0; i < out_shape.dims.size(); ++i) {
      kernel->idxs.emplace_back(Index{
          printstring("i%zu", i + 1),  // name
          out_shape.dims[i].size,      // range
          0                            // factor
      });
    }

    for (const auto& input : op.inputs) {
      const auto& binding = vars_.at(input);
      IVLOG(2, "  " << input << ": " << binding);
      switch (binding.tag) {
        case Binding::TENSOR: {
          std::vector<int64_t> strides;
          // Be careful to handle broadcasts
          int diff = out_shape.dims.size() - binding.shape.dims.size();
          for (int i = 0; i < out_shape.dims.size(); i++) {
            if (i < diff) {
              strides.push_back(0);
            } else {
              const auto& dim = binding.shape.dims[i - diff];
              auto stride = dim.stride;
              if (dim.size == 1) {
                stride = 0;
              }
              strides.push_back(stride);
            }
          }
          kernel->refs.emplace_back(Refinement{
              RefDir::In,                // dir
              input,                     // from
              input,                     // into
              BufferAccess{0, strides},  // access
          });
          // LOAD
          kernel->stmts.push_back(std::make_shared<Load>(input, ScalarName(input)));
        } break;
        case Binding::ICONST:
          kernel->stmts.push_back(std::make_shared<Constant>(ScalarName(input), binding.iconst));
          break;
        case Binding::FCONST:
          kernel->stmts.push_back(std::make_shared<Constant>(ScalarName(input), binding.fconst));
          break;
        case Binding::TUPLE:
          throw std::runtime_error("Not implemented!");
          break;
      }
    }

    std::vector<int64_t> strides;
    for (const auto& dim : out_shape.dims) {
      strides.push_back(dim.stride);
    }

    kernel->refs.emplace_back(Refinement{
        RefDir::Out,              // dir
        op.output,                // from
        op.output,                // into
        BufferAccess{0, strides}  // access
    });

    // INTRINSIC
    std::vector<std::string> scalar_inputs;
    for (const auto& input : op.inputs) {
      scalar_inputs.push_back(ScalarName(input));
    }
    AddIntrinsic(kernel.get(), op.f.fn, scalar_inputs, {ScalarName(op.output)});

    // STORE
    kernel->stmts.push_back(std::make_shared<Store>(ScalarName(op.output), op.output));
  }

  void ProcessSpecial(Block* main, const Op& op) {
    auto stmt = std::make_shared<Special>();
    stmt->name = op.f.fn;
    stmt->params = op.f.params;
    stmt->inputs = op.inputs;
    stmt->outputs = {op.output};
    main->stmts.push_back(stmt);
  }

  std::shared_ptr<Block> AddKernel(Block* parent, const char* prefix = "") {
    auto block = std::make_shared<Block>();
    block->name = printstring("%skernel_%zu", prefix, parent->stmts.size());
    parent->stmts.push_back(block);
    return block;
  }

  std::vector<TensorShape> MakeTShapes(const Contraction& con) {
    std::vector<TensorShape> tshapes;
    for (const TensorSpec& spec : con.specs) {
      auto it = vars_.find(spec.id);
      if (it == vars_.end()) {
        IVLOG(1, "Something went wrong: " << vars_);
        throw std::runtime_error(printstring("Unable to find tensor shape for id %s, ug", spec.id.c_str()));
      }
      tshapes.push_back(it->second.shape);
    }
    return tshapes;
  }

  bool NeedsZero(const FlatContraction& flat) {
    std::vector<std::pair<size_t, size_t>> out_pattern;
    if (flat.access[0].offset != 0) {
      return true;
    }
    for (size_t i = 0; i < flat.names.size(); i++) {
      if (flat.access[0].strides[i] == 0) {
        continue;
      }
      if (flat.access[0].strides[i] < 0) {
        return true;
      }  // Don't try to be fancy, fallback
      out_pattern.emplace_back(flat.access[0].strides[i], flat.ranges[i]);
    }
    for (const FlatConstraint& fc : flat.constraints) {
      bool output_only = true;
      for (size_t i = 0; i < flat.names.size(); i++) {
        if (fc.lhs[i] != 0 && flat.access[0].strides[i] == 0) {
          output_only = false;
          break;
        }
      }
      if (output_only) {
        return true;
      }
    }
    std::sort(out_pattern.begin(), out_pattern.end());
    size_t curskip = 1;
    for (const auto& p : out_pattern) {
      if (curskip != p.first) {
        return true;
      }
      curskip *= p.second;
    }
    return curskip != flat.access[0].global_index_limit;
  }

  void AddIntrinsic(Block* block,                            //
                    const std::string& name,                 //
                    const std::vector<std::string>& inputs,  //
                    const std::vector<std::string>& outputs) {
    auto stmt = std::make_shared<Intrinsic>();
    stmt->name = name;
    stmt->inputs = inputs;
    stmt->outputs = outputs;
    block->stmts.push_back(stmt);
  }

  inline std::string ScalarName(const std::string& name) {  //
    return printstring("$%s", name.c_str());
  }

 private:
  Program parsed_;
  Bindings vars_;
  ShapeMap inputs_;
  ShapeMap outputs_;
  std::set<std::string> externals_;
  std::map<std::string, TensorShape> tmps_;
  std::map<std::string, Refinement*> ref_outs_;
};

}  // namespace

Block GenerateStripe(const RunInfo& runinfo) {
  Parser parser;
  auto parsed = parser.Parse(runinfo.code);
  auto vars = BindProgram(&parsed, runinfo.input_shapes, runinfo.output_shapes);
  StripeGenerator gen(parsed, vars, runinfo.input_shapes, runinfo.output_shapes);
  return gen.Run(runinfo.program_name);
}

}  // namespace lang
}  // namespace tile
}  // namespace vertexai
