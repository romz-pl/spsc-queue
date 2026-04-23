# Static Inheritance in C++: A Deep Technical Analysis

## What "Static Inheritance" Means

In C++, the term *static inheritance* most precisely refers to inheritance relationships resolved entirely at **compile time**, as opposed to dynamic (runtime) polymorphism driven by virtual dispatch. It encompasses two primary mechanisms:

**Curiously Recurring Template Pattern (CRTP)** and **non-virtual base class inheritance with no virtual functions**. Both eliminate the runtime overhead of vtables, but they impose distinct trade-offs in binary size, compilation model, and optimization surface.

---

## CRTP: The Canonical Form of Static Inheritance

```cpp
template <typename Derived>
class Base {
public:
    void interface() {
        static_cast<Derived*>(this)->implementation();
    }
    void common_behavior() { /* shared logic */ }
};

class ConcreteA : public Base<ConcreteA> {
public:
    void implementation() { /* A-specific */ }
};

class ConcreteB : public Base<ConcreteB> {
public:
    void implementation() { /* B-specific */ }
};
```

The `static_cast` to `Derived*` is safe because the base is always instantiated with its actual derived type. The compiler resolves `implementation()` without any indirection — no vtable pointer, no virtual call, no dynamic dispatch.

**Non-virtual concrete inheritance** is simpler but more limited:

```cpp
class Base {
public:
    void method() { /* base impl */ }
};

class Derived : public Base {
public:
    void method() { /* hides, does not override */ } // name hiding
};
```

Here, dispatch is determined purely by the static type of the variable. No polymorphic substitution occurs.

---

## Impact on Binary Size

This is where static inheritance diverges sharply from dynamic inheritance, and the effects are non-trivial.

### 1. Template Instantiation Explosion

CRTP's central cost is **monomorphization**. Every unique `Derived` type causes the compiler to stamp out a fully independent instantiation of `Base<Derived>`. If `Base` contains substantial logic in `common_behavior()`, that logic is **duplicated** in the binary for every derived class. With N derived types, you get N copies of every template method body.

```
Binary contains:
  Base<ConcreteA>::common_behavior()  →  40 bytes of machine code
  Base<ConcreteB>::common_behavior()  →  40 bytes of machine code
  Base<ConcreteC>::common_behavior()  →  40 bytes of machine code
  ...
```

Contrast this with virtual dispatch, where `Base::common_behavior()` exists once and is reached through the vtable. The vtable itself is small (a pointer per virtual function per class), but the function body is never duplicated.

### 2. Absence of vtable overhead

Dynamic inheritance generates, per class:
- A **vtable** (a contiguous array of function pointers, typically 8 bytes each on 64-bit platforms, plus a RTTI pointer).
- A **vptr** embedded in each object instance (8 bytes on 64-bit).
- A `type_info` struct for RTTI.

Static inheritance eliminates all of this. For programs with hundreds of polymorphic class hierarchies but few instantiated types, virtual dispatch actually produces smaller binaries. The crossover point depends on the size of shared logic versus the number of derived types.

### 3. Inlining and Dead Code Elimination

The compiler has full visibility into all call targets at compile time. This enables aggressive **inlining** — `interface()` calling `implementation()` becomes a single inlined unit. The result is that hot paths collapse into straight-line machine code with no branches. However, inlining also inflates code size at each call site, a classic code-size vs. speed trade-off.

**Dead code elimination** is substantially more effective. Since no call can arrive through an unknown vtable slot, the linker can trivially prove that unreachable specializations are dead and strip them with `--gc-sections`.

### 4. Quantitative Summary

| Factor | Static (CRTP) | Dynamic (virtual) |
|---|---|---|
| Per-class vtable cost | None | ~(N_virtuals × 8) bytes |
| Per-object vptr cost | None | 8 bytes |
| Shared method body | Duplicated per instantiation | Single copy |
| Inlining potential | Very high | Low (devirtualization required) |
| Dead code elimination | Precise | Coarse-grained |

---

## Methods to Reduce Binary Size in Statically Inherited C++

### 1. Extract Non-Generic Logic into a Non-Template Base

The most impactful technique is the **type-erasure base class** pattern:

```cpp
class BaseImpl {
protected:
    void common_behavior_impl(/* non-type-dependent args */);
};

template <typename Derived>
class Base : public BaseImpl {
public:
    void common_behavior() {
        BaseImpl::common_behavior_impl(/* ... */);
    }
};
```

Any code path in `common_behavior` that does not depend on `Derived`'s type is moved into `BaseImpl`. This code now exists **once** in the binary. This is directly analogous to how `std::vector<T>` implementations often delegate raw memory management to a non-templated `_Vector_base`.

### 2. Explicit Template Instantiation

Declare explicit instantiations in a single translation unit:

```cpp
// base.cpp
template class Base<ConcreteA>;
template class Base<ConcreteB>;
```

And suppress implicit instantiation in headers with `extern template`. This concentrates instantiation, prevents duplicate weak symbols across TUs, and gives the linker a single authoritative definition to retain.

### 3. Link-Time Optimization (LTO) and ICF

