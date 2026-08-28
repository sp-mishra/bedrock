# Chapter 14 — Standard Library

## What the Standard Library Is

Flux ships with a set of built-in functions that are always available: `sqrt`, `map`, `filter`,
`reduce`, `range`, `matmul`, etc. These are the **core builtins** — installed unconditionally by
`install_builtin_types()` into every type environment (ch05).

The **standard library** is a second layer: a collection of modules that a Flux program can
`import`. Each module wraps a group of C++ standard library functions behind Flux-typed bindings.
The mechanism is the same FFI registry from ch12 — but instead of user code calling
`ffi_registry::register_fn`, the standard library provides pre-built `install_std_*` functions
that batch-register entire modules.

```
Flux source:   import "std.math"
                    ↓
  ffi_module_registry::find("std.math")
                    ↓
  all symbols in the module become callable as extern fn in Flux
                    ↓
  CPU backend calls C++ <cmath> functions at runtime
```

### Relationship to crank::stdlib

Crank (the other language in this codebase) already has `crank::stdlib` modules:
`std.math`, `std.string`, `std.io`, `std.collections`, etc. — see
`include/languages/crank/std/`. Each module:

1. Wraps C++ STL free functions as `inline` wrappers
2. Registers them via `ffi_module_builder` + `detail::add_fn` into a Crank `context`
3. Marks each function with `kPure` (deterministic, no effects) or explicit effect flags

Flux's standard library follows the **exact same pattern**, but registers into
`flux::ffi_registry` instead of `crank::context`, and installs Flux type schemes via
`flux::type_arena` instead of Crank's typed-thunk descriptors.

---

## Architecture

```
include/languages/flux/stdlib/
  stdlib.hpp         — umbrella: install_flux_stdlib(ffi_registry&, ...)
  std_math.hpp       — <cmath> + <numbers> → std.math
  std_string.hpp     — <string> + <algorithm> → std.string
  std_io.hpp         — <print> → std.io  (IO effect)
  std_tensor.hpp     — tensor ops (norm, softmax, relu, sigmoid, ...) → std.tensor
  std_random.hpp     — <random> → std.random  (non-deterministic effect)
  std_algo.hpp       — <algorithm> functional ops → std.algo
  detail/
    register.hpp     — add_flux_fn<Name, Fn>: one call registers type + ffi_binding
```

### The registration funnel

Every std module uses `add_flux_fn` — the Flux equivalent of `crank::stdlib::detail::add_fn`:

```cpp
// include/languages/flux/stdlib/detail/register.hpp
#pragma once
#include <languages/flux/ffi.hpp>
#include <vakya/vakya_types.hpp>
#include <string_view>

namespace flux::stdlib::detail {

// add_flux_fn<HostName, Fn> — register one C++ function into the Flux ffi_registry.
//
// - Deduces arity from Fn's signature via callable_traits
// - Interns the Flux type scheme into tara (type arena)
// - Registers into reg under HostName
// - Records the ffi_symbol in the module descriptor
template <std::string_view HostName, auto Fn,
          typename TypeSchemeFn>
void add_flux_fn(flux::ffi_module_builder&       mod,
                 flux::ffi_registry&              reg,
                 vakya::types::type_arena&        tara,
                 vakya::types::type_var_generator& gen,
                 std::string                      flux_name,
                 TypeSchemeFn                     make_scheme,
                 uint64_t                         effects = 0,
                 uint64_t                         caps    = 0)
{
    // Build the type scheme for this function
    auto scheme = make_scheme(tara, gen);

    // Register into the ffi_registry (ch12)
    reg.register_fn(std::string(HostName),
                    reinterpret_cast<void*>(Fn),
                    scheme, effects, caps);

    // Record in the module descriptor so `import "std.math"` resolves it
    mod.fn(std::string(HostName), flux_name, callable_arity_v<decltype(Fn)>);
}

} // namespace flux::stdlib::detail
```

---

## Module: `std.math`

### What it provides

All transcendental and arithmetic functions from `<cmath>`, plus constants from `<numbers>`.
All functions are **pure** (deterministic, no side effects, no allocation).

