/*
 * Aether Language Support — VS Code / Cursor extension entry point.
 *
 * Job: when a `.ae` file is opened, locate the `aether-lsp` binary and
 * start a language-server-protocol client against it. The user shouldn't
 * have to wire anything up by hand — this resolver tries every place
 * an `aether-lsp` realistically lives, in order, and only falls back to
 * a status-bar warning if none are present.
 *
 * Resolution order:
 *   1. `aether.lsp.path` setting (explicit override always wins).
 *   2. The current workspace's `build/aether-lsp` — the common case for
 *      anyone working in the Aether repo itself; building the LSP is
 *      `make lsp` in that workspace, and the resulting binary belongs
 *      to that very tree.
 *   3. `aether-lsp` resolved through PATH (covers system installs).
 *   4. Hardcoded common install dirs (`~/.local/bin`, `~/.aether/bin`,
 *      `/usr/local/bin`, `/opt/homebrew/bin`) for shells that don't
 *      have those directories on PATH (notably non-interactive Cursor
 *      child processes on some macOS configurations).
 *
 * Disabling: set `aether.lsp.enable: false` for syntax-only mode.
 */

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

const EXE_SUFFIX = process.platform === 'win32' ? '.exe' : '';
// Two valid LSP entry points, preferred in order:
//   1. `aetherc lsp` — the language server is now embedded in the
//      compiler binary (issue #327). One toolchain binary; one
//      version guarantee; one PATH probe.
//   2. `aether-lsp` — the standalone binary, kept as a transitional
//      alias so editor configs that hardcoded the name keep working.
const AETHERC_NAME      = 'aetherc' + EXE_SUFFIX;
const AETHER_LSP_NAME   = 'aether-lsp' + EXE_SUFFIX;
const OUTPUT_CHANNEL_NAME = 'Aether Language Server';

// ---------------------------------------------------------------------------
// Resolver: locate the LSP entry point without per-machine configuration.
// ---------------------------------------------------------------------------

function isExecutable(p: string): boolean {
    try {
        const st = fs.statSync(p);
        if (!st.isFile()) return false;
        if (process.platform === 'win32') return true;
        return (st.mode & 0o111) !== 0;
    } catch {
        return false;
    }
}

function expandHome(p: string): string {
    if (p === '~' || p.startsWith('~/')) return path.join(os.homedir(), p.slice(2));
    return p;
}

// Probe a list of candidate directories for a given binary name. Returns
// the first existing executable, or undefined.
function findIn(dirs: readonly string[], name: string): string | undefined {
    for (const dir of dirs) {
        if (!dir) continue;
        const candidate = path.join(dir, name);
        if (isExecutable(candidate)) return candidate;
    }
    return undefined;
}

function pathDirs(): string[] {
    const PATH = process.env.PATH ?? '';
    const sep = process.platform === 'win32' ? ';' : ':';
    return PATH.split(sep);
}

function workspaceBuildDirs(): string[] {
    const out: string[] = [];
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        out.push(path.join(folder.uri.fsPath, 'build'));
    }
    return out;
}

function commonInstallDirs(): string[] {
    const home = os.homedir();
    return [
        path.join(home, '.local', 'bin'),
        path.join(home, '.aether', 'bin'),
        '/usr/local/bin',
        '/opt/homebrew/bin',
    ];
}

interface ResolvedLsp {
    /** Executable to invoke. */
    binary: string;
    /** Args to pass — `["lsp"]` for the subcommand entry, `[]` for the standalone. */
    args: string[];
    /** Human-readable origin for the output-channel announcement. */
    source: string;
}

function resolveLspBinary(): ResolvedLsp | undefined {
    const config = vscode.workspace.getConfiguration('aether');
    const explicit = (config.get<string>('lsp.path') ?? '').trim();

    if (explicit) {
        const expanded = expandHome(explicit);
        const absolute = path.isAbsolute(expanded) ? expanded : expanded;
        if (isExecutable(absolute)) {
            // Inferring whether to pass `lsp` from the basename: if the
            // user pointed at `aetherc`, run the subcommand; otherwise
            // (`aether-lsp` or any other) pass no args.
            const base = path.basename(absolute).toLowerCase();
            const isAetherc = base === AETHERC_NAME.toLowerCase() ||
                              base.replace(/\.exe$/, '') === 'aetherc';
            return {
                binary: absolute,
                args: isAetherc ? ['lsp'] : [],
                source: 'aether.lsp.path setting',
            };
        }
        vscode.window.showWarningMessage(
            `aether.lsp.path is set to "${explicit}" but no executable was found there. ` +
            `Falling back to auto-detection.`,
        );
    }

    // Probe each location for both names, preferring `aetherc lsp` (the
    // single-binary path) over the standalone `aether-lsp`.
    const probeOrder: { dirs: string[], label: string }[] = [
        { dirs: workspaceBuildDirs(), label: 'workspace build/' },
        { dirs: pathDirs(),           label: 'PATH'              },
        { dirs: commonInstallDirs(),  label: 'common install dir' },
    ];

    for (const { dirs, label } of probeOrder) {
        const aetherc = findIn(dirs, AETHERC_NAME);
        if (aetherc) {
            return { binary: aetherc, args: ['lsp'], source: `${label} (aetherc lsp)` };
        }
        const standalone = findIn(dirs, AETHER_LSP_NAME);
        if (standalone) {
            return { binary: standalone, args: [], source: `${label} (aether-lsp)` };
        }
    }

    return undefined;
}

