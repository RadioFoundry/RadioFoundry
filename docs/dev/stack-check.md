<!-- SPDX-License-Identifier: MIT -->

# Stack check

`make BOARD=<b> stack-check` computes the worst-case stack depth of every FreeRTOS task in a build and fails when a
task's deepest call chain doesn't fit its stack. `tools/ci.sh` runs it on each board's default and maximal
configuration. It exists because an overflow is invisible elsewhere: the host tests run the same code on large pthread
stacks, and on the radio FreeRTOS' overflow hook spins without a word. On the DB25-D a 768-byte buffer in
`codeplug_contact_find` overflowed the start task at boot (radio core set-up, `radio_sm_init` > `tune` >
`dmr_configure` > `find_name`); the check reports that chain 424 bytes over.

On the radio, the debug link's `tasks` view section shows what each task has really used: its least free stack since
it started ([debuglink.md](../spec/debuglink.md) 3.7, `CONFIG_TASK_STACKS`), and `tests/hil/test_task_stacks.py`
checks every task keeps 64 bytes. The check here is the worst case over every path; the view is what the code that ran
so far needed. A task low in the view but fine here points at a call the check doesn't see (a table line).

## How it works

| Input | From |
|---|---|
| Frames and direct calls | GCC's `-fcallgraph-info=su`, always on (`STACK_INFO` in the Makefile): a `.ci` file next to each object, or one per LTO partition next to the `.elf`. It changes no code: the image is byte for byte the same without it |
| Tasks | Every `xTaskCreate(fn, "name", STACK, ...)` in the compiled sources. `STACK` is evaluated with the build's own preprocessor and flags, so `#if` choices count. FreeRTOS' timer task is found the same way; its idle task is in the table |
| Calls through pointers | The sources, below, then the table |
| Code without a call graph | The disassembly of the `.elf` (`objdump`): the C library, the compiler's runtime, assembly. Every stack adjustment in a function is added up and direct calls are followed |

For each task the tool takes the deepest chain from the entry function:

```
free = stack - (deepest chain + port overhead + margin)
```

| Term | Value |
|---|---|
| Port overhead | What the RTOS port keeps on a task's stack besides the chain (`STACK_OVERHEAD` in `platform.mk`). STM32F4: 208 bytes, the exception frame with the FPU's lazy space and alignment (27 words) plus the context switch's r4-r11, lr and s16-s31 (25 words); interrupts then run on the main stack. HR_C7000: 68 bytes, the 17-word frame of an interrupt or a yield; handlers run on the interrupt stack and don't nest |
| Margin | `STACK_MARGIN`, 128 bytes by default |

A task over its stack prints `OVER` and its whole chain with each frame.

### Pointer calls

A call through a function pointer has no target in GCC's graph. The tool reads the call site's source and works out
what the pointer can hold:

| Call | Targets |
|---|---|
| Through a struct field, `board.radio->tx(...)`, `s->cb(...)` | The struct's type is followed from the variable's declaration (a parameter, a local, a file-scope or `extern` variable) through the members. Every function stored into that field of that type: designated or positional initializers, assignments, compound literals |
| Through a variable or parameter | What is assigned to the variable; for a parameter, what callers pass in that position. A call through a struct field passes to the functions stored in that field, and two files' static functions of one name keep their own parameters. Stores of one pointer into another are followed |
| Inside a macro, `LOCK()` | The macro is expanded first |
| In an argument, `f(x, p->g())` | GCC files it at the outer call's position; the tool looks inside the arguments |

A pointer the sources store into but that never holds a function of this build is not called: an optional
`board_t` operation the radio leaves NULL, a callback whose setter nothing in the build calls. `-v` lists these.
A pointer with no store in sight is reported as unresolved. Unresolved calls don't fail the check, but each one could
hide a deeper chain: add it to the table.

### The table

`tools/stack_check_edges.txt` holds what the sources and the graph can't show, for every board in one file:

| Line | Meaning |
|---|---|
| `task FUNCTION STACK` | A task the scan can't find (FreeRTOS' idle task: its entry is a variable) |
| `ptr NAME TARGET...` | Calls through a field, variable or parameter called NAME |
| `call FUNCTION TARGET...` | Every pointer call FUNCTION makes |
| `file PATH TARGET...` | Pointer calls in PATH the sources don't resolve |
| `lib FUNCTION BYTES` | A fixed stack for a function without call graph; also cuts the disassembly walk there |
| `extern NAME BYTES` | Code outside the build, a target written `@NAME` (the stock codec) |
| `recursion FUNCTION...` | Recursion known to be bounded |

The stock AMBE+2 codec runs on the audio task's stack: 4046 bytes measured on the MD-380
([codec-md380.md](../spec/codec-md380.md)), so `stm32_codec_call` counts 4104 with the veneer. The HR_C7000 builds call
the same library by address with 4096 bytes assumed: it isn't measured on C-SKY.

A few C library paths need arguments ODR never passes and are cut with `lib` lines: newlib's `strstr` for needles of
32 characters or more, and the floating point conversions of `printf`. Check the comment in the table before adding a
call that breaks the assumption (a long `strstr` needle, `%f`).

## Reading the report

```
stack check db25d: port overhead 68 bytes, margin 128 bytes, 10 tasks (bytes)
  start    ok   stack  2560  deepest  1892  free   472  (startup_task, core/app/app.c:118)
      startup_task > radio_core_start > radio_sm_init > aprs_init > reload_cfg > load_identity > codeplug_aprs > flas...
  ...
  prvIdleTask ok   stack   512  deepest   140  free   176  (prvIdleTask, table)
      prvIdleTask > prvCheckTasksWaitingTermination > vPortFree > xTaskResumeAll > xTaskIncrementTick > vApplicationT...
  recursion (each cycle counted once): materialize, n_macs, n_receive, n_select, n_selected, n_send, n_tick, n_up, room
  21 functions without a call graph counted as 64 bytes each (-v lists them)
stack check db25d: every task fits, the least free: prvIdleTask 176 bytes
```

| Option | Shows |
|---|---|
| `make ... stack-check STACK_CHECK_ARGS=-v` | Every task's chain with frames and files, the calls resolved by the field name alone (type unknown at the call site), the pointers never set, the library functions taken from the disassembly |
| `STACK_MARGIN=256` | A different margin |
| `STACK_CHECK_REPORT_ONLY=1` | Reports without failing (a board's `config.mk` may set it while its graph is incomplete; none does) |

## Limits

* Recursion is counted once and listed. ODR has no deep recursion; the cycles found are mutual calls through callbacks
  (the network stack's `n_*`, the audio mixer).
* Interrupt handlers aren't checked: they run on the main (STM32) or interrupt (HR_C7000) stack, not a task's.
* The code before the scheduler starts (`main`) runs on the main stack and isn't checked.
* A pointer call whose target is computed at run time (an address from a table the sources don't spell) needs a table
  line.
* The disassembly walk adds every stack adjustment of a function, so a library function with several exit paths
  counts more than it uses. It doesn't see calls through registers in library code.
