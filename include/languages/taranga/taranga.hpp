#pragma once

// taranga/taranga.hpp — Umbrella include for the Taranga WebAssembly front-end.
//
// Includes the full six-band pipeline in dependency order:
//
//   Band 0: lexer.hpp, source_span.hpp, ast_tags.hpp
//   Band 1: opcode_map.hpp
//   Band 2: build_ast.hpp
//   Band 3: parser_wat.hpp, decoder_bin.hpp
//   Band 4: frontend.hpp (parse façade)
//   Band 5: module_view.hpp
//   Band 6: validate.hpp
//   Band 7: ssa_build.hpp
//   Band 8: memory.hpp, runtime_prelude.hpp
//   Band 9: lower_hl.hpp
//   Band 10: engine.hpp (instantiate/invoke), aot.hpp (cache/interchange)
//
// Pay-for-use: include individual headers when only part of the pipeline
// is needed. This umbrella is for tests and tools that use everything.

#include "languages/taranga/aot.hpp"
#include "languages/taranga/engine.hpp"
#include "languages/taranga/frontend.hpp"
#include "languages/taranga/lower_hl.hpp"
#include "languages/taranga/memory.hpp"
#include "languages/taranga/module_view.hpp"
#include "languages/taranga/opcode_map.hpp"
#include "languages/taranga/runtime_prelude.hpp"
#include "languages/taranga/ssa_build.hpp"
#include "languages/taranga/std/std.hpp"
#include "languages/taranga/validate.hpp"
