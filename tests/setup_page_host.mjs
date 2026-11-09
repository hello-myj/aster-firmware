// SPDX-License-Identifier: MIT
// Run the actual embedded script with a small DOM/HTTP fixture. No browser,
// radio, password store or external network is touched by this test.
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';

const html = fs.readFileSync(new URL('../common/setup/provision.html', import.meta.url), 'utf8');
const source = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
const timers = new Map();
let timerId=0,failPost=false;
const posts = [];
let phase = 3;
const state = { phase, token: 'test-token', error: 0, percent: 0, bytes: 0, total: 0, seconds: 1, server: '' };
const document = {
  getElementById(id) {
    if (!elements.has(id)) elements.set(id, { value: '', textContent: '', hidden: false, disabled: false });
    return elements.get(id);
  },
};
const context = vm.createContext({
  document, Promise,
  setTimeout: (callback,delay) => {const id=++timerId;timers.set(id,{callback,delay});return id;},
  clearTimeout: id => timers.delete(id),
  fetch: async (url, options) => {
    if (url === '/api/status') return { ok: true, json: async () => ({ ...state, phase }) };
    assert.equal(url, '/api/connect');
    if(failPost)return new Promise(()=>{});
    assert.equal(options.method, 'POST');
    assert.equal(options.headers['Content-Type'], 'application/json');
    posts.push(JSON.parse(options.body));
    return { ok: true, json: async () => ({ accepted: 1 }) };
  },
});
vm.runInContext(source, context);
await new Promise(setImmediate);
assert.equal(elements.get('state').textContent, '等待配网');
assert.equal(timers.size, 1);
document.getElementById('ssid').value = '中文测试';
document.getElementById('password').value = 'test-only-password';
elements.get('server').value = 'http://192.0.2.1:9081/';
await elements.get('connect').onclick();
assert.deepEqual(posts, [{ ssid: '中文测试', password: 'test-only-password', server: 'http://192.0.2.1:9081', token: 'test-token' }]);
assert.equal(elements.get('state').textContent, '已提交，请查看设备屏幕');
assert.equal(elements.get('password').value, '');

elements.get('ssid').value = '字'.repeat(11);
await elements.get('connect').onclick();
assert.equal(posts.length, 1);
assert.match(elements.get('state').textContent, /1–32/);
async function fire(delay){
  const [id,entry]=[...timers].find(([,entry])=>entry.delay===delay);
  timers.delete(id);return await entry.callback();
}
await fire(1500);
assert.match(elements.get('state').textContent,/1–32/,'poll must retain input errors');
elements.get('ssid').value='test-network';failPost=true;
const stalled=elements.get('connect').onclick();
await fire(12000);await stalled;
assert.match(elements.get('state').textContent,/超时/);
assert.equal(elements.get('connect').disabled,false);
await fire(1500);assert.match(elements.get('state').textContent,/超时/);

phase = 9;
state.percent = 100;
await fire(1500);
assert.equal(elements.get('form').hidden, true);
assert.equal(elements.get('state').textContent, '准备完成，可以使用设备了');
assert.equal(timers.size, 0, 'completion must stop polling before the AP closes');
console.log('PASS embedded page UTF-8 validation, submission and completed state');
