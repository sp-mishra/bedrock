#include "catch_amalgamated.hpp"

#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/backends/lithe_codegen_asmjit.hpp"

using namespace lithe::codegen;
using namespace lithe::codegen::backends;

// ===========================================================================
// MIR builder helpers
// ===========================================================================
namespace {
    allocated_operand preg_op(std::uint16_t id, std::string name = "") {
        preg r;
        r.id = id;
        r.name = std::move(name);
        return allocated_operand::as_preg(r);
    }

    allocated_operand arg_op(std::uint32_t idx) {
        return allocated_operand::as_argument_index(idx);
    }

    allocated_operand imm_op(std::int64_t val) {
        return allocated_operand::as_i64(val);
    }

    allocated_operand block_op(std::uint32_t bid) {
        return allocated_operand::as_block(bid);
    }

    // Emit one instruction into a block.
    allocated_instruction make_inst(std::uint32_t id,
                                    opcode op,
                                    std::vector<allocated_operand> defs,
                                    std::vector<allocated_operand> uses) {
        allocated_instruction i;
        i.id = id;
        i.op = op;
        i.defs = std::move(defs);
        i.uses = std::move(uses);
        return i;
    }

    allocated_basic_block make_block(std::uint32_t id,
                                     std::string name,
                                     std::vector<allocated_instruction> insts) {
        allocated_basic_block bb;
        bb.id = id;
        bb.name = std::move(name);
        bb.instructions = std::move(insts);
        return bb;
    }

    mir::physical_mir_function wrap(std::string name,
                                    std::vector<allocated_basic_block> blocks,
                                    std::uint32_t entry = 1) {
        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = entry;
        fn.blocks = std::move(blocks);

        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }

    // -------------------------------------------------------------------------
    // JIT a function, assert no diagnostics, return a shared handle.
    // Ownership of the JIT runtime is shared via artifact_handle.
    // -------------------------------------------------------------------------
    std::shared_ptr<jit_function_handle> jit(const mir::physical_mir_function& fn) {
        asmjit_backend backend;
        auto art = backend.emit(fn);
        REQUIRE(art.diagnostics.empty());
        REQUIRE(art.kind == artifact_kind::jit_function);
        auto* h_ptr = asmjit_backend::get_handle(art);
        REQUIRE(h_ptr != nullptr);
        REQUIRE(h_ptr->valid());
        // Aliasing constructor: share lifetime with artifact_handle, expose the
        // typed pointer. The JIT runtime stays alive as long as this shared_ptr lives.
        return std::shared_ptr<jit_function_handle>(art.handle, h_ptr);
    }

    // =========================================================================
    // Single-block function builders
    // =========================================================================

