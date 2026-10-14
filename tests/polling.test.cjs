const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
// Optional source path lets this regression test run against an older checkout.
const source = fs.readFileSync(process.argv[2] || path.join(__dirname, '../src/web/app.js'), 'utf8');
function part(start, end) {
  const from = source.indexOf(start), to = source.indexOf(end, from);
  assert.ok(from >= 0 && to > from, 'Missing source section: ' + start);
  return source.slice(from, to);
}
const poller = part('  function poller(', '  /* ---------------------------------------------------------------- router');
const flush = () => new Promise(resolve => setImmediate(resolve));
function env() {
  const timers = [];
  const c = vm.createContext({isPS5: false, document: {hidden: false},
    setTimeout: (fn, delay) => timers.push({fn, delay})});
  vm.runInContext(poller, c);
  return {c, timers};
}
(async () => {
  {
    const {c, timers} = env();
    let resolve, calls = 0;
    c.poller(() => { calls++; return new Promise(r => { resolve = r; }); }, 6000);
    await flush();
    assert.equal(calls, 1); assert.equal(timers.length, 0);
    resolve(); await flush();
    assert.equal(timers.length, 1); assert.equal(timers[0].delay, 6000);
    timers.shift().fn(); await flush();
    assert.equal(calls, 2); assert.equal(timers.length, 0);
    resolve(); await flush();
  }
  {
    const {c, timers} = env(); let attempts = 0;
    c.poller(async () => { attempts++; if (attempts === 1) throw Error('timeout'); return attempts > 2; }, 6000);
    await flush(); assert.equal(timers[0].delay, 12000);
    await timers.shift().fn(); assert.equal(timers[0].delay, 24000);
    await timers.shift().fn(); assert.equal(timers[0].delay, 6000);
    c.document.hidden = true;
    await timers.shift().fn(); assert.equal(attempts, 3); assert.equal(timers[0].delay, 18000);
  }
  {
    const {c, timers} = env();
    c.S = {launching: false}; c.get = async () => { throw Error('HTTP 503'); };
    vm.runInContext(part('  async function refreshLaunch()', '  function poller('), c);
    c.poller(c.refreshLaunch, 4000); await flush();
    assert.equal(timers[0].delay, 8000);
    await timers.shift().fn(); assert.equal(timers[0].delay, 16000);
  }
  {
    const {c, timers} = env(); let calls = 0, enabled = false;
    c.isPS5 = true;
    c.poller(() => { calls++; throw Error('synchronous failure'); }, 6000, () => enabled);
    await flush();
    assert.equal(calls, 0); assert.equal(timers[0].delay, 12000);
    enabled = true;
    await timers.shift().fn();
    assert.equal(calls, 1); assert.equal(timers[0].delay, 12000);
    for (let i = 0; i < 8; i++) await timers.shift().fn();
    assert.equal(timers[0].delay, 60000);
  }
  {
    const g = {id: 'test', tab: 'cheats', loaded: true, applying: {}};
    let callback, paints = 0;
    const c = vm.createContext({S: {g}, route: () => ({page: 'game'}),
      loadCheatState: async () => false, paintMods: () => { paints++; },
      poller: fn => { callback = fn; }});
    const start = source.lastIndexOf('    poller(async function () {');
    const end = source.indexOf('    }, 6000);', start);
    assert.ok(start >= 0 && end > start);
    vm.runInContext(source.slice(start, end + '    }, 6000);'.length), c);
    assert.equal(await callback(), false);
    assert.equal(paints, 1, 'failed state reads still update the visible error');
  }
  {
    const c = vm.createContext({});
    vm.runInContext(part('  var sema =', '  async function call('), c);
    const admitted = [c.acquire(), c.acquire()]; await Promise.all(admitted);
    const queued = Array.from({length: 16}, () => c.acquire());
    await assert.rejects(c.acquire(), /Too many queued requests/);
    assert.equal(c.sema.q.length, 16);
    for (let i = 0; i < 18; i++) c.release();
    await Promise.all(queued);
    assert.equal(c.sema.q.length, 0); assert.equal(c.sema.n, 2);
  }
  console.log('PASS: no overlapping poll ticks; error/false backoff and success reset; hidden tab; real refreshLaunch failures; PS5 interval, predicate, synchronous errors and backoff cap; bounded fetch queue');
})().catch(e => { console.error(e); process.exitCode = 1; });