```cpp
// include/languages/flux/stdlib/std_math.hpp
#pragma once
#include "detail/register.hpp"
#include <cmath>
#include <numbers>

namespace flux::stdlib {
namespace math_fns {
    // ── Unary f32 transcendentals ────────────────────────────────────────────
    [[nodiscard]] inline float sin_f32(float x)   noexcept { return std::sin(x); }
    [[nodiscard]] inline float cos_f32(float x)   noexcept { return std::cos(x); }
    [[nodiscard]] inline float tan_f32(float x)   noexcept { return std::tan(x); }
    [[nodiscard]] inline float asin_f32(float x)  noexcept { return std::asin(x); }
    [[nodiscard]] inline float acos_f32(float x)  noexcept { return std::acos(x); }
    [[nodiscard]] inline float atan_f32(float x)  noexcept { return std::atan(x); }
    [[nodiscard]] inline float sqrt_f32(float x)  noexcept { return std::sqrt(x); }
    [[nodiscard]] inline float cbrt_f32(float x)  noexcept { return std::cbrt(x); }
    [[nodiscard]] inline float exp_f32(float x)   noexcept { return std::exp(x); }
    [[nodiscard]] inline float exp2_f32(float x)  noexcept { return std::exp2(x); }
    [[nodiscard]] inline float expm1_f32(float x) noexcept { return std::expm1(x); }
    [[nodiscard]] inline float log_f32(float x)   noexcept { return std::log(x); }
    [[nodiscard]] inline float log2_f32(float x)  noexcept { return std::log2(x); }
    [[nodiscard]] inline float log10_f32(float x) noexcept { return std::log10(x); }
    [[nodiscard]] inline float log1p_f32(float x) noexcept { return std::log1p(x); }
    [[nodiscard]] inline float floor_f32(float x) noexcept { return std::floor(x); }
    [[nodiscard]] inline float ceil_f32(float x)  noexcept { return std::ceil(x); }
    [[nodiscard]] inline float round_f32(float x) noexcept { return std::round(x); }
    [[nodiscard]] inline float trunc_f32(float x) noexcept { return std::trunc(x); }
    [[nodiscard]] inline float abs_f32(float x)   noexcept { return std::fabs(x); }
    [[nodiscard]] inline float sign_f32(float x)  noexcept {
        return (x > 0.f) ? 1.f : (x < 0.f) ? -1.f : 0.f;
    }
    [[nodiscard]] inline float rsqrt_f32(float x) noexcept { return 1.f / std::sqrt(x); }

    // ── Binary f32 ────────────────────────────────────────────────────────────
    [[nodiscard]] inline float atan2_f32(float y, float x)  noexcept { return std::atan2(y,x); }
    [[nodiscard]] inline float pow_f32(float b, float e)    noexcept { return std::pow(b,e); }
    [[nodiscard]] inline float fmod_f32(float a, float b)   noexcept { return std::fmod(a,b); }
    [[nodiscard]] inline float hypot_f32(float a, float b)  noexcept { return std::hypot(a,b); }
    [[nodiscard]] inline float copysign_f32(float m, float s) noexcept { return std::copysign(m,s); }
    [[nodiscard]] inline float min_f32(float a, float b)    noexcept { return std::fmin(a,b); }
    [[nodiscard]] inline float max_f32(float a, float b)    noexcept { return std::fmax(a,b); }
    [[nodiscard]] inline float clamp_f32(float x, float lo, float hi) noexcept {
        return std::fmin(std::fmax(x, lo), hi);
    }

    // ── f64 variants ──────────────────────────────────────────────────────────
    [[nodiscard]] inline double sin_f64(double x)   noexcept { return std::sin(x); }
    [[nodiscard]] inline double cos_f64(double x)   noexcept { return std::cos(x); }
    [[nodiscard]] inline double sqrt_f64(double x)  noexcept { return std::sqrt(x); }
    [[nodiscard]] inline double exp_f64(double x)   noexcept { return std::exp(x); }
    [[nodiscard]] inline double log_f64(double x)   noexcept { return std::log(x); }
    [[nodiscard]] inline double pow_f64(double b, double e) noexcept { return std::pow(b,e); }
    [[nodiscard]] inline double min_f64(double a, double b) noexcept { return std::fmin(a,b); }
    [[nodiscard]] inline double max_f64(double a, double b) noexcept { return std::fmax(a,b); }
    [[nodiscard]] inline double clamp_f64(double x, double lo, double hi) noexcept {
        return std::fmin(std::fmax(x, lo), hi);
    }

    // ── i64 helpers ───────────────────────────────────────────────────────────
    [[nodiscard]] inline int64_t abs_i64(int64_t x)          noexcept { return std::abs(x); }
    [[nodiscard]] inline int64_t min_i64(int64_t a, int64_t b) noexcept { return a < b ? a : b; }
    [[nodiscard]] inline int64_t max_i64(int64_t a, int64_t b) noexcept { return a > b ? a : b; }
    [[nodiscard]] inline int64_t clamp_i64(int64_t x, int64_t lo, int64_t hi) noexcept {
        return x < lo ? lo : x > hi ? hi : x;
    }
    [[nodiscard]] inline int64_t gcd(int64_t a, int64_t b) noexcept {
        a = std::abs(a); b = std::abs(b);
        while (b) { a %= b; std::swap(a,b); }
        return a;
    }
    [[nodiscard]] inline int64_t lcm(int64_t a, int64_t b) noexcept {
        auto g = gcd(a,b);
        return g == 0 ? 0 : std::abs(a) / g * std::abs(b);
    }

    // ── Constants (nullary) ───────────────────────────────────────────────────
    [[nodiscard]] inline float  pi_f32() noexcept { return std::numbers::pi_v<float>; }
    [[nodiscard]] inline float  e_f32()  noexcept { return std::numbers::e_v<float>; }
    [[nodiscard]] inline float  tau_f32() noexcept { return 2.f * std::numbers::pi_v<float>; }
    [[nodiscard]] inline double pi_f64() noexcept { return std::numbers::pi; }
    [[nodiscard]] inline double e_f64()  noexcept { return std::numbers::e; }

} // namespace math_fns

inline void install_std_math(flux::ffi_registry&              reg,
                              vakya::types::type_arena&        tara,
                              vakya::types::type_var_generator& gen)
{
    using namespace math_fns;
    using D = detail;
    namespace TK = vakya::types;

    auto f32  = tara.intern_primitive("f32");
    auto f64  = tara.intern_primitive("f64");
    auto i64  = tara.intern_primitive("i64");
    auto bool_ = tara.intern_primitive("bool");

    // Helpers: build common scheme shapes
    auto f32_to_f32 = [&]{ return tara.intern_callable({f32}, f32); };
    auto f32f32_to_f32 = [&]{ return tara.intern_callable({f32,f32}, f32); };
    auto f32f32f32_to_f32 = [&]{ return tara.intern_callable({f32,f32,f32}, f32); };
    auto f64_to_f64 = [&]{ return tara.intern_callable({f64}, f64); };
    auto f64f64_to_f64 = [&]{ return tara.intern_callable({f64,f64}, f64); };
    auto i64_to_i64 = [&]{ return tara.intern_callable({i64}, i64); };
    auto i64i64_to_i64 = [&]{ return tara.intern_callable({i64,i64}, i64); };
    auto nullary_f32 = [&]{ return tara.intern_callable({}, f32); };
    auto nullary_f64 = [&]{ return tara.intern_callable({}, f64); };

    // Unary f32
    for (auto [name, fn_ptr] : std::array<std::pair<const char*, float(*)(float)>, 19>{{
        {"std.math.sin",   sin_f32},  {"std.math.cos",   cos_f32},
        {"std.math.tan",   tan_f32},  {"std.math.asin",  asin_f32},
        {"std.math.acos",  acos_f32}, {"std.math.atan",  atan_f32},
        {"std.math.sqrt",  sqrt_f32}, {"std.math.cbrt",  cbrt_f32},
        {"std.math.exp",   exp_f32},  {"std.math.exp2",  exp2_f32},
        {"std.math.expm1", expm1_f32},{"std.math.log",   log_f32},
        {"std.math.log2",  log2_f32}, {"std.math.log10", log10_f32},
        {"std.math.log1p", log1p_f32},{"std.math.floor", floor_f32},
        {"std.math.ceil",  ceil_f32}, {"std.math.round", round_f32},
        {"std.math.abs",   abs_f32},
    }})
        reg.register_fn(name, reinterpret_cast<void*>(fn_ptr), f32_to_f32());

    reg.register_fn("std.math.sign",   reinterpret_cast<void*>(sign_f32),  f32_to_f32());
    reg.register_fn("std.math.rsqrt",  reinterpret_cast<void*>(rsqrt_f32), f32_to_f32());
    reg.register_fn("std.math.trunc",  reinterpret_cast<void*>(trunc_f32), f32_to_f32());

    // Binary f32
    reg.register_fn("std.math.atan2",    reinterpret_cast<void*>(atan2_f32),    f32f32_to_f32());
    reg.register_fn("std.math.pow",      reinterpret_cast<void*>(pow_f32),      f32f32_to_f32());
    reg.register_fn("std.math.fmod",     reinterpret_cast<void*>(fmod_f32),     f32f32_to_f32());
    reg.register_fn("std.math.hypot",    reinterpret_cast<void*>(hypot_f32),    f32f32_to_f32());
    reg.register_fn("std.math.copysign", reinterpret_cast<void*>(copysign_f32), f32f32_to_f32());
    reg.register_fn("std.math.min",      reinterpret_cast<void*>(min_f32),      f32f32_to_f32());
    reg.register_fn("std.math.max",      reinterpret_cast<void*>(max_f32),      f32f32_to_f32());
    reg.register_fn("std.math.clamp",    reinterpret_cast<void*>(clamp_f32),    f32f32f32_to_f32());

    // f64
    reg.register_fn("std.math.sin_f64",  reinterpret_cast<void*>(sin_f64),  f64_to_f64());
    reg.register_fn("std.math.cos_f64",  reinterpret_cast<void*>(cos_f64),  f64_to_f64());
    reg.register_fn("std.math.sqrt_f64", reinterpret_cast<void*>(sqrt_f64), f64_to_f64());
    reg.register_fn("std.math.exp_f64",  reinterpret_cast<void*>(exp_f64),  f64_to_f64());
    reg.register_fn("std.math.log_f64",  reinterpret_cast<void*>(log_f64),  f64_to_f64());
    reg.register_fn("std.math.min_f64",  reinterpret_cast<void*>(min_f64),  f64f64_to_f64());
    reg.register_fn("std.math.max_f64",  reinterpret_cast<void*>(max_f64),  f64f64_to_f64());

    // i64
    reg.register_fn("std.math.abs_i64",   reinterpret_cast<void*>(abs_i64),   i64_to_i64());
    reg.register_fn("std.math.min_i64",   reinterpret_cast<void*>(min_i64),   i64i64_to_i64());
    reg.register_fn("std.math.max_i64",   reinterpret_cast<void*>(max_i64),   i64i64_to_i64());
    reg.register_fn("std.math.gcd",       reinterpret_cast<void*>(gcd),       i64i64_to_i64());
    reg.register_fn("std.math.lcm",       reinterpret_cast<void*>(lcm),       i64i64_to_i64());

    // Constants
    reg.register_fn("std.math.pi",   reinterpret_cast<void*>(pi_f32),  nullary_f32());
    reg.register_fn("std.math.e",    reinterpret_cast<void*>(e_f32),   nullary_f32());
    reg.register_fn("std.math.tau",  reinterpret_cast<void*>(tau_f32), nullary_f32());
    reg.register_fn("std.math.pi64", reinterpret_cast<void*>(pi_f64),  nullary_f64());
    reg.register_fn("std.math.e64",  reinterpret_cast<void*>(e_f64),   nullary_f64());
}

} // namespace flux::stdlib
```

