// Runs the shipped page code; no device requests or firmware compilation.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('../main/transports/web_console.c', import.meta.url), 'utf8');
const block = source.match(/static const char INDEX_HTML\[\] =\s*([\s\S]*?);\r?\n/)[1];
const html = block.split(/\r?\n/).filter(line => line.startsWith('"'))
  .map(line => JSON.parse(line)).join('');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1]
  .replace('refreshAll();loadConfig();setInterval(refreshEvents,2000);', '');
const requests = [];
const context = vm.createContext({
  fetch: async (url, options) => {
    requests.push({ url, options });
    return { ok: true, status: 200, text: async () => '{}', json: async () => ({}) };
  },
});
vm.runInContext(script, context);
await vm.runInContext("webFetch('/api/status')", context);
assert.equal(requests.at(-1).url, '/api/status');
await vm.runInContext("webFetch('/api/file',{method:'PUT',headers:{'Content-Type':'text/plain'},body:'hello'})", context);
assert.equal(requests.at(-1).options.headers['Content-Type'], 'text/plain');
assert.equal(requests.at(-1).options.headers['X-CClaw-Token'], undefined);
assert.equal(requests.at(-1).options.body, 'hello');
// Only the common wrapper may use raw fetch; OTA XHR has its own header test.
assert.equal((script.match(/\bfetch\(/g) || []).length, 1);
assert.doesNotMatch(source, /X-CClaw-Token|webToken|connectWeb|\/web-token|401 Unauthorized|authorized_handler/);
assert.match(source, /\.handler = handler/);
console.log('Web console: direct API access, no login/token dependency, request body preserved: PASS');
