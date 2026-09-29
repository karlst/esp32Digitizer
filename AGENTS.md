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