### Flux usage after `import "std.math"`

```flux
import "std.math"

input x : f32

-- Trigonometry
let s = std.math.sin(x)
let c = std.math.cos(x)
let r = std.math.sqrt(s*s + c*c)   -- = 1.0 (Pythagorean identity)

-- Clamping
let safe = std.math.clamp(x, 0.0, 1.0)

-- Constants
let circle_area = std.math.pi() * x * x

-- Integer math
let a = 48
let b = 18
let g = std.math.gcd(a, b)   -- g = 6
```

### Effect annotation: all pure

Every `std.math` function is registered with `effects=0, caps=0` — pure, CPU-only.
`pure fn` declarations can call them freely:

```flux
import "std.math"

pure fn normalize(x : f32, y : f32) -> f32 {
    let len = std.math.sqrt(x*x + y*y)
    x / len
}
-- :effects → pure (no effects)
```

---

## Module: `std.string`

String operations backed by C++23 `<string>` and `<algorithm>`.
All functions are pure (value-in, value-out — no mutation).

```cpp
// include/languages/flux/stdlib/std_string.hpp
#pragma once
#include "detail/register.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace flux::stdlib {
namespace string_fns {
    [[nodiscard]] inline int64_t   len(std::string s)              noexcept { return (int64_t)s.size(); }
    [[nodiscard]] inline bool      empty(std::string s)             noexcept { return s.empty(); }
    [[nodiscard]] inline std::string to_upper(std::string s) {
        std::ranges::transform(s, s.begin(),
            [](unsigned char c){ return (char)std::toupper(c); });
        return s;
    }
    [[nodiscard]] inline std::string to_lower(std::string s) {
        std::ranges::transform(s, s.begin(),
            [](unsigned char c){ return (char)std::tolower(c); });
        return s;
    }
    [[nodiscard]] inline std::string trim(std::string s) {
        auto not_ws = [](unsigned char c){ return !std::isspace(c); };
        auto b = std::ranges::find_if(s, not_ws);
        auto e = std::find_if(s.rbegin(), s.rend(), not_ws).base();
        return b >= e ? std::string{} : std::string(b, e);
    }
    [[nodiscard]] inline bool      starts_with(std::string s, std::string pre) noexcept {
        return std::string_view{s}.starts_with(pre);
    }
    [[nodiscard]] inline bool      ends_with(std::string s, std::string suf) noexcept {
        return std::string_view{s}.ends_with(suf);
    }
    [[nodiscard]] inline bool      contains(std::string s, std::string needle) noexcept {
        return s.find(needle) != std::string::npos;
    }
    [[nodiscard]] inline int64_t   index_of(std::string s, std::string needle) noexcept {
        auto p = s.find(needle);
        return p == std::string::npos ? -1 : (int64_t)p;
    }
    [[nodiscard]] inline std::string substr(std::string s, int64_t start, int64_t count) {
        if (start < 0) start = 0;
        auto n = (int64_t)s.size();
        if (start >= n) return {};
        auto len = count < 0 ? n-start : std::min(count, n-start);
        return s.substr((size_t)start, (size_t)len);
    }
    [[nodiscard]] inline std::string replace(std::string s, std::string from, std::string to) {
        if (from.empty()) return s;
        std::string out; out.reserve(s.size());
        size_t pos=0, prev=0;
        while ((pos=s.find(from,prev)) != std::string::npos) {
            out.append(s,prev,pos-prev); out.append(to);
            prev = pos+from.size();
        }
        out.append(s,prev); return out;
    }
    [[nodiscard]] inline std::string concat(std::string a, std::string b){ a.append(b); return a; }
    [[nodiscard]] inline std::string repeat(std::string s, int64_t n) {
        if (n<=0) return {};
        std::string out; out.reserve(s.size()*(size_t)n);
        for (int64_t i=0;i<n;++i) out.append(s);
        return out;
    }
    [[nodiscard]] inline int64_t    char_at(std::string s, int64_t i) noexcept {
        if (i<0||(size_t)i>=s.size()) return -1;
        return (uint8_t)s[(size_t)i];
    }
    [[nodiscard]] inline std::string from_char(int64_t c) {
        if (c<0||c>127) return {};
        return std::string(1, (char)c);
    }
    [[nodiscard]] inline int64_t    to_int(std::string s) noexcept {
        try { return std::stoll(s); } catch(...) { return 0; }
    }
    [[nodiscard]] inline double     to_float(std::string s) noexcept {
        try { return std::stod(s); } catch(...) { return 0.0; }
    }
} // namespace string_fns

inline void install_std_string(flux::ffi_registry& reg,
                                vakya::types::type_arena& tara,
                                vakya::types::type_var_generator& /*gen*/)
{
    using namespace string_fns;
    auto str  = tara.intern_primitive("string");
    auto i64  = tara.intern_primitive("i64");
    auto f64  = tara.intern_primitive("f64");
    auto bool_ = tara.intern_primitive("bool");

    reg.register_fn("std.string.len",         reinterpret_cast<void*>(len),         tara.intern_callable({str}, i64));
    reg.register_fn("std.string.empty",       reinterpret_cast<void*>(empty),       tara.intern_callable({str}, bool_));
    reg.register_fn("std.string.to_upper",    reinterpret_cast<void*>(to_upper),    tara.intern_callable({str}, str));
    reg.register_fn("std.string.to_lower",    reinterpret_cast<void*>(to_lower),    tara.intern_callable({str}, str));
    reg.register_fn("std.string.trim",        reinterpret_cast<void*>(trim),        tara.intern_callable({str}, str));
    reg.register_fn("std.string.starts_with", reinterpret_cast<void*>(starts_with), tara.intern_callable({str,str}, bool_));
    reg.register_fn("std.string.ends_with",   reinterpret_cast<void*>(ends_with),   tara.intern_callable({str,str}, bool_));
    reg.register_fn("std.string.contains",    reinterpret_cast<void*>(contains),    tara.intern_callable({str,str}, bool_));
    reg.register_fn("std.string.index_of",    reinterpret_cast<void*>(index_of),    tara.intern_callable({str,str}, i64));
    reg.register_fn("std.string.substr",      reinterpret_cast<void*>(substr),      tara.intern_callable({str,i64,i64}, str));
    reg.register_fn("std.string.replace",     reinterpret_cast<void*>(replace),     tara.intern_callable({str,str,str}, str));
    reg.register_fn("std.string.concat",      reinterpret_cast<void*>(concat),      tara.intern_callable({str,str}, str));
    reg.register_fn("std.string.repeat",      reinterpret_cast<void*>(repeat),      tara.intern_callable({str,i64}, str));
    reg.register_fn("std.string.char_at",     reinterpret_cast<void*>(char_at),     tara.intern_callable({str,i64}, i64));
    reg.register_fn("std.string.from_char",   reinterpret_cast<void*>(from_char),   tara.intern_callable({i64}, str));
    reg.register_fn("std.string.to_int",      reinterpret_cast<void*>(to_int),      tara.intern_callable({str}, i64));
    reg.register_fn("std.string.to_float",    reinterpret_cast<void*>(to_float),    tara.intern_callable({str}, f64));
}

} // namespace flux::stdlib
```

