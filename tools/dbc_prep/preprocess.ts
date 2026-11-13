// Preprocess a Berry script (.be -> .bep) exactly like the web UI's Save:
// same preprocessor (webui/src/lib/preprocessor.ts), same DBC parser, DBCs
// concatenated in bus order (bus 0 first), as ScriptsView.svelte does.
//
//   node tools/dbc_prep/preprocess.ts script.be out.bep dbc/Model3_VEH.dbc [dbc/Model3_CH.dbc ...]
//
// Exit 1 on any preprocessor error (unknown message/signal, bad call ...).
import { readFileSync, writeFileSync } from 'node:fs';
import { parseDbc } from '../../webui/src/dbc/parser.ts';
import { preprocessScript } from '../../webui/src/lib/preprocessor.ts';

const [, , src, out, ...dbcs] = process.argv;
if (!src || !out || dbcs.length === 0) {
  console.error('usage: preprocess.ts <script.be> <out.bep> <bus0.dbc> [bus1.dbc ...]');
  process.exit(2);
}
const messages = dbcs.flatMap((f) => parseDbc(readFileSync(f, 'utf8')));
const code = readFileSync(src, 'utf8');
const r = preprocessScript(code, messages);
for (const w of r.warnings) console.error(`warning ${w.code}: ${w.message}`);
for (const e of r.errors) console.error(`error ${e.code}: ${e.message}`);
if (r.errors.length) process.exit(1);
writeFileSync(out, r.code);
console.error(`ok: ${src} -> ${out} (${r.code.length} bytes, ${messages.length} DBC messages)`);
