const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const runtime = require('../resources/ui/runtime-log.js');

const source = fs.readFileSync(path.join(__dirname, '../resources/ui/ui.js'), 'utf8');
const begin = source.indexOf('  const enDict = {');
const end = source.indexOf('  function isDarkUi()', begin);
assert(begin >= 0 && end > begin);
const context = {
  window: { GnssRuntimeLog: runtime },
  preciseProductTypeLabel: value => String(value).toUpperCase()
};
vm.runInNewContext(source.slice(begin, end) +
  ';globalThis.languageTest = {translate:localizeUiText, language:value=>uiPrefs.language=value};', context);
const { translate, language } = context.languageTest;
const cases = [
  '部分来源未完成：已加载 6 个只读坐标覆盖（observation_coordinate_overrides.json）。',
  '查询提示：坐标覆盖文件过大，已忽略：E:\\MARK\\observation_coordinate_overrides.json',
  '查询提示：无法读取坐标覆盖文件，已忽略：E:\\MARK\\observation_coordinate_overrides.json',
  '查询提示：坐标覆盖文件 JSON 无效，已忽略：E:\\MARK\\observation_coordinate_overrides.json',
  '查询提示：部分所选站网（掩码0x84040）尚未完整扫描这一日期范围；未收录日期按缺测保守计算，不影响其他站网的查询结果',
  '查询提示：已逐日检查 IGS Network 质量记录，12 个测站的合格日期因质量条件或缺少记录而减少',
  '查询提示：GIM 测站筛选已启用，但没有选择机构；当前按零个 GIM 测站处理。',
  '查询提示：GIM 产品等级没有有效的机构与等级组合；当前按零个 GIM 测站处理。',
  '查询提示：GIM 测站筛选已取消。',
  '查询提示：无法准备 GIM 测站筛选查询；当前未改变观测结果。',
  '查询提示：无法绑定 GIM 机构筛选条件；当前未改变观测结果。',
  '查询提示：无法绑定 GIM 产品等级筛选条件。',
  '查询提示：参考框架日期范围没有可用的内置测站名册，结果按零个测站处理。',
  '查询提示：所选日期早于当前内置的 IGS14 参考框架时间线（2017-01-29）；旧版 ITRF/IGS 框架暂未纳入自动筛选。',
  '查询提示：未找到参考框架核心测站名册，未执行测站筛选；请确认程序目录下 resources/reference_frames 文件夹完整。',
  '查询提示：IGS/ITRF 自动方案按日期使用 IGS14、IGb14、IGS20、IGb20、IGc20 官方核心测站名册；跨框架日期范围使用重叠框架的测站并集。',
  '查询提示：IGS20 方案使用官方核心测站名册（有效期 2022-11-27 至 今）。',
  '查询提示：查询日期超出该框架有效期，请使用‘按日期自动选择’方案。',
  '查询提示：频点条件格式无效；请使用‘GPS:C1C’、‘BDS:C2I’这类‘系统:观测码’写法。无效条件按不匹配处理。',
  '查询提示：频点筛选需要可用的 Site Log Plus 频点知识库；当前没有可匹配数据。',
  '查询提示：频点筛选目前只对 IGS/MGEX 的 Site Log Plus 记录生效；其他站网没有可验证的系统+观测码区间。',
  '查询提示：每日覆盖完整性条件未匹配任何日期。',
  '查询提示：观测质量记录来自共享的 IGS/MGEX 日 QC 知识库；启用质量条件后，没有可匹配 QC 记录的其他站网测站会被排除',
  '查询提示：日期范围内没有测站满足当前筛选条件。',
  '查询提示：未找到 Site Log Plus 频点知识库（site_log_plus_equipment.sqlite）。',
  '查询提示：Site Log Plus 知识库缺少 signal_capability_intervals 表。',
  '查询提示：Site Log Plus 频点知识库可读取，但没有可匹配到高速索引测站的区间。',
  '查询完成：2026-02-15 至 2026-02-24；8310 个测站，地图显示 8274 个。 36 个暂缺坐标。',
  '下载结束：成功 10，已存在跳过 0，失败 0。',
  '下载结束：成功 1，已存在跳过 0，失败 0。统一保存到 E:\\MARK\\downloads。 Download log: E:\\MARK\\downloads\\logs\\MARK-observations.log',
  '精密产品处理完成：成功 1，已存在 0，失败 0。 Download log: E:\\MARK\\downloads\\logs\\MARK-products.log',
  '下载任务已取消，等待重新开始。 Download log: E:\\MARK\\downloads\\logs\\MARK-products.log',
  '[0/1] GFZ0OPSFIN_0000_07D_IGS_ERP_LONG: 2026-08-01：正在从 CDDIS 下载',
  '[0/1] GFZ0OPSFIN_0000_07D_IGS_ERP_LONG: 2026-08-01：尝试 BKG：GFZ0OPSFIN.ERP.gz（自动并发 1/3）',
  '[1/1] GFZ0OPSFIN_0000_07D_IGS_ERP_LONG: 2026-08-01：所有候选镜像均未成功',
  '下载失败：test.clk.gz；网络错误。',
  '查询提示：中央高速观测索引已过期（源库已变化）：gnss_availability.duckdb。请重新安装官方知识库后重试。 当前查询继续使用上一次完整索引；请重新安装官方知识库后重试。',
  '查询提示：IGS20 方案使用官方核心测站名册（有效期 2022-11-27 至 2024-02-10）。 查询日期超出该框架有效期，请使用‘按日期自动选择’方案。',
  '查询提示：无法读取 GIM 测站知识库（E:\\MARK\\gim.duckdb）；仅保留模式按零个测站处理。 原因：无法打开知识库：文件损坏。',
  '查询提示：读取 Site Log Plus 频点区间失败：no such table: signal_capability_intervals',

  "精密产品索引缺失或为空，请重新安装官方知识库后重试。",
  "精密产品索引与知识库不匹配，请重新安装官方知识库后重试。",
  "观测数据索引不可用，请重新安装官方知识库后重试。",
  "请重新安装官方知识库后重试。",
  "请调整查询日期范围。",
  "请调整所选站网。",
  "可尝试其他已收录的数据源。",
  "当前查询继续使用上一次完整索引；请重新安装官方知识库后重试。",
  "中央高速观测索引结构不完整，",
  "中央高速观测索引版本不兼容，",
  "高速索引测站编号不连续，",
  "高速索引测站别名编号无效，",
  "高速索引档案编号无效，",
  "高速索引位图损坏，",
  "完整日期位图损坏，",
  "高速索引 Site Log 编号无效，",
  "中央高速索引未覆盖所选站网掩码",
  "SP3 本地知识库尚未收录 GPS 周 2500。本次查询没有联网。",
  "CLK 本地知识库尚未收录 GPS 周 2500。本次查询没有联网。",
  "DCB 知识库尚未收录 2027 年。本次查询没有联网。",
  "广播星历知识库尚未收录这一天。本次查询没有联网。数据库：E:\\MARK\\broadcast.duckdb",

];
language('en');
assert.equal(translate('显示全部'), 'Show all');
assert.equal(translate('知识库数据截至'), 'Knowledge-base data through');
const aboutHtml = fs.readFileSync(path.join(__dirname, '../resources/ui/index.html'), 'utf8');
assert(aboutHtml.includes('id="markKnowledgeThrough">2026-08-01'));
assert(!aboutHtml.includes('class="mark-changelog"'));
assert(!aboutHtml.includes('id="markPreciseThrough"'));
for (const raw of cases) {
  const english = translate(raw);
  assert(!/[\u3400-\u9fff]/.test(english), `${raw}\n${english}`);
  language('zh');
  assert.equal(translate(raw), raw);
  language('en');
}
const combined = cases[4] + '；' + cases[0].replace('部分来源未完成：', '') + '\n' + cases[5].replace('查询提示：', '');
const output = translate(combined);
assert(!/[\u3400-\u9fff]/.test(output), output);
for (const term of ['0x84040', 'count as missing', 'Loaded 6', 'observation_coordinate_overrides.json', '12 stations']) {
  assert(output.includes(term), `Lost diagnostic: ${term}\n${output}`);
}
const localizedPath = 'E:\\数据\\坐标.json';
assert(translate('查询提示：坐标覆盖文件 JSON 无效，已忽略：' + localizedPath).includes(localizedPath));
assert.equal(runtime.create().consider(cases[0]).kind, 'notice');
assert.equal(runtime.create().consider(cases[2]).kind, 'warning');
let nativeMessages = 0;
for (const name of ['ObservationSearchDb.cpp', 'MainWindow.cpp', 'IgsData.cpp', 'PreciseProductView.cpp']) {
  const cpp = fs.readFileSync(path.join(__dirname, '../src', name), 'utf8');
  const assignments = /(?:\b(?:warning|coordinateOverrideWarning|siteLogPlusWarning)\s*\+?=|SetOperationStatusText\s*\(|SetQuerySummaryText\s*\()(.*?);/gs;
  for (const assignment of cpp.matchAll(assignments)) {
    const literals = /L"(?:\\.|[^"\\])*"(?:\s*L"(?:\\.|[^"\\])*")*/g;
    for (const group of assignment[1].matchAll(literals)) {
      const raw = [...group[0].matchAll(/L("(?:\\.|[^"\\])*")/g)].map(match => JSON.parse(match[1])).join('');
      if ((raw.match(/[\u3400-\u9fff]/g) || []).length < 4 || raw.includes('{')) continue;
      const english = translate(raw);
      assert(!/[\u3400-\u9fff]/.test(english), `${name}: ${raw}\n${english}`);
      nativeMessages++;
    }
  }
}
console.log(`Runtime language checks passed: ${cases.length} scenarios, ${nativeMessages} native messages, composed diagnostics, both languages, paths and severity.`);