### Flux usage

```flux
import "std.string"

let greeting = "  Hello, World!  "
let trimmed  = std.string.trim(greeting)       -- "Hello, World!"
let upper    = std.string.to_upper(trimmed)    -- "HELLO, WORLD!"
let n        = std.string.len(trimmed)         -- 13
let has_w    = std.string.contains(trimmed, "World")  -- true
let idx      = std.string.index_of(trimmed, "World")  -- 7

-- Build a string from a char code
let comma = std.string.from_char(44)           -- ","
let joined = std.string.concat("alpha", std.string.concat(comma, "beta"))  -- "alpha,beta"

-- Parse numbers from strings
let x = std.string.to_float("3.14")           -- 3.14 : f64
```

---

## Module: `std.io`

Console I/O backed by C++23 `<print>`. IO effect declared — `pure fn` cannot call these.

```cpp
// include/languages/flux/stdlib/std_io.hpp
#pragma once
#include "detail/register.hpp"
#include <print>
#include <string>

namespace flux::stdlib {
namespace io_fns {
    // Returns i64 0 so it flows through typed thunk with a concrete return type.
    // Flux maps the result to unit.
    inline int64_t print_str(std::string s)    { std::print("{}", s);   return 0; }
    inline int64_t println_str(std::string s)  { std::println("{}", s); return 0; }
    inline int64_t eprint_str(std::string s)   { std::print(stderr, "{}", s);   return 0; }
    inline int64_t eprintln_str(std::string s) { std::println(stderr, "{}", s); return 0; }
    inline int64_t println_i32(int32_t v)      { std::println("{}", v); return 0; }
    inline int64_t println_i64(int64_t v)      { std::println("{}", v); return 0; }
    inline int64_t println_f32(float v)        { std::println("{}", v); return 0; }
    inline int64_t println_f64(double v)       { std::println("{}", v); return 0; }
    inline int64_t println_bool(bool v)        { std::println("{}", v); return 0; }
} // namespace io_fns

// IO effect bit — must match the effect_mask used in the analysis pass
constexpr uint64_t kEffectIO = 1ULL << 2;  // stable_id = 3 → bit 2 (0-indexed)

inline void install_std_io(flux::ffi_registry& reg,
                            vakya::types::type_arena& tara,
                            vakya::types::type_var_generator& /*gen*/)
{
    using namespace io_fns;
    auto str  = tara.intern_primitive("string");
    auto i32  = tara.intern_primitive("i32");
    auto i64  = tara.intern_primitive("i64");
    auto f32  = tara.intern_primitive("f32");
    auto f64  = tara.intern_primitive("f64");
    auto bool_ = tara.intern_primitive("bool");

    // All IO functions declared with kEffectIO — pure fn cannot call them
    reg.register_fn("std.io.print",       reinterpret_cast<void*>(print_str),    tara.intern_callable({str},   i64), kEffectIO);
    reg.register_fn("std.io.println",     reinterpret_cast<void*>(println_str),  tara.intern_callable({str},   i64), kEffectIO);
    reg.register_fn("std.io.eprint",      reinterpret_cast<void*>(eprint_str),   tara.intern_callable({str},   i64), kEffectIO);
    reg.register_fn("std.io.eprintln",    reinterpret_cast<void*>(eprintln_str), tara.intern_callable({str},   i64), kEffectIO);
    reg.register_fn("std.io.println_i32", reinterpret_cast<void*>(println_i32),  tara.intern_callable({i32},   i64), kEffectIO);
    reg.register_fn("std.io.println_i64", reinterpret_cast<void*>(println_i64),  tara.intern_callable({i64},   i64), kEffectIO);
    reg.register_fn("std.io.println_f32", reinterpret_cast<void*>(println_f32),  tara.intern_callable({f32},   i64), kEffectIO);
    reg.register_fn("std.io.println_f64", reinterpret_cast<void*>(println_f64),  tara.intern_callable({f64},   i64), kEffectIO);
    reg.register_fn("std.io.println_bool",reinterpret_cast<void*>(println_bool), tara.intern_callable({bool_}, i64), kEffectIO);
}

} // namespace flux::stdlib
```

### Flux usage and effect checking

```flux
import "std.io"

-- IO is fine in a regular fn
fn report(x : f32) {
    std.io.println_f32(x)     -- IO effect propagates to report
}
report.show_effects()
-- effects: IO

-- But not in a pure fn
pure fn square(x : f32) -> f32 {
    std.io.println_f32(x)    -- ERROR: pure fn has IO effect
    x * x
}
-- error[E0030]: pure fn 'square' has IO effect
--   from: std.io.println_f32 (IO effect declared at registration)
```

### Why IO effect propagation works

When `std.io.println_f32` is registered with `effects = kEffectIO`, the `analysis_record`
for any expression that calls it accumulates that bit. The effect checker in ch05b then
walks up the call tree — if `report` calls `println_f32`, `report`'s record gets `IO` too.
The `pure fn` verifier rejects any function with non-zero effects.

---

## Module: `std.tensor`

Neural-network and signal-processing operations on tensors. Backed by C++ with optional
SIMD dispatch internally.

