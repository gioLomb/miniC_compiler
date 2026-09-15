miniC Compiler (minicc)

A teaching-oriented compiler for a restricted C subset (miniC).
It takes a .c source file, runs a full front-end + middle-end + back-end pipeline, and emits x86-64 AT&T assembly.

The project is designed for compiler courses: register allocation with spilling, instruction scheduling, local/global value numbering, loop optimisations, and so on are all implemented and can be inspected with the debug flags.



Quick start

# Build (expects the usual layout with scanner/, parser/, …)
make

# Compile a miniC program to assembly
./bin/minicc program.c -S -o program.s

# Inspect intermediate stages (parse tree, IR, etc.)
./bin/minicc program.c          # no -S → pretty-prints AST + IR

# Debug the back-end
./bin/minicc program.c -S -d -o program.s   # dumps pre/post-scheduling asm

You can then assemble and link the generated assembly with the system toolchain:

gcc -no-pie program.s -o program
./program



Command-line interface

minicc <file.c> [-S] [-d] [-o <output>]







Flag



Meaning





(none)



Run front-end + optimisations, print AST and linear IR, then exit





-S



Emit x86-64 AT&T assembly





-d



Debug mode: also print assembly before and after instruction scheduling





-o file



Write output to file instead of stdout



The miniC language

miniC is a deliberately small subset of C. The goal is to keep the language simple enough that every pass of the compiler stays understandable, while still exercising non-trivial optimisations and a realistic back-end.

Supported features







Category



What is allowed





Types



int, float





Variables



Locals, parameters, global scalars and global arrays





Control flow



if / else, while, return





Expressions



Arithmetic (+ - * / %), comparisons (< > <= >= == !=), logical (&& || !), assignment





Functions



Definitions and calls (see parameter limit below)





Arrays



Global arrays only; indexing with integer expressions





Literals



Integer and floating-point constants

Important constraints / limitations

These are hard limits of the current implementation (mostly coming from the x86-64 System V ABI and the instruction selector):





At most 6 integer parameters per function (NUM_ARG_REGS).
Extra integer arguments are rejected with a diagnostic.
Float parameters are limited to the 8 XMM argument registers.



No for loops in the concrete syntax that reaches the back-end.
The token exists, but the recommended (and tested) style is to rewrite loops as while.



No pointers, no pointer arithmetic, no struct, no union, no enum.
An array name used without [] is rejected (it would decay to a pointer in real C).



No switch, break, continue, goto.



No char, void return values in the usual sense, no function pointers.



No dynamic allocation (malloc / free are not part of the language).



Global arrays are supported; local arrays are not.



Nested functions are not allowed.

Programs that respect the constraints above (see spill_heavy.c and stress_test.c for realistic examples) are expected to compile cleanly.

Minimal example

int add(int a, int b) {
    return a + b;
}

int main() {
    int x;
    int y;
    x = 10;
    y = 20;
    return add(x, y);
}

./bin/minicc example.c -S -o example.s
gcc -no-pie example.s -o example
./example ; echo $?    # → 30



Compilation pipeline

When you run minicc file.c -S the following stages are executed in order:

Source
  ↓  lexer (re2c-generated scanner)
  ↓  recursive-descent parser          → AST
  ↓  semantic analysis + symbol tables
  ↓  AST optimisations
  ↓  IR generation                     → three-address code
  ↓  middle-end optimisations
       • SVN  (local/global value numbering)
       • DCE  (dead-code elimination)
       • CP   (copy propagation)
       • LICM (loop-invariant code motion)
       • SR   (strength reduction)
       • …
  ↓  instruction selection             → MachInstr (x86-64)
  ↓  instruction scheduling
  ↓  register allocation
       • liveness
       • interference graph
       • coalescing
       • colouring (Chaitin-Briggs style)
       • spill / reload insertion (iterative)
  ↓  assembly emission                 → AT&T syntax

Without -S the compiler stops after IR generation and pretty-prints the AST and the linear IR, which is useful while debugging the front-end.



Building from source

make              # builds bin/minicc and the unit-test binaries
make check        # runs the test suite
make check-backend
make clean

Optional AddressSanitizer / UBSan build:

make SANITIZE=1

The Makefile expects the conventional directory layout (scanner/, parser/, tests/, …).
If you are working from a flattened tree you will need to adjust the include paths or restore the original hierarchy.



Testing & stress inputs

Two hand-crafted programs are provided to exercise the register allocator and the middle-end:





spill_heavy.c – many simultaneously-live scalars (forces spills) plus repeated sub-expressions (SVN candidates). Also contains a float variant.



stress_test.c – large generated program that stresses parsing, IR, optimisations and register allocation under the 6-parameter limit.

Both should compile to assembly with:

./bin/minicc spill_heavy.c -S -o /tmp/spill.s
./bin/minicc stress_test.c -S -o /tmp/stress.s
