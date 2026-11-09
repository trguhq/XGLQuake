# Native syntax-check contracts

These deliberately incomplete, handwritten declarations let a host compiler
parse `xglbench_xgl.c` when Sun XGL and X11 development headers are unavailable.
They are **not SDK headers, not an ABI specification, and not an emulator**.
Enumeration values and opaque object representations are placeholders. No
executable may be linked against these files. The build's normal include paths
never use this directory.

The XGL declarations used by the benchmark were checked against the supplied
Sun XGL Reference Manual 802-5864. This check catches C syntax errors, missing
declarations, misspelled fields and wrong fixed-argument counts. It cannot
verify the real SDK, library linkage, varargs attribute types, runtime behavior,
rendering correctness or hardware acceleration.

From `xglbench/`, a GCC host can run:

```sh
cc -ansi -pedantic -Wall -Wextra -Werror -D__EXTENSIONS__ \
   -Itests/api_contract -I. -fsyntax-only xglbench_xgl.c
```

Compilation against the installed Solaris XGL SDK remains mandatory.