    // int64_t add(a, b) { return a + b; }
    mir::physical_mir_function fn_add() {
        return wrap("add", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::add, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t sub(a, b) { return a - b; }
    mir::physical_mir_function fn_sub() {
        return wrap("sub", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::sub, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t mul(a, b) { return a * b; }
    mir::physical_mir_function fn_mul() {
        return wrap("mul", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::mul, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t div_fn(a, b) { return a / b; }  (returns 0 when b==0)
    mir::physical_mir_function fn_div() {
        return wrap("div_fn", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::div, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t mod_fn(a, b) { return a % b; }  (returns 0 when b==0)
    mir::physical_mir_function fn_mod() {
        return wrap("mod_fn", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::mod, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t neg_fn(a, _) { return -a; }
    mir::physical_mir_function fn_neg() {
        return wrap("neg_fn", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::neg, {preg_op(1)}, {preg_op(0)}),
                                       make_inst(3, opcode::ret, {}, {preg_op(1)}),
                                   })
                    });
    }

    // int64_t add_imm(a, _) { return a + 100; }
    mir::physical_mir_function fn_add_imm() {
        return wrap("add_imm", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_imm, {preg_op(1)}, {imm_op(100)}),
                                       make_inst(3, opcode::add, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t bitwise_and(a, b) { return a & b; }
    mir::physical_mir_function fn_bit_and() {
        return wrap("bit_and", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::bit_and, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t bitwise_or(a, b) { return a | b; }
    mir::physical_mir_function fn_bit_or() {
        return wrap("bit_or", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::bit_or, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t bitwise_xor(a, b) { return a ^ b; }
    mir::physical_mir_function fn_bit_xor() {
        return wrap("bit_xor", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::bit_xor, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t bitwise_not(a, _) { return ~a; }
    mir::physical_mir_function fn_bit_not() {
        return wrap("bit_not", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::bit_not, {preg_op(1)}, {preg_op(0)}),
                                       make_inst(3, opcode::ret, {}, {preg_op(1)}),
                                   })
                    });
    }

    // int64_t shl_fn(a, b) { return a << b; }
    mir::physical_mir_function fn_shl() {
        return wrap("shl_fn", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::shl, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t shr_fn(a, b) { return a >> b; }  (arithmetic shift)
    mir::physical_mir_function fn_shr() {
        return wrap("shr_fn", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::shr, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t cmp_eq(a, b) { return a == b ? 1 : 0; }
    mir::physical_mir_function fn_cmp_eq() {
        return wrap("cmp_eq", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::cmp_eq, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t cmp_lt(a, b) { return a < b ? 1 : 0; }
    mir::physical_mir_function fn_cmp_lt() {
        return wrap("cmp_lt", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::cmp_lt, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t logical_and(a, b) { return (a != 0 && b != 0) ? 1 : 0; }
    mir::physical_mir_function fn_logical_and() {
        return wrap("logical_and", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::logical_and, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // int64_t logical_not(a, _) { return (a == 0) ? 1 : 0; }
    mir::physical_mir_function fn_logical_not() {
        return wrap("logical_not", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::logical_not, {preg_op(1)}, {preg_op(0)}),
                                       make_inst(3, opcode::ret, {}, {preg_op(1)}),
                                   })
                    });
    }

    // =========================================================================
    // Multi-block function builders — control flow patterns for Go/Java
    // =========================================================================

    // if/else pattern:
    //   int64_t abs(a, _) { return a < 0 ? -a : a; }
    //
    // BB layout:
    //   bb1 (entry): r0=load_arg[0]; r1=cmp_lt(r0, 0); branch_cond r1 -> bb2, bb3
    //   bb2 (negative): r2=neg(r0);  branch -> bb4
    //   bb3 (positive): r3=mov(r0);  branch -> bb4
    //   bb4 (merge):    r4=nop (phi would go here, we use the two branches directly)
    //
    // Since we don't have phi nodes in physical MIR, we write to the same preg
    // in both branches and read it in the merge block.
    mir::physical_mir_function fn_abs() {
        //  bb1: entry
        //    r0 = load_arg[0]
        //    r1 = load_imm 0
        //    r2 = cmp_lt r0, r1
        //    branch_cond r2 -> bb2, bb3
        //  bb2: negative
        //    r3 = neg r0
        //    branch -> bb4
        //  bb3: non_negative
        //    r3 = mov r0
        //    branch -> bb4
        //  bb4: merge
        //    ret r3

        auto bb1 = make_block(1, "entry", {
                                  make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                  make_inst(2, opcode::load_imm, {preg_op(10)}, {imm_op(0)}),
                                  make_inst(3, opcode::cmp_lt, {preg_op(1)}, {preg_op(0), preg_op(10)}),
                                  make_inst(4, opcode::branch_cond, {}, {preg_op(1), block_op(2), block_op(3)}),
                              });

        auto bb2 = make_block(2, "negative", {
                                  make_inst(5, opcode::neg, {preg_op(3)}, {preg_op(0)}),
                                  make_inst(6, opcode::branch, {}, {block_op(4)}),
                              });

        auto bb3 = make_block(3, "non_negative", {
                                  make_inst(7, opcode::mov, {preg_op(3)}, {preg_op(0)}),
                                  make_inst(8, opcode::branch, {}, {block_op(4)}),
                              });

        auto bb4 = make_block(4, "merge", {
                                  make_inst(9, opcode::ret, {}, {preg_op(3)}),
                              });

        return wrap("abs", {bb1, bb2, bb3, bb4});
    }

    // Countdown loop (Go-style):
    //   int64_t sum_1_to_n(n, _) {
    //     int64_t acc = 0, i = n;
    //     while (i > 0) { acc += i; i--; }
    //     return acc;
    //   }
    //
    // BB layout:
    //   bb1 (init):    acc=0; i=load_arg[0]; branch -> bb2
    //   bb2 (header):  cmp_gt i, 0; branch_cond -> bb3, bb4
    //   bb3 (body):    acc=add(acc,i); i=sub(i,1); branch -> bb2
    //   bb4 (exit):    ret acc
    mir::physical_mir_function fn_sum_1_to_n() {
        // Register assignment:
        //   r0 = n (argument)
        //   r1 = acc  (accumulator, init 0)
        //   r2 = i    (loop counter, init n)
        //   r3 = zero (constant 0 for cmp)
        //   r4 = one  (constant 1 for decrement)
        //   r5 = cmp result

        auto bb1 = make_block(1, "init", {
                                  make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                  make_inst(2, opcode::load_imm, {preg_op(1)}, {imm_op(0)}), // acc = 0
                                  make_inst(3, opcode::mov, {preg_op(2)}, {preg_op(0)}), // i = n
                                  make_inst(4, opcode::branch, {}, {block_op(2)}),
                              });

        auto bb2 = make_block(2, "header", {
                                  make_inst(5, opcode::load_imm, {preg_op(3)}, {imm_op(0)}),
                                  make_inst(6, opcode::cmp_gt, {preg_op(5)}, {preg_op(2), preg_op(3)}), // i > 0
                                  make_inst(7, opcode::branch_cond, {}, {preg_op(5), block_op(3), block_op(4)}),
                              });

        auto bb3 = make_block(3, "body", {
                                  make_inst(8, opcode::add, {preg_op(1)}, {preg_op(1), preg_op(2)}), // acc += i
                                  make_inst(9, opcode::load_imm, {preg_op(4)}, {imm_op(1)}),
                                  make_inst(10, opcode::sub, {preg_op(2)}, {preg_op(2), preg_op(4)}), // i -= 1
                                  make_inst(11, opcode::branch, {}, {block_op(2)}),
                              });

        auto bb4 = make_block(4, "exit", {
                                  make_inst(12, opcode::ret, {}, {preg_op(1)}),
                              });

        return wrap("sum_1_to_n", {bb1, bb2, bb3, bb4});
    }

    // Fibonacci (Java-style iterative):
    //   int64_t fib(n, _) {
    //     if (n <= 1) return n;
    //     int64_t a=0, b=1, tmp;
    //     for (int64_t i=2; i<=n; i++) { tmp=a+b; a=b; b=tmp; }
    //     return b;
    //   }
    //
    // BB layout:
    //   bb1 (init):    r0=n; cmp_le(r0,1) -> bb2 (base), bb3 (loop_init)
    //   bb2 (base):    ret r0
    //   bb3 (loop_init): a=0; b=1; i=2; branch -> bb4
    //   bb4 (header):  cmp_le(i, n) -> bb5 (body), bb6 (done)
    //   bb5 (body):    tmp=a+b; a=b; b=tmp; i=i+1; branch -> bb4
    //   bb6 (done):    ret b
    mir::physical_mir_function fn_fib() {
        // Registers: r0=n, r1=a, r2=b, r3=tmp, r4=i, r5=cmp, r6=one, r7=two, r8=const1

        auto bb1 = make_block(1, "init", {
                                  make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                  make_inst(2, opcode::load_imm, {preg_op(8)}, {imm_op(1)}),
                                  make_inst(3, opcode::cmp_le, {preg_op(5)}, {preg_op(0), preg_op(8)}), // n<=1
                                  make_inst(4, opcode::branch_cond, {}, {preg_op(5), block_op(2), block_op(3)}),
                              });

        auto bb2 = make_block(2, "base", {
                                  make_inst(5, opcode::ret, {}, {preg_op(0)}),
                              });

        auto bb3 = make_block(3, "loop_init", {
                                  make_inst(6, opcode::load_imm, {preg_op(1)}, {imm_op(0)}), // a = 0
                                  make_inst(7, opcode::load_imm, {preg_op(2)}, {imm_op(1)}), // b = 1
                                  make_inst(8, opcode::load_imm, {preg_op(4)}, {imm_op(2)}), // i = 2
                                  make_inst(9, opcode::branch, {}, {block_op(4)}),
                              });

        auto bb4 = make_block(4, "header", {
                                  make_inst(10, opcode::cmp_le, {preg_op(5)}, {preg_op(4), preg_op(0)}), // i<=n
                                  make_inst(11, opcode::branch_cond, {}, {preg_op(5), block_op(5), block_op(6)}),
                              });

        auto bb5 = make_block(5, "body", {
                                  make_inst(12, opcode::add, {preg_op(3)}, {preg_op(1), preg_op(2)}), // tmp=a+b
                                  make_inst(13, opcode::mov, {preg_op(1)}, {preg_op(2)}), // a=b
                                  make_inst(14, opcode::mov, {preg_op(2)}, {preg_op(3)}), // b=tmp
                                  make_inst(15, opcode::load_imm, {preg_op(6)}, {imm_op(1)}),
                                  make_inst(16, opcode::add, {preg_op(4)}, {preg_op(4), preg_op(6)}), // i++
                                  make_inst(17, opcode::branch, {}, {block_op(4)}),
                              });

        auto bb6 = make_block(6, "done", {
                                  make_inst(18, opcode::ret, {}, {preg_op(2)}),
                              });

        return wrap("fib", {bb1, bb2, bb3, bb4, bb5, bb6});
    }

    // GCD (Go/Java euclidean algorithm):
    //   int64_t gcd(a, b) {
    //     while (b != 0) { int64_t t = a % b; a = b; b = t; }
    //     return a;
    //   }
    //
    // BB layout:
    //   bb1 (init):    r0=a; r1=b; branch -> bb2
    //   bb2 (header):  cmp_ne(r1,0) -> bb3 (body), bb4 (done)
    //   bb3 (body):    r2=mod(r0,r1); r0=r1; r1=r2; branch -> bb2
    //   bb4 (done):    ret r0
    mir::physical_mir_function fn_gcd() {
        auto bb1 = make_block(1, "init", {
                                  make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                  make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                  make_inst(3, opcode::branch, {}, {block_op(2)}),
                              });

        auto bb2 = make_block(2, "header", {
                                  make_inst(4, opcode::load_imm, {preg_op(5)}, {imm_op(0)}),
                                  make_inst(5, opcode::cmp_ne, {preg_op(3)}, {preg_op(1), preg_op(5)}), // b!=0
                                  make_inst(6, opcode::branch_cond, {}, {preg_op(3), block_op(3), block_op(4)}),
                              });

        auto bb3 = make_block(3, "body", {
                                  make_inst(7, opcode::mod, {preg_op(2)}, {preg_op(0), preg_op(1)}), // t = a%b
                                  make_inst(8, opcode::mov, {preg_op(0)}, {preg_op(1)}), // a = b
                                  make_inst(9, opcode::mov, {preg_op(1)}, {preg_op(2)}), // b = t
                                  make_inst(10, opcode::branch, {}, {block_op(2)}),
                              });

        auto bb4 = make_block(4, "done", {
                                  make_inst(11, opcode::ret, {}, {preg_op(0)}),
                              });

        return wrap("gcd", {bb1, bb2, bb3, bb4});
    }

    // Chained arithmetic:
    //   int64_t poly(a, b) { return a*a + 2*a*b + b*b; }  (= (a+b)^2)
    mir::physical_mir_function fn_poly() {
        return wrap("poly", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}), // a
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}), // b
                                       make_inst(3, opcode::mul, {preg_op(2)}, {preg_op(0), preg_op(0)}), // a*a
                                       make_inst(4, opcode::load_imm, {preg_op(9)}, {imm_op(2)}),
                                       make_inst(5, opcode::mul, {preg_op(3)}, {preg_op(9), preg_op(0)}), // 2*a
                                       make_inst(6, opcode::mul, {preg_op(4)}, {preg_op(3), preg_op(1)}), // 2*a*b
                                       make_inst(7, opcode::mul, {preg_op(5)}, {preg_op(1), preg_op(1)}), // b*b
                                       make_inst(8, opcode::add, {preg_op(6)}, {preg_op(2), preg_op(4)}), // a*a + 2*a*b
                                       make_inst(9, opcode::add, {preg_op(7)}, {preg_op(6), preg_op(5)}), // + b*b
                                       make_inst(10, opcode::ret, {}, {preg_op(7)}),
                                   })
                    });
    }

    // Ternary max via comparisons (Go idiom):
    //   int64_t max(a, b) { return a >= b ? a : b; }
    mir::physical_mir_function fn_max() {
        //  bb1: r0=a; r1=b; r2=cmp_ge(r0,r1); branch_cond r2 -> bb2, bb3
        //  bb2: r3=mov(r0); branch -> bb4
        //  bb3: r3=mov(r1); branch -> bb4
        //  bb4: ret r3

        auto bb1 = make_block(1, "entry", {
                                  make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                  make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                  make_inst(3, opcode::cmp_ge, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                  make_inst(4, opcode::branch_cond, {}, {preg_op(2), block_op(2), block_op(3)}),
                              });
        auto bb2 = make_block(2, "take_a", {
                                  make_inst(5, opcode::mov, {preg_op(3)}, {preg_op(0)}),
                                  make_inst(6, opcode::branch, {}, {block_op(4)}),
                              });
        auto bb3 = make_block(3, "take_b", {
                                  make_inst(7, opcode::mov, {preg_op(3)}, {preg_op(1)}),
                                  make_inst(8, opcode::branch, {}, {block_op(4)}),
                              });
        auto bb4 = make_block(4, "done", {
                                  make_inst(9, opcode::ret, {}, {preg_op(3)}),
                              });
        return wrap("max", {bb1, bb2, bb3, bb4});
    }

    // Collatz steps counter (loop with complex conditional — Go-style):
    //   int64_t collatz(n, _) {
    //     int64_t steps = 0;
    //     while (n != 1) {
    //       if (n % 2 == 0) n = n / 2; else n = 3*n + 1;
    //       steps++;
    //     }
    //     return steps;
    //   }
    //
    // BB layout:
    //   bb1 (init):     r0=n; r1=steps=0; branch -> bb2
    //   bb2 (while_cmp): cmp_ne(r0,1) -> bb3, bb7
    //   bb3 (mod_check): r2=mod(r0,2); cmp_eq(r2,0) -> bb4, bb5
    //   bb4 (even):      r0=div(r0,2); branch -> bb6
    //   bb5 (odd):       r3=3*r0; r0=r3+1; branch -> bb6
    //   bb6 (incr):      r1=r1+1; branch -> bb2
    //   bb7 (done):      ret r1
    mir::physical_mir_function fn_collatz() {
        auto bb1 = make_block(1, "init", {
                                  make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                  make_inst(2, opcode::load_imm, {preg_op(1)}, {imm_op(0)}), // steps=0
                                  make_inst(3, opcode::branch, {}, {block_op(2)}),
                              });

        auto bb2 = make_block(2, "while_cmp", {
                                  make_inst(4, opcode::load_imm, {preg_op(9)}, {imm_op(1)}),
                                  make_inst(5, opcode::cmp_ne, {preg_op(5)}, {preg_op(0), preg_op(9)}),
                                  make_inst(6, opcode::branch_cond, {}, {preg_op(5), block_op(3), block_op(7)}),
                              });

        auto bb3 = make_block(3, "mod_check", {
                                  make_inst(7, opcode::load_imm, {preg_op(8)}, {imm_op(2)}),
                                  make_inst(8, opcode::mod, {preg_op(2)}, {preg_op(0), preg_op(8)}), // n%2
                                  make_inst(9, opcode::load_imm, {preg_op(7)}, {imm_op(0)}),
                                  make_inst(10, opcode::cmp_eq, {preg_op(3)}, {preg_op(2), preg_op(7)}), // (n%2)==0
                                  make_inst(11, opcode::branch_cond, {}, {preg_op(3), block_op(4), block_op(5)}),
                              });

        auto bb4 = make_block(4, "even", {
                                  make_inst(12, opcode::load_imm, {preg_op(6)}, {imm_op(2)}),
                                  make_inst(13, opcode::div, {preg_op(0)}, {preg_op(0), preg_op(6)}), // n=n/2
                                  make_inst(14, opcode::branch, {}, {block_op(6)}),
                              });

        auto bb5 = make_block(5, "odd", {
                                  make_inst(15, opcode::load_imm, {preg_op(10)}, {imm_op(3)}),
                                  make_inst(16, opcode::mul, {preg_op(4)}, {preg_op(10), preg_op(0)}), // 3*n
                                  make_inst(17, opcode::load_imm, {preg_op(11)}, {imm_op(1)}),
                                  make_inst(18, opcode::add, {preg_op(0)}, {preg_op(4), preg_op(11)}), // n=3n+1
                                  make_inst(19, opcode::branch, {}, {block_op(6)}),
                              });

        auto bb6 = make_block(6, "incr", {
                                  make_inst(20, opcode::load_imm, {preg_op(12)}, {imm_op(1)}),
                                  make_inst(21, opcode::add, {preg_op(1)}, {preg_op(1), preg_op(12)}), // steps++
                                  make_inst(22, opcode::branch, {}, {block_op(2)}),
                              });

        auto bb7 = make_block(7, "done", {
                                  make_inst(23, opcode::ret, {}, {preg_op(1)}),
                              });

        return wrap("collatz", {bb1, bb2, bb3, bb4, bb5, bb6, bb7});
    }

    // Popcount (Java-style bit manipulation):
    //   int64_t popcount(n, _) {
    //     int64_t count = 0;
    //     while (n != 0) { count += n & 1; n >>= 1; }
    //     return count;
    //   }
    //
    // BB layout:
    //   bb1 (init):   r0=n; r1=count=0; branch -> bb2
    //   bb2 (header): cmp_ne(r0,0) -> bb3, bb4
    //   bb3 (body):   r2=r0&1; r1=r1+r2; r0=r0>>1; branch -> bb2
    //   bb4 (done):   ret r1
    mir::physical_mir_function fn_popcount() {
        auto bb1 = make_block(1, "init", {
                                  make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                  make_inst(2, opcode::load_imm, {preg_op(1)}, {imm_op(0)}), // count=0
                                  make_inst(3, opcode::branch, {}, {block_op(2)}),
                              });

        auto bb2 = make_block(2, "header", {
                                  make_inst(4, opcode::load_imm, {preg_op(5)}, {imm_op(0)}),
                                  make_inst(5, opcode::cmp_ne, {preg_op(3)}, {preg_op(0), preg_op(5)}),
                                  make_inst(6, opcode::branch_cond, {}, {preg_op(3), block_op(3), block_op(4)}),
                              });

        auto bb3 = make_block(3, "body", {
                                  make_inst(7, opcode::load_imm, {preg_op(6)}, {imm_op(1)}),
                                  make_inst(8, opcode::bit_and, {preg_op(2)}, {preg_op(0), preg_op(6)}), // n&1
                                  make_inst(9, opcode::add, {preg_op(1)}, {preg_op(1), preg_op(2)}), // count+=n&1
                                  make_inst(10, opcode::load_imm, {preg_op(7)}, {imm_op(1)}),
                                  make_inst(11, opcode::shr, {preg_op(0)}, {preg_op(0), preg_op(7)}), // n>>=1
                                  make_inst(12, opcode::branch, {}, {block_op(2)}),
                              });

        auto bb4 = make_block(4, "done", {
                                  make_inst(13, opcode::ret, {}, {preg_op(1)}),
                              });

        return wrap("popcount", {bb1, bb2, bb3, bb4});
    }
} // namespace


// ===========================================================================
// CONCEPT / TRAIT TESTS
// ===========================================================================

TEST_CASE (



"asmjit_backend satisfies CodeEmissionTarget"
,
"[asmjit][concept]"
)
 {
    STATIC_REQUIRE(CodeEmissionTarget<asmjit_backend>);
}

TEST_CASE (



"asmjit_backend traits and capabilities"
,
"[asmjit][traits]"
)
 {
    const auto t = asmjit_backend::traits();
    REQUIRE(t.name == "asmjit_backend");
    REQUIRE(t.input_phase == target_input_phase::physical_mir);
    REQUIRE(t.produced_artifact == artifact_kind::jit_function);

    const auto caps = asmjit_backend::capabilities();
    CHECK(caps.has(backend_feature::integer_arithmetic));
    CHECK(caps.has(backend_feature::spill_load_store));
    CHECK(caps.has(backend_feature::branches));
    CHECK(caps.has(backend_feature::calls));
    CHECK(caps.has(backend_feature::memory_operands));
    CHECK(caps.has(backend_feature::stack_frame));
}

// ===========================================================================
// BASIC ARITHMETIC
// ===========================================================================

TEST_CASE (



"asmjit: add(a,b)"
,
"[asmjit][arithmetic]"
)
 {
    auto h = jit(fn_add());
    CHECK(h->call(3, 4) == 7);
    CHECK(h->call(0, 0) == 0);
    CHECK(h->call(-1, 1) == 0);
    CHECK(h->call(100, 200) == 300);
    CHECK(h->call(-50, -50) == -100);
}

TEST_CASE (



"asmjit: sub(a,b)"
,
"[asmjit][arithmetic]"
)
 {
    auto h = jit(fn_sub());
    CHECK(h->call(10, 4) == 6);
    CHECK(h->call(0, 5) == -5);
    CHECK(h->call(-3, -7) == 4);
}

TEST_CASE (



"asmjit: mul(a,b)"
,
"[asmjit][arithmetic]"
)
 {
    auto h = jit(fn_mul());
    CHECK(h->call(6, 7) == 42);
    CHECK(h->call(0, 99) == 0);
    CHECK(h->call(-3, 4) == -12);
    CHECK(h->call(-3, -4) == 12);
}

TEST_CASE (



"asmjit: div(a,b)"
,
"[asmjit][arithmetic]"
)
 {
    auto h = jit(fn_div());
    CHECK(h->call(10, 2) == 5);
    CHECK(h->call(7, 2) == 3);
    CHECK(h->call(-10, 2) == -5);
    CHECK(h->call(0, 5) == 0);
    CHECK(h->call(100, 10) == 10);
}

TEST_CASE (



"asmjit: div by zero guard"
,
"[asmjit][arithmetic][safety]"
)
 {
    auto h = jit(fn_div());
    // Must not crash; returns 0 per guard
    CHECK(h->call(42, 0) == 0);
    CHECK(h->call(-1, 0) == 0);
    CHECK(h->call(0, 0) == 0);
}

TEST_CASE (



"asmjit: mod(a,b)"
,
"[asmjit][arithmetic]"
)
 {
    auto h = jit(fn_mod());
    CHECK(h->call(10, 3) == 1);
    CHECK(h->call(9, 3) == 0);
    CHECK(h->call(7, 2) == 1);
    CHECK(h->call(-7, 2) == -1);
    CHECK(h->call(100, 7) == 2);
}

TEST_CASE (



"asmjit: mod by zero guard"
,
"[asmjit][arithmetic][safety]"
)
 {
    auto h = jit(fn_mod());
    CHECK(h->call(42, 0) == 0);
    CHECK(h->call(-1, 0) == 0);
}

TEST_CASE (



"asmjit: neg(a)"
,
"[asmjit][arithmetic]"
)
 {
    auto h = jit(fn_neg());
    CHECK(h->call(5, 0) == -5);
    CHECK(h->call(-5, 0) == 5);
    CHECK(h->call(0, 0) == 0);
}

TEST_CASE (



"asmjit: add_imm(a+100)"
,
"[asmjit][arithmetic]"
)
 {
    auto h = jit(fn_add_imm());
    CHECK(h->call(0, 0) == 100);
    CHECK(h->call(10, 0) == 110);
    CHECK(h->call(-100, 0) == 0);
}

// ===========================================================================
// BITWISE OPERATIONS
// ===========================================================================

TEST_CASE (



"asmjit: bitwise_and(a,b)"
,
"[asmjit][bitwise]"
)
 {
    auto h = jit(fn_bit_and());
    CHECK(h->call(0xFF, 0x0F) == 0x0F);
    CHECK(h->call(0xAA, 0x55) == 0);
    CHECK(h->call(-1, 0x123) == 0x123);
}

TEST_CASE (



"asmjit: bitwise_or(a,b)"
,
"[asmjit][bitwise]"
)
 {
    auto h = jit(fn_bit_or());
    CHECK(h->call(0xA0, 0x0B) == 0xAB);
    CHECK(h->call(0, 0) == 0);
    CHECK(h->call(0xFF, 0) == 0xFF);
}

TEST_CASE (



"asmjit: bitwise_xor(a,b)"
,
"[asmjit][bitwise]"
)
 {
    auto h = jit(fn_bit_xor());
    CHECK(h->call(0xFF, 0x0F) == 0xF0);
    CHECK(h->call(5, 5) == 0);
    CHECK(h->call(0, 0xFF) == 0xFF);
}

TEST_CASE (



"asmjit: bitwise_not(a)"
,
"[asmjit][bitwise]"
)
 {
    auto h = jit(fn_bit_not());
    CHECK(h->call(0, 0) == ~std::int64_t{0});
    CHECK(h->call(-1, 0) == 0);
    CHECK(h->call(1, 0) == ~std::int64_t{1});
}

TEST_CASE (



"asmjit: shl(a,b)"
,
"[asmjit][bitwise]"
)
 {
    auto h = jit(fn_shl());
    CHECK(h->call(1, 3) == 8);
    CHECK(h->call(1, 0) == 1);
    CHECK(h->call(3, 10) == 3072);
}

TEST_CASE (



"asmjit: shr(a,b) arithmetic"
,
"[asmjit][bitwise]"
)
 {
    auto h = jit(fn_shr());
    CHECK(h->call(8, 3) == 1);
    CHECK(h->call(-8, 1) == -4); // arithmetic right shift preserves sign
    CHECK(h->call(256, 4) == 16);
}

// ===========================================================================
// COMPARISON OPERATIONS
// ===========================================================================

TEST_CASE (



"asmjit: cmp_eq(a,b)"
,
"[asmjit][cmp]"
)
 {
    auto h = jit(fn_cmp_eq());
    CHECK(h->call(5, 5) == 1);
    CHECK(h->call(5, 4) == 0);
    CHECK(h->call(-1, -1) == 1);
    CHECK(h->call(0, 1) == 0);
}

TEST_CASE (



"asmjit: cmp_lt(a,b)"
,
"[asmjit][cmp]"
)
 {
    auto h = jit(fn_cmp_lt());
    CHECK(h->call(3, 5) == 1);
    CHECK(h->call(5, 3) == 0);
    CHECK(h->call(5, 5) == 0);
    CHECK(h->call(-1, 0) == 1);
}

TEST_CASE (



"asmjit: logical_and(a,b)"
,
"[asmjit][logical]"
)
 {
    auto h = jit(fn_logical_and());
    CHECK(h->call(1, 1) == 1);
    CHECK(h->call(1, 0) == 0);
    CHECK(h->call(0, 1) == 0);
    CHECK(h->call(0, 0) == 0);
    CHECK(h->call(5, 3) == 1);
}

TEST_CASE (



"asmjit: logical_not(a)"
,
"[asmjit][logical]"
)
 {
    auto h = jit(fn_logical_not());
    CHECK(h->call(0, 0) == 1);
    CHECK(h->call(1, 0) == 0);
    CHECK(h->call(-5, 0) == 0);
}

// ===========================================================================
// MULTI-BLOCK CONTROL FLOW — Go/Java patterns
// ===========================================================================

TEST_CASE (



"asmjit: abs(a) — if/else control flow"
,
"[asmjit][controlflow]"
)
 {
    auto h = jit(fn_abs());
    CHECK(h->call(5, 0) == 5);
    CHECK(h->call(-5, 0) == 5);
    CHECK(h->call(0, 0) == 0);
    CHECK(h->call(-100,0) == 100);
    CHECK(h->call(100, 0) == 100);
    CHECK(h->call(-1, 0) == 1);
}

TEST_CASE (



"asmjit: max(a,b) — comparison + select"
,
"[asmjit][controlflow]"
)
 {
    auto h = jit(fn_max());
    CHECK(h->call(5, 3) == 5);
    CHECK(h->call(3, 5) == 5);
    CHECK(h->call(5, 5) == 5);
    CHECK(h->call(-1, -2) == -1);
    CHECK(h->call(0, -1) == 0);
}

TEST_CASE (



"asmjit: sum_1_to_n(n) — countdown loop (Go-style)"
,
"[asmjit][loop]"
)
 {
    auto h = jit(fn_sum_1_to_n());
    CHECK(h->call(0, 0) == 0);
    CHECK(h->call(1, 0) == 1);
    CHECK(h->call(5, 0) == 15); // 1+2+3+4+5
    CHECK(h->call(10, 0) == 55); // triangular number
    CHECK(h->call(100,0) == 5050); // Gauss's formula
}

TEST_CASE (



"asmjit: fib(n) — iterative fibonacci (Java-style)"
,
"[asmjit][loop]"
)
 {
    auto h = jit(fn_fib());
    CHECK(h->call(0, 0) == 0);
    CHECK(h->call(1, 0) == 1);
    CHECK(h->call(2, 0) == 1);
    CHECK(h->call(5, 0) == 5);
    CHECK(h->call(10, 0) == 55);
    CHECK(h->call(20, 0) == 6765);
}

TEST_CASE (



"asmjit: gcd(a,b) — euclidean loop with mod (Go/Java)"
,
"[asmjit][loop]"
)
 {
    auto h = jit(fn_gcd());
    CHECK(h->call(48, 18) == 6);
    CHECK(h->call(100, 75) == 25);
    CHECK(h->call(7, 3) == 1);
    CHECK(h->call(12, 4) == 4);
    CHECK(h->call(0, 5) == 5); // gcd(0,b) = b
    CHECK(h->call(5, 0) == 5); // gcd(a,0) = a (loop doesn't execute)
}

TEST_CASE (



"asmjit: poly(a,b) — chained arithmetic (a+b)^2"
,
"[asmjit][arithmetic]"
)
 {
    auto h = jit(fn_poly());
    CHECK(h->call(0, 0) == 0);
    CHECK(h->call(1, 1) == 4); // (1+1)^2 = 4
    CHECK(h->call(2, 3) == 25); // (2+3)^2 = 25
    CHECK(h->call(5, 5) == 100); // (5+5)^2 = 100
    CHECK(h->call(3, 4) == 49); // (3+4)^2 = 49
    CHECK(h->call(-2, 2) == 0); // (-2+2)^2 = 0
}

TEST_CASE (



"asmjit: collatz(n) steps — nested if-in-loop (Go-style)"
,
"[asmjit][loop]"
)
 {
    auto h = jit(fn_collatz());
    CHECK(h->call(1, 0) == 0); // already 1
    CHECK(h->call(2, 0) == 1); // 2->1
    CHECK(h->call(4, 0) == 2); // 4->2->1
    CHECK(h->call(6, 0) == 8); // known Collatz sequence length
    CHECK(h->call(27, 0) == 111); // known: longest chain for n<=27
}

TEST_CASE (



"asmjit: popcount(n) — bit manipulation loop (Java-style)"
,
"[asmjit][loop][bitwise]"
)
 {
    auto h = jit(fn_popcount());
    CHECK(h->call(0, 0) == 0);
    CHECK(h->call(1, 0) == 1);
    CHECK(h->call(255, 0) == 8);
    CHECK(h->call(0xFF00FF, 0) == 16);
    // Note: shr is arithmetic (sign-preserving) so popcount only works for
    // non-negative inputs; negative values would loop forever via asr.
    CHECK(h->call(0x7FFFFFFFFFFFFFFF, 0) == 63); // all positive bits set
}

// ===========================================================================
// MULTIPLE INDEPENDENT JIT COMPILATIONS
// ===========================================================================

TEST_CASE (



"asmjit: all arithmetic ops coexist"
,
"[asmjit][multi]"
)
 {
    auto ha = jit(fn_add());
    auto hs = jit(fn_sub());
    auto hm = jit(fn_mul());
    auto hd = jit(fn_div());
    auto hr = jit(fn_mod());

    CHECK(ha->call(7, 3) == 10);
    CHECK(hs->call(7, 3) == 4);
    CHECK(hm->call(7, 3) == 21);
    CHECK(hd->call(7, 3) == 2);
    CHECK(hr->call(7, 3) == 1);
}

TEST_CASE (



"asmjit: multi-block functions coexist"
,
"[asmjit][multi]"
)
 {
    auto h_abs = jit(fn_abs());
    auto h_max = jit(fn_max());
    auto h_sum = jit(fn_sum_1_to_n());
    auto h_fib = jit(fn_fib());
    auto h_gcd = jit(fn_gcd());

    CHECK(h_abs->call(-7, 0) == 7);
    CHECK(h_max->call(3, 8) == 8);
    CHECK(h_sum->call(10, 0) == 55);
    CHECK(h_fib->call(10, 0) == 55);
    CHECK(h_gcd->call(48, 18) == 6);
}

// ===========================================================================
// EMIT ARTIFACT VALIDATION
// ===========================================================================

TEST_CASE (



"asmjit: emit produces jit_function artifact"
,
"[asmjit][artifact]"
)
 {
    asmjit_backend backend;
    const auto art = backend.emit(fn_add());
    CHECK(art.kind == artifact_kind::jit_function);
    CHECK(art.name == "add");
    CHECK(art.diagnostics.empty());
    CHECK(art.handle != nullptr);
    CHECK(art.handle->kind == artifact_handle_kind::jit_function);
    CHECK(art.metadata.count("jit_fn_name") == 1);
    // Handle is released automatically when art goes out of scope.
}

TEST_CASE (



"asmjit: re-using backend across multiple emit() calls"
,
"[asmjit][artifact]"
)
 {
    asmjit_backend backend;
    const auto a1 = backend.emit(fn_add());
    const auto a2 = backend.emit(fn_mul());
    const auto a3 = backend.emit(fn_fib());

    CHECK(a1.diagnostics.empty());
    CHECK(a2.diagnostics.empty());
    CHECK(a3.diagnostics.empty());

    auto h1 = std::shared_ptr<jit_function_handle>(a1.handle, asmjit_backend::get_handle(a1));
    auto h2 = std::shared_ptr<jit_function_handle>(a2.handle, asmjit_backend::get_handle(a2));
    auto h3 = std::shared_ptr<jit_function_handle>(a3.handle, asmjit_backend::get_handle(a3));

    CHECK(h1->call(2, 3) == 5);
    CHECK(h2->call(2, 3) == 6);
    CHECK(h3->call(10, 0) == 55);
}

// ===========================================================================
// FLOATING-POINT ARITHMETIC TESTS
// ===========================================================================

namespace {
    // double fadd_const() { return 1.5 + 2.5; }  → 4.0
    mir::physical_mir_function fn_fp_add() {
        return wrap("fp_add", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::fload_imm, {preg_op(0)}, {allocated_operand::as_f64(1.5)}),
                                       make_inst(2, opcode::fload_imm, {preg_op(1)}, {allocated_operand::as_f64(2.5)}),
                                       make_inst(3, opcode::fadd, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // double fmul_const() { return 2.0 * 3.0; }  → 6.0
    mir::physical_mir_function fn_fp_mul() {
        return wrap("fp_mul", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::fload_imm, {preg_op(0)}, {allocated_operand::as_f64(2.0)}),
                                       make_inst(2, opcode::fload_imm, {preg_op(1)}, {allocated_operand::as_f64(3.0)}),
                                       make_inst(3, opcode::fmul, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // double fdiv_const() { return 7.0 / 2.0; }  → 3.5
    mir::physical_mir_function fn_fp_div() {
        return wrap("fp_div", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::fload_imm, {preg_op(0)}, {allocated_operand::as_f64(7.0)}),
                                       make_inst(2, opcode::fload_imm, {preg_op(1)}, {allocated_operand::as_f64(2.0)}),
                                       make_inst(3, opcode::fdiv, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // double fsub_const() { return 10.0 - 3.5; }  → 6.5
    mir::physical_mir_function fn_fp_sub() {
        return wrap("fp_sub", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::fload_imm, {preg_op(0)}, {allocated_operand::as_f64(10.0)}),
                                       make_inst(2, opcode::fload_imm, {preg_op(1)}, {allocated_operand::as_f64(3.5)}),
                                       make_inst(3, opcode::fsub, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }

    // double fneg_const() { return -5.0; }  → -5.0
    mir::physical_mir_function fn_fp_neg() {
        return wrap("fp_neg", {
                        make_block(1, "entry", {
                                       make_inst(1, opcode::fload_imm, {preg_op(0)}, {allocated_operand::as_f64(5.0)}),
                                       make_inst(2, opcode::fneg, {preg_op(1)}, {preg_op(0)}),
                                       make_inst(3, opcode::ret, {}, {preg_op(1)}),
                                   })
                    });
    }

#if defined(LITHE_HAS_ASMJIT)
    static std::int64_t jit_trivial_add(std::int64_t a, std::int64_t b) noexcept { return a + b; }
    static double jit_trivial_f64(std::int64_t a, std::int64_t b) noexcept {
        return static_cast<double>(a + b);
    }
#endif
} // anonymous namespace

TEST_CASE (



"asmjit FP: fadd 1.5 + 2.5 = 4.0"
,
"[asmjit][fp]"
)
 {
    auto h = jit(fn_fp_add());
    REQUIRE(h->returns_f64());
    CHECK(h->call_f64(0, 0) == Catch::Approx(4.0));
}

TEST_CASE (



"asmjit FP: fmul 2.0 * 3.0 = 6.0"
,
"[asmjit][fp]"
)
 {
    auto h = jit(fn_fp_mul());
    REQUIRE(h->returns_f64());
    CHECK(h->call_f64(0, 0) == Catch::Approx(6.0));
}

TEST_CASE (



"asmjit FP: fdiv 7.0 / 2.0 = 3.5"
,
"[asmjit][fp]"
)
 {
    auto h = jit(fn_fp_div());
    REQUIRE(h->returns_f64());
    CHECK(h->call_f64(0, 0) == Catch::Approx(3.5));
}

TEST_CASE (



"asmjit FP: fsub 10.0 - 3.5 = 6.5"
,
"[asmjit][fp]"
)
 {
    auto h = jit(fn_fp_sub());
    REQUIRE(h->returns_f64());
    CHECK(h->call_f64(0, 0) == Catch::Approx(6.5));
}

TEST_CASE (



"asmjit FP: fneg 5.0 = -5.0"
,
"[asmjit][fp]"
)
 {
    auto h = jit(fn_fp_neg());
    REQUIRE(h->returns_f64());
    CHECK(h->call_f64(0, 0) == Catch::Approx(-5.0));
}

// ============================================================================
// Finding 11: jit_function_handle rejects wrong return lane
// ============================================================================

#if defined(LITHE_HAS_ASMJIT)
TEST_CASE ("jit_function_handle rejects wrong return lane", "[lithe][asmjit]") {
    // i64 handle: call() valid, call_f64() must assert (wrong lane).
    jit_function_handle i64_handle;
    // Manually set fn_ptr to a trivial function so valid() returns true.
    i64_handle.fn_ptr = &jit_trivial_add;

    REQUIRE(i64_handle.valid());
    REQUIRE_FALSE(i64_handle.returns_f64());
    REQUIRE(i64_handle.call(1, 2) == 3);

    // f64 handle: call_f64() valid.
    jit_function_handle f64_handle;
    f64_handle.fn_ptr_f64 = &jit_trivial_f64;

    REQUIRE(f64_handle.valid());
    REQUIRE(f64_handle.returns_f64());
    REQUIRE(f64_handle.call_f64(3, 4) == Catch::Approx(7.0));
}
#endif

// ============================================================================
// Hardening: jit_function_handle ownership and lifetime
// ============================================================================

#if defined(LITHE_HAS_ASMJIT)
TEST_CASE ("jit_function_handle move transfers ownership; moved-from is invalid",
          "[lithe][asmjit][ownership]") {
    // Construct a handle with a concrete function pointer so valid() is true.
    jit_function_handle src;
    src.fn_ptr = &jit_trivial_add;

    REQUIRE(src.valid());

    jit_function_handle dst = std::move(src);

    // After move: source must be invalid (fn_ptr was exchanged to nullptr).
    REQUIRE_FALSE(src.valid());  // NOLINT(bugprone-use-after-move) — intentional
    // Destination must own the pointer.
    REQUIRE(dst.valid());
    REQUIRE_FALSE(dst.returns_f64());
    REQUIRE(dst.call(5, 7) == 12);
}

TEST_CASE ("jit_function_handle move assignment transfers ownership",
          "[lithe][asmjit][ownership]") {
    jit_function_handle src;
    src.fn_ptr = &jit_trivial_add;

    jit_function_handle dst;
    dst = std::move(src);

    REQUIRE_FALSE(src.valid());  // NOLINT(bugprone-use-after-move) — intentional
    REQUIRE(dst.valid());
    REQUIRE(dst.call(10, 3) == 13);
}

TEST_CASE ("jit_function_handle default-constructed handle is invalid",
          "[lithe][asmjit][ownership]") {
    jit_function_handle h;
    REQUIRE_FALSE(h.valid());
    REQUIRE_FALSE(h.returns_f64());
    REQUIRE_THROWS_AS(h.call(0, 0), std::logic_error);
    REQUIRE_THROWS_AS(h.call_f64(0, 0), std::logic_error);
}
#endif

// ============================================================================
// GAP-3: fn_f64_t is double(*)(double, double) — correct ABI
// ============================================================================

#if defined(LITHE_HAS_ASMJIT)
namespace {
    // Reference function: takes two doubles, returns their product.
    static double gap3_f64_mul(double a, double b) noexcept { return a * b; }
} // anonymous namespace

TEST_CASE ("fn_f64_t accepts double arguments (GAP-3 ABI fix)",
          "[lithe][asmjit][gap3][abi]") {
    // Verify the type alias matches double(*)(double, double).
    static_assert(std::is_same_v<jit_function_handle::fn_f64_t,
                                 double (*)(double, double)>,
                  "fn_f64_t must be double(*)(double, double) — GAP-3 ABI fix");

    // Assign a native function with the correct double-arg signature.
    jit_function_handle h;
    h.fn_ptr_f64 = &gap3_f64_mul;

    REQUIRE(h.valid());
    REQUIRE(h.returns_f64());
    // Floating-point arguments are correctly passed as doubles.
    CHECK(h.call_f64(2.5, 4.0) == Catch::Approx(10.0));
    CHECK(h.call_f64(-1.0, 3.0) == Catch::Approx(-3.0));
    CHECK(h.call_f64(0.0, 99.9) == Catch::Approx(0.0));
}

TEST_CASE ("JIT-compiled f64 function returns correct result via call_f64 (GAP-3)",
          "[lithe][asmjit][gap3][abi]") {
    // JIT a function that computes 1.5 + 2.5 = 4.0 using fload_imm + fadd.
    // Then call it via call_f64(0, 0) — the args are ignored (constants),
    // but the call must succeed with the double-returning signature.
    auto h = jit(fn_fp_add());
    REQUIRE(h->returns_f64());
    CHECK(h->call_f64(0.0, 0.0) == Catch::Approx(4.0));
}

TEST_CASE ("JIT-compiled f64 function: call_f64 with non-zero args does not corrupt result (GAP-3)",
          "[lithe][asmjit][gap3][abi]") {
    // The JIT function uses only fload_imm (no load_arg) so the double args
    // passed to call_f64 are unused — but they must not corrupt registers.
    auto h = jit(fn_fp_mul());
    REQUIRE(h->returns_f64());
    CHECK(h->call_f64(3.14, 2.71) == Catch::Approx(6.0));
}
#endif
