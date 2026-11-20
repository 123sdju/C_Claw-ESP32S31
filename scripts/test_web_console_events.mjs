// Execute the actual embedded page's event logic, without browser/network/device access.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('../main/transports/web_console.c', import.meta.url), 'utf8');
const block = source.match(/static const char INDEX_HTML\[\] =\s*([\s\S]*?);\r?\n/)[1];
const html = block.split(/\r?\n/).filter(line => line.startsWith('"'))
  .map(line => JSON.parse(line)).join('');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1]
  .replace('refreshAll();loadConfig();setInterval(refreshEvents,2000);', '');
const event = seq => ({ seq, source: 'runtime', kind: 'assistant', level: 'info', text: `event ${seq}` });
const response = (latest, seqs, extra = {}) => ({ latest_seq: latest, oldest_seq: 1,
  clear_generation: 0, events: seqs.map(event), ...extra });
function page(fetch = async () => { throw new Error('unexpected fetch'); }) {
  const context = vm.createContext({ events: { textContent: '' }, eventMeta: { textContent: '' }, fetch });
  vm.runInContext(script, context);
  return { context, run: code => vm.runInContext(code, context),
    render: data => { context.input = data; vm.runInContext('renderEvents(input)', context); } };
}

const p = page();
p.render(response(2, [1, 2]));
p.render(response(3, [2, 3]));
assert.equal(p.run('cache.map(x=>x.seq).join(",")'), '1,2,3');
assert.equal(p.run('since'), 3);
p.render(response(8, [7, 8], { oldest_seq: 7, gap: true }));
assert.equal(p.run('cache.map(x=>x.seq).join(",")'), '7,8');
assert.match(p.context.eventMeta.textContent, /overwritten/);
p.render(response(10, [9, 10]));
assert.match(p.context.eventMeta.textContent, /overwritten/); // don't silently erase loss notice
p.render(response(20, [19, 20], { truncated: true, omitted: 8 }));
assert.equal(p.run('cache.length'), 2);
assert.match(p.context.eventMeta.textContent, /truncated/);
p.render(response(0, [], { cursor_reset: true }));
assert.equal(p.run('since'), 0);
assert.equal(p.run('cache.length'), 0);
p.render(response(1, [1]));
assert.equal(p.run('since'), 1);

// A clear can leave the sequence unchanged and must still invalidate every
// connected browser's cached display, without relying on a cursor gap.
const clear = page();
clear.render(response(2, [1, 2]));
clear.render(response(2, [], { clear_generation: 1 }));
assert.equal(clear.run('cache.length'), 0);
assert.equal(clear.run('since'), 2);
clear.render(response(3, [3], { clear_generation: 1 }));
assert.equal(clear.run('cache.map(x=>x.seq).join(",")'), '3');
assert.match(clear.context.eventMeta.textContent, /display cleared/);

// A selected history page is separate from the rolling event cache. A late
// client or a cursor gap can restore it even after its events were overwritten.
const history = page();
history.render(response(3, [1, 2, 3], { view: [event(1), event(2)], view_seq: 2 }));
assert.equal(history.run('viewCache.map(x=>x.seq).join(",")'), '1,2');
assert.equal(history.run('cache.map(x=>x.seq).join(",")'), '3');
history.render(response(2002, [2001, 2002], {
  oldest_seq: 1000, gap: true, view: [event(1), event(2)], view_seq: 2
}));
assert.equal(history.run('viewCache.map(x=>x.seq).join(",")'), '1,2');
assert.equal(history.run('cache.map(x=>x.seq).join(",")'), '2001,2002');
history.render(response(2003, [2003], { oldest_seq: 1000, view: null, view_seq: 2 }));
assert.equal(history.run('viewCache.length'), 2);
assert.equal(history.run('cache.map(x=>x.seq).join(",")'), '2001,2002,2003');
history.render(response(2004, [2004], {
  oldest_seq: 2004, clear_generation: 1, view: [event(2004)], view_seq: 2004
}));
assert.equal(history.run('viewCache.map(x=>x.seq).join(",")'), '2004');
assert.equal(history.run('cache.length'), 0);
history.render(response(2004, [], { clear_generation: 2, view: null, view_seq: 0 }));
assert.equal(history.run('viewCache.length + cache.length'), 0);
assert.throws(() => history.render(response(5, [], { view: [], view_seq: 6 })), /invalid event response/);

assert.throws(() => p.render({ error: 'unavailable' }), /invalid event response/);
assert.equal(p.run('since'), 1);

let calls = 0;
let release;
const q = page(() => { calls++; return new Promise(resolve => { release = resolve; }); });
const first = q.run('refreshEvents()');
await q.run('refreshEvents()');
assert.equal(calls, 1); // interval and manual refresh share a single in-flight request
release({ ok: true, json: async () => response(4, [3, 4]) });
await first;
assert.equal(q.run('since'), 4);
q.context.fetch = async () => ({ ok: false, status: 503 });
await q.run('refreshEvents()');
assert.equal(q.run('since'), 4);
assert.equal(q.run('eventLoading'), false);
assert.match(q.context.eventMeta.textContent, /HTTP 503/);
q.context.fetch = async () => ({ ok: true, json: async () => response(5, [5]) });
await q.run('refreshEvents()');
assert.equal(q.run('since'), 5);

// The startup page shows loading/repair state and reloads when interaction starts.
const startupBlock = source.match(/static const char STARTUP_HTML\[\] =\s*([\s\S]*?);\r?\n/)[1];
const startupHtml = startupBlock.split(/\r?\n/).filter(line => line.startsWith('"'))
  .map(line => JSON.parse(line)).join('');
const startupScript = startupHtml.match(/<script>([\s\S]*?)<\/script>/)[1]
  .replace('refreshStartup();setInterval(refreshStartup,2000);', '');
let phase = 'loading', startupMessage = '正在读取 U 盘配置', interactionStarted = false, reloads = 0;
const bootElements = { state: { textContent: '' }, events: { textContent: '' } };
const boot = vm.createContext({
  document: { getElementById(id) { return bootElements[id]; } },
  location: { reload() { reloads++; } },
  fetch: async (url) => {
    if (url === '/api/status') return { ok: true, json: async () => ({ phase, message: startupMessage, interaction: { started: interactionStarted } }) };
    if (url.startsWith('/api/events')) return { ok: true, json: async () => ({
      view: [{ seq: 1, text: '正在读取 U 盘配置' }], view_seq: 1, events: [{ seq: 1, text: 'duplicate' }]
    }) };
    throw new Error('unexpected startup request');
  }
});
vm.runInContext(startupScript, boot);
await vm.runInContext('refreshStartup()', boot);
assert.equal(bootElements.events.textContent, '正在读取 U 盘配置');
assert.equal(bootElements.state.textContent, startupMessage);
phase = 'nvs_repair'; interactionStarted = true; startupMessage = '设备 NVS 存储初始化失败；数据已保留，请修复设备存储。';
await vm.runInContext('refreshStartup()', boot);
assert.equal(bootElements.state.textContent, startupMessage);
assert.equal(reloads, 0);
phase = 'running';
await vm.runInContext('refreshStartup()', boot);
assert.equal(reloads, 1);

console.log('Web event regression: append/deduplicate, gap/truncation notice, reset-to-zero,');
console.log('invalid response, single-flight polling, startup loading and recovery: PASS');
