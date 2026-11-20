// Test the actual embedded OTA JavaScript without contacting or flashing a board.
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { createHash, webcrypto } from 'node:crypto';
import vm from 'node:vm';

const source = readFileSync(new URL('../main/transports/web_console.c', import.meta.url), 'utf8');
const block = source.match(/static const char INDEX_HTML\[\] =\s*([\s\S]*?);\r?\n/)[1];
const html = block.split(/\r?\n/).filter(line => line.startsWith('"'))
  .map(line => JSON.parse(line)).join('');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1]
  .replace('refreshAll();loadConfig();setInterval(refreshEvents,2000);', '');
const reference = bytes => createHash('sha256').update(bytes).digest('hex');
const buffer = bytes => Uint8Array.from(bytes).buffer;
const context = vm.createContext({ setTimeout }); // no crypto: ordinary LAN HTTP
vm.runInContext(script, context);
async function digest(bytes) {
  context.input = buffer(bytes);
  return vm.runInContext('otaSha256(input)', context);
}
assert.equal(await digest([]), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
assert.equal(await digest(Buffer.from('abc')), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
for (const size of [1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129, 1048577]) {
  const bytes = Uint8Array.from({ length: size }, (_, i) => (i * 37 + 19) & 255);
  assert.equal(await digest(bytes), reference(bytes), `fallback length ${size}`);
}
const million = Buffer.alloc(1000000, 'a');
assert.equal(await digest(million), 'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0');
context.crypto = { subtle: undefined };
assert.equal(await digest(million), reference(million));
context.crypto = webcrypto;
assert.equal(await digest(million), reference(million));
context.crypto = { subtle: { digest: async () => { throw new Error('unavailable'); } } };
assert.equal(await digest(million), reference(million));
delete context.crypto;
const firmwarePath = new URL('../build/CClaw.bin', import.meta.url);
if (existsSync(firmwarePath)) {
  const bytes = readFileSync(firmwarePath);
  assert.equal(await digest(bytes), reference(bytes));
  console.log(`Existing firmware SHA-256 matched Node crypto (${bytes.length} bytes)`);
}

const bytes = Buffer.from([0xe9, 0, 128, 255, 1, 2, 3]);
const file = { name: 'CClaw.bin', size: bytes.length, arrayBuffer: async () => buffer(bytes) };
const requests = [];
let sent;
Object.assign(context, {
  otaFile: { files: [file] }, otaProgress: {}, otaProgressText: {}, ota: {},
  alert: message => { throw new Error(message); },
  fetch: async (url, options) => {
    requests.push({ url, options });
    return { ok: true, text: async () => '{}' };
  },
  XMLHttpRequest: class {
    upload = {};
    open(method, url) { requests.push({ url, options: { method } }); }
    setRequestHeader(name, value) { requests.at(-1).options.headers = { [name]: value }; }
    send(body) { sent = body; }
  },
});
await vm.runInContext('uploadOta()', context);
assert.equal(requests.length, 2);
for (const [i, request] of requests.entries()) {
  const url = new URL(request.url, 'http://192.168.4.1');
  assert.equal(url.pathname, i === 0 ? '/api/ota/prepare' : '/api/ota/upload');
  assert.equal(url.searchParams.get('sha256'), reference(bytes));
  assert.equal(request.options.method, 'POST');
  assert.equal(request.options.headers?.['X-CClaw-Token'], undefined);
}
assert.equal(sent, file);
requests.length = 0;
file.arrayBuffer = async () => { throw new Error('file read failed'); };
await vm.runInContext('uploadOta()', context);
assert.equal(requests.length, 0);
assert.match(context.otaProgressText.textContent, /hashing failed: file read failed/);
console.log('OTA SHA-256: vectors, padding boundaries, native/fallback, HTTP upload and read failure: PASS');