```cpp
// include/languages/flux/stdlib/std_tensor.hpp
#pragma once
#include "detail/register.hpp"
#include <cmath>
#include <numeric>
#include <algorithm>

namespace flux::stdlib {
namespace tensor_fns {

    // All functions take flux_tensor_view + return via output pointer
    // (same ABI as Level-1 FFI — ch12)
    using TV = flux_tensor_view;

    // ── Activation functions (elementwise, f32) ───────────────────────────────
    inline void relu(TV const* in, size_t /*n*/, TV* out) {
        auto* src = static_cast<float const*>(in[0].data);
        auto* dst = static_cast<float*>(out->data);
        for (size_t i = 0; i < in[0].dims[0]; ++i)
            dst[i] = src[i] > 0.f ? src[i] : 0.f;
    }
    inline void sigmoid(TV const* in, size_t, TV* out) {
        auto* src = static_cast<float const*>(in[0].data);
        auto* dst = static_cast<float*>(out->data);
        for (size_t i = 0; i < in[0].dims[0]; ++i)
            dst[i] = 1.f / (1.f + std::exp(-src[i]));
    }
    inline void tanh_act(TV const* in, size_t, TV* out) {
        auto* src = static_cast<float const*>(in[0].data);
        auto* dst = static_cast<float*>(out->data);
        for (size_t i = 0; i < in[0].dims[0]; ++i)
            dst[i] = std::tanh(src[i]);
    }
    inline void gelu(TV const* in, size_t, TV* out) {
        // GELU approximation: 0.5 * x * (1 + tanh(sqrt(2/pi) * (x + 0.044715*x^3)))
        constexpr float sqrt2_over_pi = 0.7978845608028654f;
        auto* src = static_cast<float const*>(in[0].data);
        auto* dst = static_cast<float*>(out->data);
        for (size_t i = 0; i < in[0].dims[0]; ++i) {
            float x = src[i];
            dst[i] = 0.5f * x * (1.f + std::tanh(sqrt2_over_pi * (x + 0.044715f * x*x*x)));
        }
    }
    inline void leaky_relu(TV const* in, size_t, TV* out) {
        // in[0] = input tensor, in[1] = scalar alpha (broadcasted)
        auto* src   = static_cast<float const*>(in[0].data);
        float alpha = *static_cast<float const*>(in[1].data);
        auto* dst   = static_cast<float*>(out->data);
        for (size_t i = 0; i < in[0].dims[0]; ++i)
            dst[i] = src[i] > 0.f ? src[i] : alpha * src[i];
    }

    // ── Normalization ─────────────────────────────────────────────────────────
    inline void softmax(TV const* in, size_t, TV* out) {
        // Numerically stable: subtract max before exp
        auto* src = static_cast<float const*>(in[0].data);
        auto* dst = static_cast<float*>(out->data);
        size_t n  = in[0].dims[0];
        float  m  = *std::max_element(src, src+n);
        float  s  = 0.f;
        for (size_t i = 0; i < n; ++i) { dst[i] = std::exp(src[i]-m); s += dst[i]; }
        for (size_t i = 0; i < n; ++i) dst[i] /= s;
    }
    inline void layer_norm(TV const* in, size_t, TV* out) {
        // in[0]=input, in[1]=scale(γ), in[2]=bias(β)
        auto* src   = static_cast<float const*>(in[0].data);
        auto* gamma = static_cast<float const*>(in[1].data);
        auto* beta  = static_cast<float const*>(in[2].data);
        auto* dst   = static_cast<float*>(out->data);
        size_t n    = in[0].dims[0];
        float  mean = std::accumulate(src, src+n, 0.f) / (float)n;
        float  var  = 0.f;
        for (size_t i=0;i<n;++i) { float d=src[i]-mean; var+=d*d; }
        var /= (float)n;
        float  inv  = 1.f / std::sqrt(var + 1e-5f);
        for (size_t i=0;i<n;++i) dst[i] = (src[i]-mean)*inv*gamma[i]+beta[i];
    }

    // ── Reductions ────────────────────────────────────────────────────────────
    inline void tensor_sum(TV const* in, size_t, TV* out) {
        auto* src = static_cast<float const*>(in[0].data);
        auto* dst = static_cast<float*>(out->data);
        *dst = std::accumulate(src, src+in[0].dims[0], 0.f);
    }
    inline void tensor_mean(TV const* in, size_t, TV* out) {
        auto* src = static_cast<float const*>(in[0].data);
        auto* dst = static_cast<float*>(out->data);
        size_t n  = in[0].dims[0];
        *dst = std::accumulate(src, src+n, 0.f) / (float)n;
    }
    inline void tensor_max(TV const* in, size_t, TV* out) {
        auto* src = static_cast<float const*>(in[0].data);
        auto* dst = static_cast<float*>(out->data);
        *dst = *std::max_element(src, src+in[0].dims[0]);
    }
    inline void tensor_min(TV const* in, size_t, TV* out) {
        auto* src = static_cast<float const*>(in[0].data);
        auto* dst = static_cast<float*>(out->data);
        *dst = *std::min_element(src, src+in[0].dims[0]);
    }
    inline void tensor_norm2(TV const* in, size_t, TV* out) {
        // L2 norm: sqrt(sum(x*x))
        auto* src = static_cast<float const*>(in[0].data);
        auto* dst = static_cast<float*>(out->data);
        float s   = 0.f;
        for (size_t i=0; i<in[0].dims[0]; ++i) s += src[i]*src[i];
        *dst = std::sqrt(s);
    }

    // ── Element-wise scalar ops ───────────────────────────────────────────────
    inline void tensor_scale(TV const* in, size_t, TV* out) {
        // in[0]=tensor, in[1]=scalar
        auto* src   = static_cast<float const*>(in[0].data);
        float alpha = *static_cast<float const*>(in[1].data);
        auto* dst   = static_cast<float*>(out->data);
        for (size_t i=0; i<in[0].dims[0]; ++i) dst[i] = src[i]*alpha;
    }
    inline void tensor_add_scalar(TV const* in, size_t, TV* out) {
        auto* src = static_cast<float const*>(in[0].data);
        float b   = *static_cast<float const*>(in[1].data);
        auto* dst = static_cast<float*>(out->data);
        for (size_t i=0; i<in[0].dims[0]; ++i) dst[i] = src[i]+b;
    }
    inline void tensor_clip(TV const* in, size_t, TV* out) {
        // in[0]=tensor, in[1]=lo, in[2]=hi
        auto* src = static_cast<float const*>(in[0].data);
        float lo  = *static_cast<float const*>(in[1].data);
        float hi  = *static_cast<float const*>(in[2].data);
        auto* dst = static_cast<float*>(out->data);
        for (size_t i=0; i<in[0].dims[0]; ++i)
            dst[i] = src[i]<lo ? lo : src[i]>hi ? hi : src[i];
    }

} // namespace tensor_fns

inline void install_std_tensor(flux::ffi_registry& reg,
                                vakya::types::type_arena& tara,
                                vakya::types::type_var_generator& gen)
{
    using namespace tensor_fns;
    // All tensor ops are pure (no side effects), CPU capability
    // Type schemes use rank-polymorphic tensor<f32> (shape from analysis pass)
    auto f32     = tara.intern_primitive("f32");
    auto T_var   = tara.intern_variable(gen.fresh());
    type_ref ta[] = {T_var};
    auto tensor_T = tara.intern_constructor("tensor", ta);  // tensor<T> (rank unspecified here)

    // f32 tensor → f32 tensor (elementwise, shape-preserving)
    auto t_to_t = tara.intern_callable({tensor_T}, tensor_T);
    // f32 scalar result
    auto t_to_f32 = tara.intern_callable({tensor_T}, f32);
    // tensor + scalar → tensor
    auto tf_to_t = tara.intern_callable({tensor_T, f32}, tensor_T);
    // tensor + tensor + tensor → tensor (layer_norm: input, gamma, beta)
    auto ttt_to_t = tara.intern_callable({tensor_T, tensor_T, tensor_T}, tensor_T);

    // Activations (elementwise)
    reg.register_fn("std.tensor.relu",        reinterpret_cast<void*>(relu),        t_to_t);
    reg.register_fn("std.tensor.sigmoid",     reinterpret_cast<void*>(sigmoid),     t_to_t);
    reg.register_fn("std.tensor.tanh",        reinterpret_cast<void*>(tanh_act),    t_to_t);
    reg.register_fn("std.tensor.gelu",        reinterpret_cast<void*>(gelu),        t_to_t);
    reg.register_fn("std.tensor.softmax",     reinterpret_cast<void*>(softmax),     t_to_t);
    reg.register_fn("std.tensor.leaky_relu",  reinterpret_cast<void*>(leaky_relu),  tf_to_t);
    reg.register_fn("std.tensor.layer_norm",  reinterpret_cast<void*>(layer_norm),  ttt_to_t);

    // Reductions → scalar
    reg.register_fn("std.tensor.sum",    reinterpret_cast<void*>(tensor_sum),   t_to_f32);
    reg.register_fn("std.tensor.mean",   reinterpret_cast<void*>(tensor_mean),  t_to_f32);
    reg.register_fn("std.tensor.max",    reinterpret_cast<void*>(tensor_max),   t_to_f32);
    reg.register_fn("std.tensor.min",    reinterpret_cast<void*>(tensor_min),   t_to_f32);
    reg.register_fn("std.tensor.norm2",  reinterpret_cast<void*>(tensor_norm2), t_to_f32);

    // Element-wise scalar ops
    reg.register_fn("std.tensor.scale",       reinterpret_cast<void*>(tensor_scale),      tf_to_t);
    reg.register_fn("std.tensor.add_scalar",  reinterpret_cast<void*>(tensor_add_scalar), tf_to_t);
    reg.register_fn("std.tensor.clip",        reinterpret_cast<void*>(tensor_clip),       ttt_to_t);
}

} // namespace flux::stdlib
```