// ---------------------------------------------------------------------------
// Activation.
// ---------------------------------------------------------------------------

function showLspMissingMessage(channel: vscode.OutputChannel): void {
    const msg =
        'Aether language server (aether-lsp) was not found. ' +
        'Build it with `make lsp` in the Aether repo, or set `aether.lsp.path` ' +
        'to point at the binary.';
    channel.appendLine(`[aether] ${msg}`);
    vscode.window.showWarningMessage(msg, 'Open Settings').then((choice) => {
        if (choice === 'Open Settings') {
            vscode.commands.executeCommand(
                'workbench.action.openSettings',
                'aether.lsp.path',
            );
        }
    });
}

// Start (or restart) the language-server client against the resolved
// binary. Pure LSP wiring — it never touches the output channel's
// lifetime or the config-change listener, so it is safe to call
// repeatedly (on activation and on every aether.lsp.* change) without
// leaking channels or listeners. Failure to find or start the server is
// reported but NEVER thrown: the grammar + file icon are declarative
// contributions that stand on their own, so syntax-only mode is always
// available even if the server is missing.
async function startClient(channel: vscode.OutputChannel): Promise<void> {
    const config = vscode.workspace.getConfiguration('aether');
    const enabled = config.get<boolean>('lsp.enable', true);
    if (!enabled) {
        channel.appendLine('[aether] LSP disabled via aether.lsp.enable; syntax-only mode.');
        return;
    }

    const resolved = resolveLspBinary();
    if (!resolved) {
        showLspMissingMessage(channel);
        return;
    }

    const argDisplay = resolved.args.length ? ` ${resolved.args.join(' ')}` : '';
    channel.appendLine(`[aether] Using LSP server: ${resolved.binary}${argDisplay} (${resolved.source}).`);

    const serverOptions: ServerOptions = {
        run:   { command: resolved.binary, args: resolved.args, transport: TransportKind.stdio },
        debug: { command: resolved.binary, args: resolved.args, transport: TransportKind.stdio },
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: 'file',     language: 'aether' },
            { scheme: 'untitled', language: 'aether' },
        ],
        outputChannel: channel,
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.ae'),
        },
        initializationOptions: {
            clientName: 'vscode-aether',
        },
    };

    client = new LanguageClient(
        'aether',
        'Aether Language Server',
        serverOptions,
        clientOptions,
    );

    try {
        await client.start();
        channel.appendLine('[aether] Language server connected.');
    } catch (err) {
        channel.appendLine(`[aether] Failed to start language server: ${err}`);
        vscode.window.showErrorMessage(
            `Aether language server failed to start: ${err instanceof Error ? err.message : String(err)}`,
        );
        client = undefined;
    }
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    // Channel and the config-change listener are created ONCE per
    // activation and torn down with the extension. Earlier this function
    // re-invoked itself on every config change, which created a fresh
    // channel and registered another config listener each time — leaking
    // listeners and spawning duplicate LSP clients over a session. The
    // restart now reuses this channel and only cycles the client.
    const channel = vscode.window.createOutputChannel(OUTPUT_CHANNEL_NAME);
    context.subscriptions.push(channel);

    // Settings change → restart just the client (not the whole extension),
    // so the output channel and this very listener are never duplicated.
    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration(async (event) => {
            if (
                event.affectsConfiguration('aether.lsp.path') ||
                event.affectsConfiguration('aether.lsp.enable')
            ) {
                channel.appendLine('[aether] Configuration changed; restarting language server.');
                await deactivate();
                await startClient(channel);
            }
        }),
    );

    await startClient(channel);
}

export async function deactivate(): Promise<void> {
    if (!client) return;
    try {
        await client.stop();
    } catch {
        /* swallow — extension shutdown is best-effort */
    } finally {
        client = undefined;
    }
}
