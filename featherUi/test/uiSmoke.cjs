// Stable entry point for the browser checks. The required module runs its suite
// immediately and reports failure with a nonzero process exit code. It intercepts
// requests locally, so running this does not operate connected boards.
require("./controlsSmoke.cjs");
