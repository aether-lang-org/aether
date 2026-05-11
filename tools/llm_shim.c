/* tools/llm_shim.c — `ae help --llm <weights.gguf>` inference path.
 *
 * Compiled into ae ONLY when `make ae AETHER_ENABLE_LLM=1`. The
 * default build omits this file entirely — see the Makefile's `ae`
 * recipe and tools/ae_help.c's #ifdef AETHER_ENABLE_LLM gate.
 *
 * This is the on-machine LLM escalation path for issue #415. Privacy
 * contract is enforced by code structure:
 *
 *   - We open exactly two files: the script (read elsewhere) and
 *     the user-supplied weights file (mmap'd by llama.cpp).
 *   - No network calls. We don't link sockets API into this TU; we
 *     never call `socket(2)`, `connect(2)`, or any DNS resolver.
 *     The strace privacy guard in `tests/integration/ae_help_privacy`
 *     enforces this end-to-end.
 *   - No spinning up a server, no "model marketplace", no fetching.
 *     The flag takes a path to an existing file or aborts cleanly.
 *
 * Build-time linkage:
 *   $ make ae AETHER_ENABLE_LLM=1 \
 *       LLM_LDFLAGS="-L/path/to/llama.cpp/build -lllama -lggml -lstdc++ -lm"
 *
 *   Headers live next to the library — pass via standard CPPFLAGS
 *   or set `LLM_CFLAGS="-I/path/to/llama.cpp"` and inject through
 *   make's CFLAGS.
 *
 * The shim targets the stable llama.h C API surface (llama.cpp
 * post-v0.3, current as of 2026). If the upstream API rotates, this
 * file is the only place that needs updating.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* llama.cpp's C header. The build flow expects the user to provide
 * `-I` to its directory. The minimum surface we touch is intentionally
 * tiny — every symbol below is documented in upstream's llama.h. */
#include "llama.h"

/* Forward-declared in tools/ae_help.c. Single entry point. Returns
 * 0 on success (response streamed to `out`), non-zero on error
 * (message already printed to stderr). */
int ae_llm_run(const char* weights_path, const char* prompt, FILE* out);

/* Reasonable defaults for an interactive single-shot prompt. The
 * context window matches a small 3-7B model's training; n_predict
 * caps response length so a misconfigured run doesn't hang. */
#define AE_LLM_CTX_SIZE     2048
#define AE_LLM_N_PREDICT    512
#define AE_LLM_N_THREADS    4

int ae_llm_run(const char* weights_path, const char* prompt, FILE* out) {
    if (!weights_path || !prompt || !out) {
        fprintf(stderr, "ae_llm_run: null argument\n");
        return 1;
    }

    /* Initialise llama.cpp's global state once. Idempotent. */
    llama_backend_init();

    /* Load the user-supplied weights. mmap'd by default — large
     * model files don't actually consume RAM until pages are
     * touched. */
    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;  /* CPU-only; no CUDA assumption. */
    struct llama_model* model = llama_load_model_from_file(weights_path, mparams);
    if (!model) {
        fprintf(stderr, "ae help --llm: failed to load weights from %s\n", weights_path);
        llama_backend_free();
        return 1;
    }

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx     = AE_LLM_CTX_SIZE;
    cparams.n_threads = AE_LLM_N_THREADS;
    cparams.n_threads_batch = AE_LLM_N_THREADS;
    struct llama_context* ctx = llama_new_context_with_model(model, cparams);
    if (!ctx) {
        fprintf(stderr, "ae help --llm: failed to create inference context\n");
        llama_free_model(model);
        llama_backend_free();
        return 1;
    }

    /* Tokenise prompt. -1 for n_tokens means "auto-size by adding a
     * BOS token if appropriate". */
    int prompt_len = (int)strlen(prompt);
    int max_tokens = prompt_len + 16;  /* generous overhead for special tokens */
    llama_token* tokens = malloc((size_t)max_tokens * sizeof(llama_token));
    if (!tokens) {
        fprintf(stderr, "ae help --llm: out of memory tokenising prompt\n");
        llama_free(ctx);
        llama_free_model(model);
        llama_backend_free();
        return 1;
    }
    int n_tok = llama_tokenize(model, prompt, prompt_len,
                                tokens, max_tokens,
                                /*add_special=*/1, /*parse_special=*/0);
    if (n_tok < 0) {
        fprintf(stderr, "ae help --llm: tokenisation failed (rc=%d)\n", n_tok);
        free(tokens);
        llama_free(ctx);
        llama_free_model(model);
        llama_backend_free();
        return 1;
    }

    /* Feed the prompt batch. We use the simple "one batch" path —
     * fine for short prompts (our diagnostics summaries are
     * bounded). */
    struct llama_batch batch = llama_batch_init(n_tok, 0, 1);
    for (int i = 0; i < n_tok; i++) {
        batch.token[i]    = tokens[i];
        batch.pos[i]      = i;
        batch.seq_id[i][0] = 0;
        batch.n_seq_id[i]  = 1;
        batch.logits[i]   = (i == n_tok - 1) ? 1 : 0;
    }
    batch.n_tokens = n_tok;

    if (llama_decode(ctx, batch) != 0) {
        fprintf(stderr, "ae help --llm: prompt decode failed\n");
        llama_batch_free(batch);
        free(tokens);
        llama_free(ctx);
        llama_free_model(model);
        llama_backend_free();
        return 1;
    }

    /* Generate response token-by-token. Greedy sampling for
     * determinism — no `top_k` / temperature randomness here since
     * a diagnostic helper should be reproducible. */
    int n_gen = 0;
    int cur_pos = n_tok;
    int rc = 0;
    while (n_gen < AE_LLM_N_PREDICT) {
        const float* logits = llama_get_logits_ith(ctx, batch.n_tokens - 1);
        if (!logits) { rc = 1; break; }

        /* Pick the highest-probability token. */
        const int vocab_size = llama_n_vocab(model);
        int best = 0;
        float best_p = logits[0];
        for (int i = 1; i < vocab_size; i++) {
            if (logits[i] > best_p) { best_p = logits[i]; best = i; }
        }

        if (best == llama_token_eos(model)) break;

        /* Detokenise + stream to `out`. */
        char piece[256];
        int pl = llama_token_to_piece(model, best, piece, (int)sizeof(piece), 0, 0);
        if (pl > 0) {
            fwrite(piece, 1, (size_t)pl, out);
            fflush(out);
        }

        /* Feed the chosen token back. */
        batch.n_tokens = 1;
        batch.token[0]    = best;
        batch.pos[0]      = cur_pos;
        batch.seq_id[0][0] = 0;
        batch.n_seq_id[0]  = 1;
        batch.logits[0]   = 1;
        if (llama_decode(ctx, batch) != 0) { rc = 1; break; }
        cur_pos++;
        n_gen++;
    }
    fputc('\n', out);

    llama_batch_free(batch);
    free(tokens);
    llama_free(ctx);
    llama_free_model(model);
    llama_backend_free();
    return rc;
}
