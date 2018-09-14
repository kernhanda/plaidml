#include "tile/lang/gen_stripe.h"

#include "tile/lang/compile.h"

namespace vertexai {
namespace tile {
namespace lang {

using stripe::proto::Intrinsic;

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

  stripe::proto::Block Run(const std::string& name) {
    LOG(INFO) << "Compiling " << parsed_.ops.size() << " ops";
    // The top level block represents a program.
    // A single inner block of the program respresents the entry point to the program.
    // Declarations made on the program relate to user supplied inputs and outputs.
    // Declarations made on main relate to temporaries needed for communication between kernels.
    // The list of kernels to execute are the list of blocks defined within main.
    stripe::proto::Block program;
    program.set_name(name);
    auto stmt = program.add_stmts();
    auto main = stmt->mutable_block();
    main->set_name("main");
    // Add decls for external inputs/outputs
    AddDecls(&program, main, inputs_, true);
    AddDecls(&program, main, outputs_, false);
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
          ProcessContraction(main, op);
          break;
        case Op::FUNCTION:
          if (op.f.is_special()) {
            ProcessSpecial(main, op);
          } else if (op.f.fn == "reshape") {
            ProcessReshape(main, op);
          } else {
            ProcessElementwise(main, op);
          }
          break;
        case Op::CONSTANT:
          // LOG(INFO) << "CONSTANT";
          break;
      }
    }
    // Add decls for temporaries that are not aliased
    for (const auto& tmp : tmps_) {
      AddDecl(main, tmp.first, tmp.second);
    }
    IVLOG(2, "Done");
    return program;
  }

 private:
  void AddDecls(stripe::proto::Block* program, stripe::proto::Block* main, const ShapeMap& shapes, bool is_input) {
    for (const auto& item : shapes) {
      externals_.insert(item.first);
      AddDecl(program, item.first, item.second);
      if (is_input) {
        auto input = main->add_ref_ins();
        input->set_into(item.first);
        input->set_from(item.first);
      } else {
        auto output = main->add_ref_outs();
        output->set_into(item.first);
        output->set_from(item.first);
        output->set_agg_op(Intrinsic::Value_Name(Intrinsic::ASSIGN));
        ref_outs_.insert(std::make_pair(item.first, output));
      }
    }
  }

  void AddDecl(stripe::proto::Block* block, const std::string& name, const TensorShape& shape) {
    auto decl = block->add_decls();
    decl->set_name(name);
    auto pb_shape = decl->mutable_shape();
    pb_shape->set_type(static_cast<shape::proto::TensorShape::DataType>(shape.type));
    for (const auto& src : shape.dims) {
      auto dst = pb_shape->add_dimensions();
      dst->set_size(src.size);
      dst->set_stride(src.stride);
    }
  }

  void ProcessReshape(stripe::proto::Block* main, const Op& op) {
    auto it = ref_outs_.find(op.output);
    if (it != ref_outs_.end()) {
      // Remove the temporary
      // tmps_.erase(op.inputs[0]);
      it->second->set_from(op.inputs[0]);
      // Add an alias
      auto alias = main->add_ref_ins();
      alias->set_into(op.inputs[0]);
      alias->set_from(op.output);
    } else {
      // Remove the temporary
      // tmps_.erase(op.output);
      // Add an alias
      auto alias = main->add_ref_ins();
      alias->set_into(op.output);
      alias->set_from(op.inputs[0]);
    }
  }