Enable **LTO** (`-flto` in GCC/Clang, `/GL` in MSVC). The linker sees the entire program as a single optimization unit, allowing it to:
- Merge **identical code folding (ICF)**: if `Base<ConcreteA>::common_behavior` and `Base<ConcreteB>::common_behavior` compile to identical machine code (common when the derived type's specific method is not called within it), ICF folds them into a single symbol. Gold linker and LLD support this with `--icf=all`.
- Perform whole-program dead-stripping with precision unavailable to per-TU compilers.

### 4. Optimize for Size Compilation Flags

- **`-Os`** (GCC/Clang): Optimize for size, suppressing inlining decisions that increase code size beyond a threshold.
- **`-Oz`** (Clang): More aggressive size optimization than `-Os`, preferring shorter instruction sequences even at minor speed cost.
- **`__attribute__((noinline))`** on specific CRTP methods that are called from many sites but are non-critical to hotpath performance.

### 5. Section Garbage Collection

Compile with `-ffunction-sections -fdata-sections` and link with `--gc-sections`. Each function and data object occupies an independent linker section. Any section unreachable from the entry point is eliminated. This is particularly effective for CRTP code because many specializations may be instantiated by the compiler but never actually called.

### 6. `[[nodiscard]]` and Profile-Guided Optimization (PGO)

PGO (`-fprofile-generate` / `-fprofile-use`) informs the compiler which instantiations are on hot paths. Cold instantiations can be compiled with `-Os` and placed in cold sections, while hot ones retain full `-O3` inlining. This allows nuanced binary layout without a blanket size penalty.

---

## Execution Speed

### Elimination of Indirect Dispatch

A virtual call on modern x86-64 involves:
1. Load vptr from object (`MOV rax, [rcx]`).
2. Load function pointer from vtable (`MOV rax, [rax + offset]`).
3. Indirect call (`CALL rax`).

This is **branch-target indirect**, which the CPU's branch predictor handles poorly when the call site is polymorphic (many derived types flow through). A misprediction on a virtual call costs 15–20 cycles of pipeline flush on modern microarchitectures.

Static inheritance emits a **direct call** or, after inlining, no call at all. The CPU's branch predictor trivially handles direct calls. The result is deterministic, predictable control flow.

### Inlining and Superoptimization

When `Base::interface()` is inlined into the caller and `implementation()` is subsequently inlined into that, the compiler sees the entire call chain as one body. This enables:
- **Constant propagation** across what were previously call boundaries.
- **Auto-vectorization** of loops that span the inlined chain.
- **Register allocation** across the entire fused body, eliminating callee-save/restore overhead.

These effects are unreachable through a vtable dispatch because the compiler cannot see through the indirect call.

### Instruction Cache Pressure

The flip side is that aggressive inlining expands code size at call sites, which increases **i-cache pressure**. In tight loops calling a CRTP method, the inlined body may not fit in L1i. Virtual dispatch, by contrast, keeps the function body in one memory location — once warmed in i-cache, it is shared by all call sites. For workloads with very high call frequency to large methods, virtual dispatch can outperform inlined CRTP due to better cache utilization.

---

## The Compiler's Role

### Frontend: Template Instantiation Engine

The C++ frontend must instantiate `Base<Derived>` for every distinct `Derived`. This is done lazily in most implementations — a template is only instantiated when its member is **odr-used**. The instantiation produces a full AST subtree per specialization, which is type-checked and annotated independently.

### Middle-End: Inlining Heuristics

LLVM's inliner (used by Clang) assigns each function a **cost** based on instruction count, and each call site an **inline threshold** based on optimization level and call frequency. CRTP methods called through `static_cast` are perfectly transparent — the inliner sees a direct call and evaluates it normally. The `always_inline` attribute bypasses the cost model entirely for critical paths.

GCC's inliner (`-finline-functions`) uses a similar model. At `-O2`, the threshold is conservative; at `-O3`, it is significantly relaxed, which is why CRTP code sees the largest size increase at `-O3`.

### Backend: Devirtualization as a Comparison Point

Modern compilers implement **devirtualization** (`-fdevirtualize` in GCC, enabled by default at `-O2`): if the compiler can prove the dynamic type of a pointer (e.g., through escape analysis or whole-program visibility with LTO), it converts a virtual call into a direct call and potentially inlines it. This partially closes the speed gap between virtual and static dispatch, but requires LTO and is not always provable. Static inheritance requires no such proof — dispatch is inherently direct.

### Linker: Symbol Deduplication

Template instantiations are emitted as **weak symbols** (COMDAT groups in ELF, `.linkonce` in older formats). Multiple translation units that independently instantiate `Base<ConcreteA>` each emit a weak copy. The linker deduplicates them, retaining one. This is correct but can significantly increase **link time** and intermediate object file sizes in large codebases. Explicit instantiation declarations (`extern template`) prevent redundant weak emissions, improving build performance.

---

## Summary

Static inheritance in C++ is a compile-time mechanism that trades **binary size** for **execution speed and optimization transparency**. The key insight is that monomorphization — the duplication of template bodies per type parameter — is the price paid for eliminating all runtime dispatch overhead. Mitigating this cost requires deliberately factoring type-independent logic out of templates, leveraging LTO with ICF, and using section GC to strip unreachable instantiations. The compiler is not a passive participant: its inliner, devirtualizer, and optimizer transform the high-level abstraction into machine code that, at its best, is indistinguishable from hand-written non-polymorphic code — with zero runtime overhead for the abstraction layer itself.

---

> [!NOTE]
> 
> Generated by Claude.ai
>
> Model: Sonet 4.6
>
> Prompt: Provide a thorough description of static inheritance in a C++ program. Evaluate the influence of static inheritance on the binary size of the final program. Provide the methods to descrese the size of the binary size. Discuss also the execution speed and the role of the compiler. This description is intended for a computer science expert. 
