const assert = require('node:assert/strict');
const { create } = require('../resources/ui/runtime-log.js');
let now = 0;
const log = create(() => now);
assert.equal(log.consider('正在读取下载路线：1/500','download',{busy:true}), null);
assert.equal(log.consider('已自动选择 34 个具有本地知识库的站网。'), null);
for (let i=0;i<1000;i++) {
  now += 1;
  assert.equal(log.consider(`已完成 abmf${i}.25d.Z`,'download',{progress:true,busy:true}), null);
}
const result = log.consider('下载结束：成功 1000，已存在跳过 0，失败 0。','download');
assert.equal(result.kind, 'download');
assert.equal(log.consider('下载结束：成功 1000，已存在跳过 0，失败 0。','download'), null);
const errors=[];
for (let i=0;i<100;i++) errors.push(log.consider(`下载失败：file${i}.clk.gz；网络错误。`,'download',{busy:true}));
assert.equal(new Set(errors.map(e=>e.key)).size,1);
assert.match(errors.at(-1).text,/累计 100 条/);
assert.equal(errors.at(-1).kind,'warning');
assert.equal(log.consider('已取消下载任务。','download',{progress:true}).kind,'download');
assert.equal(log.consider('查询完成：2025-01-01 至 2026-08-01；497 个测站，地图显示 490 个。').kind,'notice');
assert.equal(log.consider('无法打开知识库：文件损坏。','notice').kind,'warning');
assert.equal(log.consider('部分来源未完成：无法连接 BKG。','notice').kind,'warning');
assert.equal(log.consider('精密产品查询范围 2025-01-01 至 2026-07-01：193 个逻辑产品；已检查 547 天；知识库已有索引 547 天；读取耗时 0.0 秒。').text,
  '精密产品查询完成：2025-01-01 至 2026-07-01；193 个产品。');
assert.equal(log.consider('方案：'+ '2025-01-01 至 2025-02-01；'.repeat(50),'download').text.length,260);
const progress = create(() => now);
assert.ok(progress.consider('下载进度：10%','download',{busy:true}));
assert.equal(progress.consider('下载进度：20%','download',{busy:true}),null);
now += 20001;
assert.ok(progress.consider('下载进度：30%','download',{busy:true}));
assert.ok(progress.consider('下载进度：15%','download',{busy:true,stream:'precise'}));
console.log('Runtime log tests passed: 1000 routine events suppressed, errors merged, task results/cancellation retained.');