### Flux usage

```flux
import "std.tensor"

input logits : tensor<f32>[512]
input gamma  : tensor<f32>[512]
input beta   : tensor<f32>[512]

-- Activation functions
let activated_relu    = std.tensor.relu(logits)
let activated_sigmoid = std.tensor.sigmoid(logits)
let probs             = std.tensor.softmax(logits)

-- Layer normalization
let normed = std.tensor.layer_norm(logits, gamma, beta)

-- Reductions
let total = std.tensor.sum(logits)    -- total : f32
let avg   = std.tensor.mean(logits)   -- avg   : f32
let norm  = std.tensor.norm2(logits)  -- norm  : f32

-- Clipping
let safe  = std.tensor.clip(logits, -10.0, 10.0)

-- Full forward pass example
let hidden = std.tensor.gelu(std.tensor.layer_norm(logits, gamma, beta))
let output = std.tensor.softmax(std.tensor.scale(hidden, 0.1))
```

---

## Module: `std.random`

Non-deterministic random number generation via C++ `<random>`.
Declared **non-deterministic** — cannot be called from `pure fn`.

```cpp
// include/languages/flux/stdlib/std_random.hpp
#pragma once
#include "detail/register.hpp"
#include <random>

namespace flux::stdlib {
namespace random_fns {
    // Thread-local engine — no shared state, no data races
    inline std::mt19937& rng() {
        thread_local std::mt19937 eng{std::random_device{}()};
        return eng;
    }

    inline float  rand_f32() {
        return std::uniform_real_distribution<float>{0.f, 1.f}(rng());
    }
    inline double rand_f64() {
        return std::uniform_real_distribution<double>{0.0, 1.0}(rng());
    }
    inline int64_t rand_i64(int64_t lo, int64_t hi) {
        return std::uniform_int_distribution<int64_t>{lo, hi}(rng());
    }
    inline float  rand_normal_f32() {
        return std::normal_distribution<float>{0.f, 1.f}(rng());
    }
    inline void   seed(int64_t s) {
        rng().seed(static_cast<uint32_t>(s));
    }
} // namespace random_fns

// Non-deterministic effect bit (bit 4 = stable_id 5)
constexpr uint64_t kEffectNonDeterministic = 1ULL << 4;

inline void install_std_random(flux::ffi_registry& reg,
                                vakya::types::type_arena& tara,
                                vakya::types::type_var_generator& /*gen*/)
{
    using namespace random_fns;
    auto f32 = tara.intern_primitive("f32");
    auto f64 = tara.intern_primitive("f64");
    auto i64 = tara.intern_primitive("i64");
    auto unit = tara.intern_primitive("unit");

    reg.register_fn("std.random.rand_f32",    reinterpret_cast<void*>(rand_f32),    tara.intern_callable({}, f32), kEffectNonDeterministic);
    reg.register_fn("std.random.rand_f64",    reinterpret_cast<void*>(rand_f64),    tara.intern_callable({}, f64), kEffectNonDeterministic);
    reg.register_fn("std.random.rand_i64",    reinterpret_cast<void*>(rand_i64),    tara.intern_callable({i64,i64}, i64), kEffectNonDeterministic);
    reg.register_fn("std.random.rand_normal", reinterpret_cast<void*>(rand_normal_f32), tara.intern_callable({}, f32), kEffectNonDeterministic);
    reg.register_fn("std.random.seed",        reinterpret_cast<void*>(seed),        tara.intern_callable({i64}, unit));
}

} // namespace flux::stdlib
```

### Flux usage

```flux
import "std.random"

-- Generate random values
let r  = std.random.rand_f32()          -- uniform [0,1)
let n  = std.random.rand_normal_f32()   -- N(0,1)
let d6 = std.random.rand_i64(1, 6)      -- die roll

-- Seed for reproducibility
std.random.seed(42)
let x = std.random.rand_f32()           -- same every run after seed(42)

-- Cannot use in pure fn:
pure fn bad() -> f32 {
    std.random.rand_f32()   -- ERROR: non-deterministic effect
}
```

---

## Module: `std.algo`

Higher-order functional algorithms not covered by the core `map`/`filter`/`reduce` builtins.
All pure, backed by `<algorithm>` and `<numeric>`.

