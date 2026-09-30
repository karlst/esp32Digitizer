# Project coding conventions

These conventions apply throughout this project and may be updated as development proceeds.

- Document all C++ classes and entry points with Doxygen-style comments, beginning with `/**` and using `*` at the start of subsequent lines.
- Place a useful descriptive comment immediately before every function or method definition, including constructors. Header declarations and comments inside the body do not replace this requirement. Use Doxygen-style comments for C++ definitions.
- Document logical paragraphs or blocks of code with `//` comments from the start. Explain their purpose and non-obvious decisions rather than simply repeating the code.
- Update comments whenever the corresponding code changes so they remain accurate.
- Use camelCase for C++ and JavaScript identifiers.
- Prefer a single return point in each function. For functions that return a value, assign the result to `retVal` and return it at the end. Multiple return points are allowed when a single return point would make the code awkward.
- Do not use `continue` statements.
- Always use curly brackets for `if` statements, including single-statement bodies. Use braces for associated `else` branches as well.
- Keep each C++ class in its own file, using a separate header/source pair where appropriate. Include a descriptive comment at the top of each file.
- Follow other standard coding conventions as appropriate. These guidelines are not exhaustive.

## Required explanation level (confirmed by Karl, 2026-09-30)

- Use the explanatory level approved in `s3Recorder/src/ads1256.cpp` and
  `s3Recorder/src/acquisition.cpp` as the baseline for ALL project code, including
  future changes. Do this during implementation; do not wait to be asked again.
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

## Agreement on authorizing work

- Discuss and queue proposed coding changes when Karl says a change is needed.
  Do not implement merely because a discussion identifies a requirement.
- Wait for explicit authorization such as "Go!", "Code!", or an otherwise clear
  instruction to implement. Ask if authorization is unclear. Read-only inspection
  and explanations may proceed while discussing the queue.