  void ProcessContraction(stripe::proto::Block* main, const Op& op) {
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
        special_op.f.fn = Intrinsic::Value_Name(Intrinsic::ZERO);
        special_op.inputs.push_back(op.output);
      } else {
        special_op.f.fn = Intrinsic::Value_Name(Intrinsic::COPY);
        special_op.inputs.push_back(op.c.use_default);
      }
      ProcessSpecial(main, special_op);
    }

    auto kernel = AddKernel(main);
    kernel->set_comments(to_string(op));
    assert(flat.names.size() == flat.ranges.size());
    for (int i = 0; i < flat.names.size(); i++) {
      auto idx = kernel->add_idxs();
      idx->set_name(flat.names[i]);
      idx->set_range(flat.ranges[i]);
      idx->set_factor(0);
    }
    for (const auto& constraint : flat.constraints) {
      auto out = kernel->add_constraints();
      for (const auto& lhs : constraint.lhs) {
        out->add_lhs(lhs);
      }
      out->set_rhs(constraint.rhs);
    }

    std::vector<std::string> scalar_inputs;
    for (size_t i = 1; i < flat.inputs.size(); i++) {
      auto scalar_name = ScalarName(flat.inputs[i]);
      scalar_inputs.push_back(scalar_name);

      auto input = kernel->add_ref_ins();
      input->set_into(flat.inputs[i]);
      input->set_from(flat.inputs[i]);
      IntoAccess(flat.access[i], input->mutable_access());

      // LOAD
      auto stmt = kernel->add_stmts()->mutable_load();
      stmt->set_from(flat.inputs[i]);
      stmt->set_into(scalar_name);
    }

    // Combination Op
    if (kernel->ref_ins_size() > 1) {
      switch (flat.comb_op) {
        case CombinationOp::NONE:
          break;
        case CombinationOp::MULTIPLY:
          AddIntrinsic(kernel, Intrinsic::Value_Name(Intrinsic::MUL), scalar_inputs, {ScalarName(flat.output)});
          break;
        case CombinationOp::PLUS:
          AddIntrinsic(kernel, Intrinsic::Value_Name(Intrinsic::ADD), scalar_inputs, {ScalarName(flat.output)});
          break;
        case CombinationOp::EQ:
          AddIntrinsic(kernel, Intrinsic::Value_Name(Intrinsic::EQ), scalar_inputs, {ScalarName(flat.output)});
          break;
        case CombinationOp::COND:
          AddIntrinsic(kernel, Intrinsic::Value_Name(Intrinsic::COND), scalar_inputs, {ScalarName(flat.output)});
          break;
      }
    }

    auto output = kernel->add_ref_outs();
    output->set_into(flat.output);
    output->set_from(flat.output);
    switch (flat.agg_op) {
      case AggregationOp::SUM:
        output->set_agg_op(Intrinsic::Value_Name(Intrinsic::SUM));
        break;
      case AggregationOp::MAX:
        output->set_agg_op(Intrinsic::Value_Name(Intrinsic::MAX));
        break;
      case AggregationOp::MIN:
        output->set_agg_op(Intrinsic::Value_Name(Intrinsic::MIN));
        break;
      case AggregationOp::PROD:
        output->set_agg_op(Intrinsic::Value_Name(Intrinsic::PROD));
        break;
      case AggregationOp::ASSIGN:
        output->set_agg_op(Intrinsic::Value_Name(Intrinsic::ASSIGN));
        break;
      case AggregationOp::NONE:
        break;
    }
    IntoAccess(flat.access[0], output->mutable_access());

    // STORE
    auto stmt = kernel->add_stmts()->mutable_store();
    stmt->set_from(ScalarName(flat.output));
    stmt->set_into(flat.output);
  }

  void ProcessElementwise(stripe::proto::Block* main, const Op& op) {
    auto kernel = AddKernel(main);
    kernel->set_comments(to_string(op));

    const TensorShape& out_shape = vars_.at(op.output).shape;
    for (std::size_t i = 0; i < out_shape.dims.size(); ++i) {
      auto idx = kernel->add_idxs();
      idx->set_name(printstring("i%zu", i + 1));
      idx->set_range(out_shape.dims[i].size);
      idx->set_factor(0);
    }

    for (const auto& input : op.inputs) {
      const auto& binding = vars_.at(input);
      IVLOG(2, "  " << input << ": " << binding);
      switch (binding.tag) {
        case Binding::TENSOR: {
          auto ref_in = kernel->add_ref_ins();
          ref_in->set_into(input);
          ref_in->set_from(input);
          auto access = ref_in->mutable_access();
          // Be careful to handle broadcasts
          int diff = out_shape.dims.size() - binding.shape.dims.size();
          for (int i = 0; i < out_shape.dims.size(); i++) {
            if (i < diff) {
              access->add_strides(0);
            } else {
              const auto& dim = binding.shape.dims[i - diff];
              auto stride = dim.stride;
              if (dim.size == 1) {
                stride = 0;
              }
              access->add_strides(stride);
            }
          }
          // LOAD
          auto stmt_load = kernel->add_stmts()->mutable_load();
          stmt_load->set_from(input);
          stmt_load->set_into(ScalarName(input));
        } break;
        case Binding::ICONST: {
          auto stmt_const = kernel->add_stmts()->mutable_constant();
          stmt_const->set_name(ScalarName(input));
          stmt_const->set_iconst(binding.iconst);
        } break;
        case Binding::FCONST: {
          auto stmt_const = kernel->add_stmts()->mutable_constant();
          stmt_const->set_name(ScalarName(input));
          stmt_const->set_fconst(binding.fconst);
        } break;
        case Binding::TUPLE:
          throw std::runtime_error("Not implemented!");
          break;
      }
    }

    auto ref_out = kernel->add_ref_outs();
    ref_out->set_into(op.output);
    ref_out->set_from(op.output);
    auto access = ref_out->mutable_access();
    for (const auto& dim : out_shape.dims) {
      access->add_strides(dim.stride);
    }

    // INTRINSIC
    auto stmt_core = kernel->add_stmts()->mutable_intrinsic();
    stmt_core->set_name(op.f.fn);
    for (const auto& input : op.inputs) {
      stmt_core->add_inputs(ScalarName(input));
    }
    stmt_core->add_outputs(ScalarName(op.output));

    // STORE
    auto stmt_store = kernel->add_stmts()->mutable_store();
    stmt_store->set_from(ScalarName(op.output));
    stmt_store->set_into(op.output);
  }

  void ProcessSpecial(stripe::proto::Block* main, const Op& op) {
    auto stmt = main->add_stmts()->mutable_special();
    stmt->set_name(op.f.fn);
    for (const auto& input : op.inputs) {
      stmt->add_inputs(input);
    }
    for (const auto& param : op.f.params) {
      stmt->add_params(param);
    }
    stmt->add_outputs(op.output);
  }

  stripe::proto::Block* AddKernel(stripe::proto::Block* parent, const char* prefix = "") {
    auto name = printstring("%skernel_%zu", prefix, parent->stmts_size());
    auto stmt = parent->add_stmts();
    auto block = stmt->mutable_block();
    block->set_name(name);
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

  void IntoAccess(const FlatTensorAccess& src, stripe::proto::BufferAccess* dst) {
    dst->set_offset(src.offset);
    for (const auto& stride : src.strides) {
      dst->add_strides(stride);
    }
  }

  void AddIntrinsic(stripe::proto::Block* block,             //
                    const std::string& name,                 //
                    const std::vector<std::string>& inputs,  //
                    const std::vector<std::string>& outputs) {
    auto stmt = block->add_stmts()->mutable_intrinsic();
    stmt->set_name(name);
    for (const auto& input : inputs) {
      stmt->add_inputs(input);
    }
    for (const auto& output : outputs) {
      stmt->add_outputs(output);
    }
  }

  inline std::string ScalarName(const std::string& name) { return printstring("$%s", name.c_str()); }

 private:
  Program parsed_;
  Bindings vars_;
  ShapeMap inputs_;
  ShapeMap outputs_;
  std::set<std::string> externals_;
  std::map<std::string, TensorShape> tmps_;
  std::map<std::string, stripe::proto::RefineOut*> ref_outs_;
};

}  // namespace

stripe::proto::Block GenerateStripe(const RunInfo& runinfo) {
  Parser parser;
  auto parsed = parser.Parse(runinfo.code);
  auto vars = BindProgram(&parsed, runinfo.input_shapes, runinfo.output_shapes);
  StripeGenerator gen(parsed, vars, runinfo.input_shapes, runinfo.output_shapes);
  return gen.Run(runinfo.program_name);
}

}  // namespace lang
}  // namespace tile
}  // namespace vertexai
