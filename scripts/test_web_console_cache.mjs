// Exercise the actual embedded page and its interpretation of observed usage.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(new URL('../main/transports/web_console.c', import.meta.url), 'utf8');
const block = source.match(/static const char INDEX_HTML\[\] =\s*([\s\S]*?);\r?\n/)[1];
const html = block.split(/\r?\n/).filter(line => line.startsWith('"'))
  .map(line => JSON.parse(line)).join('');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1]
  .replace('refreshAll();loadConfig();setInterval(refreshEvents,2000);', '');
const status = { textContent: '' }, cacheMetrics = { textContent: '' };
let statusResponse;
const context = vm.createContext({ status, cacheMetrics,
  events: { textContent: '' }, eventMeta: { textContent: '' },
  fetch: async url => {
    if (url === '/api/status') return { text: async () => JSON.stringify(statusResponse) };
    if (url.startsWith('/api/events')) return { ok: true, json: async () => ({
      latest_seq: 0, oldest_seq: 0, events: []
    }) };
    throw new Error(`unexpected request ${url}`);
  }
});
vm.runInContext(script, context);
const format = value => { context.metricInput = value; return vm.runInContext('formatCacheMetrics(metricInput)', context); };

assert.equal(format(null), '缓存观测数据不可用');
assert.match(format({ models: [] }), /尚未观察到模型用量事件/);
const model = { provider: 'provider-a', model: 'model-1', usage_events: 6,
  cache_unreported: 1, cache_unusable: 2, cache_measured: 3,
  prompt_tokens: 400, cached_tokens: 75, cache_hit_basis_points: 1875,
  token_totals_saturated: false, cache_write_reported: 3,
  cache_write_unreported: 2, cache_write_unusable: 1, cache_write_tokens: 40,
  cache_write_tokens_saturated: false };
const metrics = { models: [model], model_events_omitted: 2,
  summary_cache: { hit: 1, prefix_hit: 2, miss: 3, rejected: 4 } };
let rendered = format(metrics);
assert.match(rendered, /provider-a \/ model-1/);
assert.match(rendered, /18\.75%（75 \/ 400）/);
assert.match(rendered, /可计量 \/ 已观察用量事件：3 \/ 6；未报告：1；无效：2/);
assert.match(rendered, /缓存写入 token：40；已报告 \/ 未报告 \/ 无效事件：3 \/ 2 \/ 1/);
assert.match(rendered, /本地摘要：完整命中 1；前缀命中 2；未命中 3；拒绝复用 4/);
assert.match(rendered, /未计入模型统计的事件：2/);
assert.match(rendered, /事件可能丢失/);

// An explicitly reported zero is different from absent or unusable data.
rendered = format({ models: [{ ...model, cache_measured: 1, cached_tokens: 0,
  cache_hit_basis_points: 0, cache_write_reported: 1, cache_write_tokens: 0 }] });
assert.match(rendered, /0\.00%（0 \/ 400）/);
assert.match(rendered, /缓存写入 token：0/);
rendered = format({ models: [{ ...model, cache_measured: 0, cache_hit_basis_points: null,
  cache_write_reported: 0, cache_write_unreported: 0, cache_write_unusable: 1 }] });
assert.match(rendered, /输入 token 复用比例：不可计算/);
assert.match(rendered, /缓存写入 token：无有效数据；已报告 \/ 未报告 \/ 无效事件：0 \/ 0 \/ 1/);

rendered = format({ models: [{ ...model, token_totals_saturated: true,
  cache_write_tokens_saturated: true, cache_write_tokens: Number.MAX_SAFE_INTEGER + 1 }] });
assert.match(rendered, /输入 token 复用比例：不可计算/);
assert.match(rendered, /缓存写入 token：—（累计值已饱和）/);

// Refresh actually places the interpretation into the page using textContent.
statusResponse = { profile: { cache_metrics: metrics } };
await vm.runInContext('refreshAll()', context);
assert.match(status.textContent, /cache_hit_basis_points/);
assert.match(cacheMetrics.textContent, /18\.75%/);
statusResponse = { profile: { cache_metrics: null } };
await vm.runInContext('refreshAll()', context);
assert.equal(cacheMetrics.textContent, '缓存观测数据不可用');
console.log('web console cache metrics tests passed');
