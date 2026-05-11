/* ae_help.h — `ae help <script.ae>` companion command.
 *
 * Surface: `ae help <script.ae> [--fix] [--json] [--llm <gguf>]`
 *
 * Offline, on-machine diagnostic for closure-DSL config-IS-code
 * scripts. Issue #414 (heuristic core) + #415 (optional LLM
 * escalation). Privacy invariants:
 *
 *   - No network calls. Ever.
 *   - No file reads outside the script, resolvable imports, and
 *     co-located `<name>.help.md` hint files.
 *   - No execution of the script. Static analysis only.
 *   - Output is local stdout; no logs, no caching of script body.
 *
 * Design: `docs/cic-help.md`.
 *
 * Wiring: `ae` parses `help <path>` and forwards to
 * `ae_help_main(argc, argv)`. Bare `ae help` (no path) falls through
 * to the existing CLI usage banner in `tools/ae.c`.
 */
#ifndef AE_HELP_H
#define AE_HELP_H

/* Argv passed in starts with the script path (or first flag).
 * The dispatcher in tools/ae.c strips the leading `help`. */
int ae_help_main(int argc, char** argv);

/* Disambiguator: returns true iff `argv0` looks like a script
 * path the help command should claim (ends in `.ae`, file exists).
 * Used by the dispatcher so bare `ae help` keeps printing usage. */
int ae_help_is_script_target(const char* arg);

#endif
