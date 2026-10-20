# Project coding conventions

These conventions apply throughout this project and may be updated as development proceeds.

- Document all C++ classes and entry points with Doxygen-style comments, beginning with `/**` and using `*` at the start of subsequent lines.
- Place a useful descriptive comment immediately before every function or method definition, including constructors. Header declarations and comments inside the body do not replace this requirement. Use Doxygen-style comments for C++ definitions.
- Document logical paragraphs or blocks of code with `//` comments from the start. Explain their purpose and non-obvious decisions rather than simply repeating the code.
- Separate logical paragraphs with a blank line before standalone comment blocks.
- Update comments whenever the corresponding code changes so they remain accurate.
- Use PascalCase for project-owned C++ class and struct names. Class files match
  the class name (for example, `SdRecorder.h` and `SdRecorder.cpp`). Small nested
  record types may remain with the owning class.
- Prefix every class/struct data member, including static members, with `_`.
  Use a lowercase first letter after the underscore; avoid reserved double
  underscores and underscore-uppercase names.
- Use camelCase for methods, functions, ordinary variables and JavaScript identifiers.
  Name class instances after their type (`SdRecorder sdRecorder`), adding a
  meaningful qualifier when needed (`sdRecorderBackup`). Member instances combine
  both rules (`SdRecorder _sdRecorder`). Collection names may be plural; result
  variables may remain `retVal`. Preserve names required by framework APIs,
  including Arduino's Serial/Serial1/ESP globals and fake SDK interfaces.
- Production startup files construct application services and invoke the real
  application class. Test startup supplies test services through ordinary
  interfaces. Do not instantiate concrete test/profile classes in production
  or duplicate the recording/acquisition implementation.
- Prefer a single return point in each function. For functions that return a value, assign the result to `retVal` and return it at the end. Multiple return points are allowed when a single return point would make the code awkward.
- Do not use `continue` statements.
- Always use curly brackets for `if` statements, including single-statement bodies. Use braces for associated `else` branches as well.
- Keep each C++ class in its own file, using a separate header/source pair where appropriate. Include a descriptive comment at the top of each file.
- Follow other standard coding conventions as appropriate. These guidelines are not exhaustive.

## Required explanation level (confirmed by Karl, 2026-09-30)

- Apply the explanatory requirements below to ALL code, including tests and
  future changes. The original S3 examples were removed with that project;
  `espP4/main/src/RecordingApplication.cpp` documents the current transfer path.
  Do this during implementation; do not wait to be asked again.
- Write for a capable reader who does not already know embedded systems or the
  framework. Explain the execution sequence, who calls each function, what its
  arguments and return value mean, and what success does and does not establish.
- Define hardware and scheduling terms on first use: sample, DRDY/Data Ready,
  edge, ISR/Interrupt Service Routine, task/thread, notification, and similar
  terms. Explain relevant library calls and non-obvious arguments where used.
- Explain why locks, queues, buffers, delays, and timeouts exist; identify which
  task/core owns data, how it is shared, and what happens when an operation fails.
- Explain units, command bytes, flags, counters, reset behavior, and measurement
  limitations. Distinguish a requested action, a queued action, and confirmed
  hardware state. Never describe a diagnostic as proving more than it does.
- Put an overview and a useful reading path at the top of complex files. Use
  function comments plus comments before logical blocks. Avoid jargon-only
  summaries such as "ADC owner" or "complete conversion" without explanation.
- Keep comments proportional: a simple getter needs its meaning and limitations,
  not a long restatement of its code. Comments must describe actual behavior.
- For buffering and data-transfer code, identify the exact call that reads or
  writes the data. Explain pointer arithmetic, wraparound, ownership transfer,
  and synchronization with a concrete example where useful. Labels such as
  "producer", "consumer", "sink", and "publish" need their project-specific
  meaning explained; they are not explanations by themselves.

## Agreement on authorizing work

- Discuss and queue proposed coding changes when Karl says a change is needed.
  Do not implement merely because a discussion identifies a requirement.
- Wait for explicit authorization such as "Go!", "Code!", or an otherwise clear
  instruction to implement. Ask if authorization is unclear. Read-only inspection
  and explanations may proceed while discussing the queue.

## Reusable recording component (requested by Karl, 2026-09-30)

- Encapsulate the disk writer and its ring buffer together as a reusable library.
  Other projects must not need the ADS1256 driver, acquisition class, Feather UART,
  web UI, or their status protocol to use it.
- Keep card pins, SPI controller selection and project-specific recording metadata
  in an adapter/configuration layer. Expose a small data-submission, start/finish,
  and statistics interface; do not expose mutable buffer pointers to callers.
- Submitting data must not perform disk I/O or wait for a write. Retain queued
  memory until its write completes. Report overflow, partial writes and flush
  failures explicitly. Keep maximum write/flush latency and buffer statistics
  inside the component; translate them to the Feather protocol outside it.
- Test buffering and write failures independently of physical hardware. Preserve
  existing card formatting and unrelated files; deletion is an explicit action.

## Hardware verification preference (Karl, 2026-10-06)

- Karl does not have and will not have an oscilloscope. Never ask him to obtain, use, or provide one. Plan verification using documentation, component markings, and software/hardware counters available on the project boards. Explain measurement limitations plainly.
## Working agreement on this PC (Karl, October 17, 2026)

- Karl authorizes source-file edits and firmware uploads after an explicit Go
  or equivalent clear instruction for that work. Do not ask for a second
  conversational confirmation for the same authorized action. Discuss proposed
  changes first; read-only inspection may proceed without Go.
- Karl handles all Git operations. Do not run Git commands, stage, commit, push,
  pull, switch branches, or otherwise change Git state unless he explicitly
  delegates a particular operation.
- For now, Karl runs PowerShell commands himself as a learning exercise. Give
  step-by-step commands and explain their purpose; do not independently build,
  upload, or operate hardware. This preference may change when Karl says so.
  Explicitly requested documentation edits may be performed directly.
- Use plain language, explain unfamiliar terms, and give one manageable step at
  a time. Clearly distinguish PC-side editing/building from board uploads and
  execution. Treat this as a communication preference, not a judgment of ability.
- Tool-enforced filesystem or device permission checks are separate from this
  agreement. Do not claim they have been disabled or bypass them.