```cpp
// include/languages/flux/stdlib/std_algo.hpp
#pragma once
#include "detail/register.hpp"
#include <algorithm>
#include <numeric>
#include <vector>

namespace flux::stdlib {
namespace algo_fns {
    using fvec = std::vector<float>;
    using ivec = std::vector<int64_t>;

    [[nodiscard]] inline fvec sort_asc(fvec v) {
        std::ranges::sort(v); return v;
    }
    [[nodiscard]] inline fvec sort_desc(fvec v) {
        std::ranges::sort(v, std::greater<float>{}); return v;
    }
    [[nodiscard]] inline fvec reverse_vec(fvec v) {
        std::ranges::reverse(v); return v;
    }
    [[nodiscard]] inline fvec unique_vals(fvec v) {
        std::ranges::sort(v);
        auto last = std::unique(v.begin(), v.end());
        v.erase(last, v.end());
        return v;
    }
    [[nodiscard]] inline ivec argsort(fvec v) {
        ivec idx(v.size());
        std::iota(idx.begin(), idx.end(), 0);
        std::ranges::sort(idx, [&](auto a, auto b){ return v[a] < v[b]; });
        return idx;
    }
    [[nodiscard]] inline float dot_vec(fvec a, fvec b) noexcept {
        float s = 0.f;
        auto n  = std::min(a.size(), b.size());
        for (size_t i=0;i<n;++i) s += a[i]*b[i];
        return s;
    }
    [[nodiscard]] inline float running_sum(fvec v) noexcept {
        return std::accumulate(v.begin(), v.end(), 0.f);
    }
    [[nodiscard]] inline fvec cumsum(fvec v) {
        std::partial_sum(v.begin(), v.end(), v.begin()); return v;
    }
    [[nodiscard]] inline fvec linspace(float start, float stop, int64_t n) {
        fvec out; out.reserve((size_t)n);
        if (n <= 1) { out.push_back(start); return out; }
        float step = (stop - start) / (float)(n-1);
        for (int64_t i=0;i<n;++i) out.push_back(start + (float)i * step);
        return out;
    }
    [[nodiscard]] inline fvec clamp_vec(fvec v, float lo, float hi) {
        for (auto& x : v) x = x<lo ? lo : x>hi ? hi : x;
        return v;
    }
} // namespace algo_fns

inline void install_std_algo(flux::ffi_registry& reg,
                              vakya::types::type_arena& tara,
                              vakya::types::type_var_generator& /*gen*/)
{
    using namespace algo_fns;
    auto f32  = tara.intern_primitive("f32");
    auto i64  = tara.intern_primitive("i64");

    // vec<f32> → f32 type ref
    type_ref ta[] = {f32};
    auto vec_f32 = tara.intern_constructor("vec", ta);

    reg.register_fn("std.algo.sort",       reinterpret_cast<void*>(sort_asc),    tara.intern_callable({vec_f32}, vec_f32));
    reg.register_fn("std.algo.sort_desc",  reinterpret_cast<void*>(sort_desc),   tara.intern_callable({vec_f32}, vec_f32));
    reg.register_fn("std.algo.reverse",    reinterpret_cast<void*>(reverse_vec), tara.intern_callable({vec_f32}, vec_f32));
    reg.register_fn("std.algo.unique",     reinterpret_cast<void*>(unique_vals), tara.intern_callable({vec_f32}, vec_f32));
    reg.register_fn("std.algo.dot",        reinterpret_cast<void*>(dot_vec),     tara.intern_callable({vec_f32, vec_f32}, f32));
    reg.register_fn("std.algo.sum",        reinterpret_cast<void*>(running_sum), tara.intern_callable({vec_f32}, f32));
    reg.register_fn("std.algo.cumsum",     reinterpret_cast<void*>(cumsum),      tara.intern_callable({vec_f32}, vec_f32));
    reg.register_fn("std.algo.linspace",   reinterpret_cast<void*>(linspace),    tara.intern_callable({f32, f32, i64}, vec_f32));
    reg.register_fn("std.algo.clamp",      reinterpret_cast<void*>(clamp_vec),   tara.intern_callable({vec_f32, f32, f32}, vec_f32));
}

} // namespace flux::stdlib
```

### Flux usage

```flux
import "std.algo"

let data = range(1, 10).map(fn(x) { x * 1.0 })   -- vec<f32> [1,2,...,9]

let sorted   = std.algo.sort(data)
let reversed = std.algo.reverse(sorted)
let total    = std.algo.sum(data)       -- 45.0
let grid     = std.algo.linspace(0.0, 1.0, 11)   -- [0.0, 0.1, ..., 1.0]
let cs       = std.algo.cumsum(grid)    -- [0.0, 0.1, 0.2, ..., 5.5]
```

---

## Umbrella: `install_flux_stdlib`

One call registers all standard library modules:

```cpp
// include/languages/flux/stdlib/stdlib.hpp
#pragma once
#include "std_math.hpp"
#include "std_string.hpp"
#include "std_io.hpp"
#include "std_tensor.hpp"
#include "std_random.hpp"
#include "std_algo.hpp"

namespace flux::stdlib {

// install_flux_stdlib — register all standard library modules into an ffi_registry.
//
// Call once after constructing the ffi_registry, before compiling any Flux source
// that uses import "std.*".
//
// selective: pass false to omit non-deterministic (std.random) and IO (std.io)
// modules — useful for pure compute environments.
inline void install_flux_stdlib(flux::ffi_registry&              reg,
                                 vakya::types::type_arena&        tara,
                                 vakya::types::type_var_generator& gen,
                                 bool selective = false)
{
    install_std_math(reg, tara, gen);
    install_std_string(reg, tara, gen);
    install_std_tensor(reg, tara, gen);
    install_std_algo(reg, tara, gen);

    if (!selective) {
        install_std_io(reg, tara, gen);
        install_std_random(reg, tara, gen);
    }
}

} // namespace flux::stdlib
```

---

## How `import` Works: Resolution Pipeline

When Flux source contains `import "std.math"`:

```
1. ch04 name resolution sees import_decl node for "std.math"
2. resolver::resolve_import("std.math")
3. Checks ffi_module_registry::find("std.math")
4. Found → iterates all ffi_symbols in the module
5. For each symbol: inserts name → ffi_binding into the ffi_registry lookup table
6. For each symbol: installs the type scheme into type_env_
7. From this point, "std.math.sin", "std.math.cos" etc. resolve like any extern fn
```

```
                              Flux source
                                  |
                 import "std.math" declaration
                                  |
                     ch04 resolver::resolve_import
                                  |
                    ffi_module_registry::find("std.math")
                         /                  \
                   not found             found: ffi_module_descriptor
                      |                          |
                   error:                  for each ffi_symbol:
              "unknown module"              ├─ ffi_registry.insert(name, binding)
                                            └─ type_env_.insert(name, scheme)
                                  |
                     std.math.sin, std.math.cos etc.
                     now resolve like any extern fn
                                  |
                              ch07 lowerer
                                  |
                          ffi_call_tag node
                                  |
                         CPU backend → <cmath> call
```

