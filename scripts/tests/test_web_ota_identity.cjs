'use strict';
// Actual browser header decoder; layout agrees with the SDK descriptor tests.
const assert = require('node:assert/strict');
const vm = require('node:vm');
const {readWebSource, productionFunctions} = require('./web_source.cjs');
const context = vm.createContext({Uint8Array, DataView, TextDecoder, t: x => x});
vm.runInContext(productionFunctions(readWebSource().script, ['decodeFirmware']) +
  '\nglobalThis.decode = decodeFirmware;', context);
const expected = {target: 'xiao_esp32c6_gc9a01', layout: '4mb_ota_littlefs_v1'};
const schema = {config_version: 1, firmware_identity_version: 2};
function firmware() {
  const a = new Uint8Array(1024), v = new DataView(a.buffer);
  const text = (offset, value) => a.set(new TextEncoder().encode(value), offset);
  a[0] = 0xe9;
  v.setUint32(32, 0xabcd5432, true);
  v.setUint32(288, 0x46424c31, true);
  v.setUint32(292, schema.firmware_identity_version, true);
  v.setUint32(376, schema.config_version, true);
  text(48, 'current'); text(296, expected.target); text(344, expected.layout);
  a.fill(0x31, 176, 208);
  return a;
}
const decode = bytes => context.decode(bytes.buffer, expected, schema, 1835008, 100000);
let bytes = firmware();
assert.equal(decode(bytes).id, '31'.repeat(32));
assert.equal(decode(bytes).format, 1);
new DataView(bytes.buffer).setUint32(376, 2, true);
assert.equal(decode(bytes).format, 2);
for (const mutate of [
  a => {const v = new DataView(a.buffer); v.setUint32(292, 1, true); v.setUint32(376, 11, true);},
  a => new DataView(a.buffer).setUint32(376, 0, true),
  a => a[296] = 88,
  a => a[344] = 88,
  a => a[0] = 0,
  a => a.fill(0, 176, 208),
]) {
  bytes = firmware(); mutate(bytes); assert.throws(() => decode(bytes));
}
assert.throws(() => context.decode(new Uint8Array(379).buffer, expected, schema, 1835008, 100000));
assert.throws(() => context.decode(firmware().buffer, expected, schema, 90000, 100000));
console.log('PASS browser OTA: current/newer aggregate accepted; legacy, wrong identity, truncated/oversize rejected');