---

## Integration: Wiring stdlib into `flux_engine`

In `repl_engine.hpp` (ch13), `flux_engine` installs the standard library during construction:

```cpp
#include <languages/flux/stdlib/stdlib.hpp>

flux_engine::flux_engine() {
    // Install core builtins (sqrt, map, filter, range, ...)
    install_builtin_types(type_env_, tara_, gen_);

    // Install standard library modules
    flux::stdlib::install_flux_stdlib(ffi_, tara_, gen_);
}
```

After this, any Flux program can `import "std.math"`, `import "std.tensor"`, etc. and use
the full standard library without any additional setup.

---

## Complete Tutorial: Copy-Paste Programs

### Program 1 — Math module

```cpp
// stdlib_math_tutorial.cpp
#include <languages/flux/stdlib/stdlib.hpp>
#include <languages/flux/compiler.hpp>
#include <print>

int main() {
    flux::ffi_registry ffi;
    vakya::types::type_arena tara;
    vakya::types::type_var_generator gen;
    flux::stdlib::install_flux_stdlib(ffi, tara, gen);

    flux::compiler compiler{ffi, tara, gen};

    auto result = compiler.run(R"(
        import "std.math"

        input x : f32
        input y : f32

        let dist   = std.math.sqrt(x*x + y*y)
        let angle  = std.math.atan2(y, x)
        let pi     = std.math.pi()
        let deg    = angle * 180.0 / pi
    )", {{"x", 3.0f}, {"y", 4.0f}}, "dist", "deg");

    std::println("distance = {}", result.get<float>("dist"));   // 5.0
    std::println("angle    = {} deg", result.get<float>("deg")); // 53.13...
}
```

Expected:
```text
distance = 5
angle    = 53.1301 deg
```

### Program 2 — Tensor activations

```cpp
// stdlib_tensor_tutorial.cpp
#include <languages/flux/stdlib/stdlib.hpp>
#include <languages/flux/compiler.hpp>
#include <print>

int main() {
    flux::ffi_registry ffi;
    vakya::types::type_arena tara;
    vakya::types::type_var_generator gen;
    flux::stdlib::install_flux_stdlib(ffi, tara, gen);

    flux::compiler compiler{ffi, tara, gen};

    // Softmax a simple 4-element logit vector
    std::vector<float> logits = {1.0f, 2.0f, 3.0f, 4.0f};

    auto result = compiler.run(R"(
        import "std.tensor"

        input logits : tensor<f32>[4]
        let probs = std.tensor.softmax(logits)
        let total = std.tensor.sum(probs)   -- should be 1.0
    )", {{"logits", logits}}, "probs", "total");

    auto probs = result.get<std::vector<float>>("probs");
    std::println("softmax: [{:.4f}, {:.4f}, {:.4f}, {:.4f}]",
                 probs[0], probs[1], probs[2], probs[3]);
    std::println("sum(softmax) = {:.6f}", result.get<float>("total"));
}
```

Expected:
```text
softmax: [0.0321, 0.0871, 0.2369, 0.6439]
sum(softmax) = 1.000000
```

### Program 3 — String processing

```cpp
// stdlib_string_tutorial.cpp
#include <languages/flux/stdlib/stdlib.hpp>
#include <languages/flux/compiler.hpp>
#include <print>

int main() {
    flux::ffi_registry ffi;
    vakya::types::type_arena tara;
    vakya::types::type_var_generator gen;
    flux::stdlib::install_flux_stdlib(ffi, tara, gen);

    flux::compiler compiler{ffi, tara, gen};

    auto result = compiler.run(R"(
        import "std.string"

        let raw = "  Hello, Flux!  "
        let t   = std.string.trim(raw)
        let u   = std.string.to_upper(t)
        let n   = std.string.len(t)
        let has = std.string.contains(t, "Flux")
    )", {}, "t", "u", "n", "has");

    std::println("trimmed  : '{}'", result.get<std::string>("t"));
    std::println("upper    : '{}'", result.get<std::string>("u"));
    std::println("length   : {}",   result.get<int64_t>("n"));
    std::println("has Flux : {}",   result.get<bool>("has"));
}
```

Expected:
```text
trimmed  : 'Hello, Flux!'
upper    : 'HELLO, FLUX!'
length   : 12
has Flux : true
```

### Program 4 — REPL with stdlib

```text
flux> :import std.math
std.math loaded (38 functions)

flux> std.math.sin(std.math.pi() / 2.0)
1 : f32

flux> std.math.gcd(48, 18)
6 : i64

flux> import "std.tensor"
std.tensor loaded (16 functions)

flux> input v : tensor<f32>[4]
v : tensor<f32>[4]

flux> let s = std.tensor.softmax(v)
s : tensor<f32>[4]

flux> std.tensor.sum(s)
1 : f32

flux> import "std.random"
std.random loaded (5 functions)

flux> std.random.rand_f32()
0.73821 : f32

flux> pure fn bad() -> f32 { std.random.rand_f32() }
error[E0030]: pure fn 'bad' has non-deterministic effect
  --> <repl>:1:26
   |
 1 | pure fn bad() -> f32 { std.random.rand_f32() }
   |                        ^^^^^^^^^^^^^^^^^^^^  non-deterministic effect from std.random
   |
   = help: remove 'pure' keyword or replace with a deterministic expression
```

---

## Effect Summary Across All Modules

| Module | Effects | Capabilities | Notes |
|--------|---------|-------------|-------|
| `std.math` | none (pure) | CPU | Safe in `pure fn` |
| `std.string` | none (pure) | CPU | Safe in `pure fn` |
| `std.tensor` | none (pure) | CPU | Safe in `pure fn` |
| `std.algo` | none (pure) | CPU | Safe in `pure fn` |
| `std.io` | IO | CPU | Not allowed in `pure fn` |
| `std.random` | NonDeterministic | CPU | Not allowed in `pure fn` |

---

## What We Have

| Module | C++ header | Functions |
|--------|-----------|-----------|
| `std.math` | `<cmath>`, `<numbers>` | 40+ transcendentals, min/max/clamp, gcd/lcm, constants |
| `std.string` | `<string>`, `<algorithm>` | 18 string ops |
| `std.io` | `<print>` | 9 print/println variants (IO effect) |
| `std.tensor` | `<cmath>`, `<algorithm>`, `<numeric>` | 15 activations, normalizations, reductions |
| `std.random` | `<random>` | 5 random generators (non-deterministic effect) |
| `std.algo` | `<algorithm>`, `<numeric>` | 9 sorting/scan/linspace ops |

All modules are:
- **Header-only** — no separate compilation
- **Zero-cost abstraction** — C++ inline functions with no indirection
- **Effect-annotated** — each function declares its side effects at registration time
- **Type-inferred** — Flux deduces return types from type schemes; no explicit annotation needed

## Next

[Chapter 12 → FFI](ch_ffi.md) — register your own C++ functions as Flux builtins using the
same mechanism the standard library uses.

[Chapter 13 → Debugging & REPL](ch_debug_repl.md) — run the standard library functions
interactively in the REPL.
