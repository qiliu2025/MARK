(function (root) {
  'use strict';
  // Match complete native messages before applying UI label translations.
  // Numbered fields retain counts, identifiers and user-supplied values.
  const messageTemplates = [
    ['候选范围：{0} / {1} 个测站（{2} 个有坐标）。', 'Candidate scope: {0} / {1} stations ({2} with coordinates).'],
    ['候选范围：{0} / {1} 个测站（{2} 个有坐标）；已取消范围外 {3} 个选择。', 'Candidate scope: {0} / {1} stations ({2} with coordinates); deselected {3} outside the scope.'],
    ['候选范围：全部 {0} 个测站。', 'Candidate scope: all {0} stations.'],
    ['候选范围：请先查询当天测站。', 'Candidate scope: query stations first.'],
    ['地图框选：在地图内按住左键拖出矩形候选范围。', 'Drag a rectangle on the map to define the candidate scope.'],
    ['日期筛选：在 {0} 至 {1} 中保留 {2} / {3} 天', 'Date filters: retained {2} / {3} days from {0} to {1}'],
    ['日期筛选：在 {0} 至 {1} 中保留 {2} / {3} 天。', 'Date filters: retained {2} / {3} days from {0} to {1}.'],
    ['日期筛选：在 {0} 至 {1} 中保留 {2} / {3} 天（空间天气指标缺失 {4} 天）', 'Date filters: retained {2} / {3} days from {0} to {1} ({4} days lack space-weather records)'],
    ['{0}保留日期：{1}', '{0} Retained dates: {1}', [0,1]],
    ['保留日期：无', 'Retained dates: none'],
    ['保留日期：{0}', 'Retained dates: {0}', [0]],
    ['日期筛选：起始日期无效。', 'Date filters: the start date is invalid.'],
    ['日期筛选无法读取空间天气知识库：{0}', 'Date filters cannot read the space-weather index: {0}', [0]],
    ['{0} 至 {1} · {2}：{3} 个去重测站', '{0} to {1} · {2}: {3} unique stations'],
    ['{0}（Site Log按观测日期应用设备履历；地图显示 {1} 个，{2} 个日文件因元数据条件被排除）', '{0} (equipment history matched by observation date; {1} mapped, {2} daily files excluded by metadata filters)', [0]],
    ['已选择 {0} · 候选范围 {1} · 当前列表 {2} · 总计 {3}', 'Selected {0} · In scope {1} · Listed {2} · Total {3}'],
    ['当前没有可用的本地知识库；红色站网暂不能参与本地查询。', 'No local indexes are available; unavailable networks cannot be queried locally.'],
    ['解压与 RINEX 转换', 'Extraction and RINEX conversion'],
    ['只有 {0} 个符合筛选与坐标要求的候选测站，少于目标 {1} 个；原有选择未被替换。', 'Only {0} candidates meet the filters and coordinate requirements, below the target of {1}; the previous selection was retained.'],
    ['候选池在 {0} 只有 {1} 个已确认可用站、{2} 个待核实站，无法保证每日 {3} 站；原有选择未被替换。', 'On {0}, the candidate pool has {1} confirmed available stations and {2} unverified stations; {3} daily stations cannot be guaranteed. The previous selection was retained.'],
    ['当前测站集合满足数量、距离与每日可用约束。', 'The station set meets the count, distance and daily availability requirements.'],
    ['当前测站集合尚未满足全部选站约束。', 'The station set does not meet all selection requirements.'],
    ['当前日期筛选未保留任何评估日期。', 'The date filters retained no evaluation dates.'],
    ['选站参数无效：请检查目标数量、时间范围和距离。', 'Invalid station-selection parameters: check the target count, date range and distances.'],
    ['所选测站中有测站不在当前候选筛选范围内，请重新选站。', 'Some selected stations are outside the current candidate scope. Select stations again.'],
    ['每日最低可用站数不能大于目标测站数量。', 'The minimum daily available station count cannot exceed the target count.'],
    ['暂未找到满足目标数量与距离约束的方案；原有选择未被替换。', 'No plan meeting the target count and distance requirements was found; the previous selection was retained.'],
    ['选站已取消。', 'Station selection cancelled.'],
    ['Site Log 日期履历筛选已取消。', 'Site Log equipment-history filtering cancelled.'],
    ['请至少选择一个站网。', 'Select at least one network.'],
    ['日期范围无效。', 'The date range is invalid.'],
    ['暂未找到同时满足数量、距离与每日可用约束的方案；可扩大候选范围或调整条件。原有选择未被替换。', 'No plan meeting the count, distance and daily availability requirements was found. Expand the candidate scope or adjust the conditions. The previous selection was retained.'],
    ['高级筛选候选项读取提示：{0}', 'Advanced-filter choices: {0}', [0]],
    ['{0} Site Log提示：{1}', '{0} Site Log note: {1}', [0,1]],
    ['{0}（服务器未提供总大小，请继续等待）', '{0} (the server did not provide a total size; please wait)', [0]]
  ];
  const messagePatterns = messageTemplates.map(([source, english, fields = []]) => {
    const keys = [];
    const pattern = source.split(/(\{\d+\})/).map(part => {
      if (/^\{\d+\}$/.test(part)) { keys.push(Number(part.slice(1,-1))); return '([\\s\\S]+?)'; }
      return part.replace(/[.*+?^${}()|[\]\\]/g,'\\$&').replace(/\s+/g,'\\s*')
        .replace(/（/g,'[（(]').replace(/）/g,'[）)]').replace(/：/g,'[:：]');
    }).join('');
    return { pattern:new RegExp('^'+pattern.replace(/。$/,'[。.]?')+'$'), english, keys, fields };
  });
  function translateMessage(value, translate = text => text) {
    const raw = String(value).trim();
    if (/^\d{4}-\d{2}-\d{2}(?:\s*至\s*\d{4}-\d{2}-\d{2})?(?:\s*[、；;]\s*\d{4}-\d{2}-\d{2}(?:\s*至\s*\d{4}-\d{2}-\d{2})?)*$/.test(raw))
      return raw.replace(/\s*至\s*/g, ' to ').replace(/[、；]/g, '; ');
    const lines = raw.split(/\s*(?:\r?\n|\s+(?=日期筛选：|保留日期：))\s*/).filter(Boolean);
    if (lines.length > 1) return lines.map(translate).join(raw.includes('\n') ? '\n' : ' ');
    for (const {pattern,english,keys,fields} of messagePatterns) {
      const match = pattern.exec(raw);
      if (!match) continue;
      const values = new Map(keys.map((key,index) => [key,match[index+1]]));
      return english.replace(/\{(\d+)\}/g, (_,key) => {
        const index=Number(key), text=values.get(index);
        return fields.includes(index) ? translate(text) : text;
      });
    }
    return null;
  }
  const diagnosticPhrases = new Map([
    ['尚未完整扫描这一日期范围。', 'This date range is not fully indexed.'],
    ['未收录日期按缺测保守计算，不影响其他站网的查询结果', 'Unindexed dates count as missing; results from other networks are unaffected'],
    ['GIM 测站筛选已启用，但没有选择机构', 'GIM station filtering is enabled, but no provider is selected'],
    ['当前按零个 GIM 测站处理。', 'The GIM station set is empty.'],
    ['GIM 产品等级没有有效的机构与等级组合', 'No valid GIM provider and product-level combination is selected'],
    ['GIM 测站筛选已取消。', 'GIM station filtering was cancelled.'],
    ['当前未改变观测结果。', 'Observation results have not been changed.'],
    ['无法准备 GIM 测站筛选查询', 'Unable to prepare the GIM station query'],
    ['无法绑定 GIM 机构筛选条件', 'Unable to bind the GIM provider filter'],
    ['无法绑定 GIM 产品等级筛选条件。', 'Unable to bind the GIM product-level filter.'],
    ['仅保留模式按零个测站处理。', 'The keep-only station set is empty.'],
    ['排除模式暂不排除任何测站。', 'Exclude mode has not excluded any stations.'],
    ['参考框架日期范围没有可用的内置测站名册，结果按零个测站处理。', 'No reference-frame station roster is available for these dates; the result is empty.'],
    ['所选日期早于当前内置的 IGS14 参考框架时间线（2017-01-29）', 'The dates precede the built-in IGS14 reference-frame timeline (2017-01-29)'],
    ['旧版 ITRF/IGS 框架暂未纳入自动筛选。', 'Earlier ITRF/IGS frames are not included in automatic selection.'],
    ['未找到参考框架核心测站名册，未执行测站筛选；请确认程序目录下 resources/reference_frames 文件夹完整。', 'The reference-frame core roster is missing; station filtering was not applied. Check resources/reference_frames in the application folder.'],
    ['未找到参考框架核心测站名册，未执行测站筛选', 'The reference-frame core roster is missing; station filtering was not applied'],
    ['请确认程序目录下 resources/reference_frames 文件夹完整。', 'Check resources/reference_frames in the application folder.'],
    ['IGS/ITRF 自动方案按日期使用 IGS14、IGb14、IGS20、IGb20、IGc20 官方核心测站名册', 'Automatic IGS/ITRF selection uses the official IGS14, IGb14, IGS20, IGb20 and IGc20 core rosters for the selected dates'],
    ['跨框架日期范围使用重叠框架的测站并集。', 'Dates spanning multiple frames use the union of their station rosters.'],
    ['查询日期超出该框架有效期，请使用‘按日期自动选择’方案。', 'The dates are outside this frame\'s validity period; use automatic selection by date.'],
    ['频点条件格式无效', 'The observation-code filter is invalid'],
    ['请使用‘GPS:C1C’、‘BDS:C2I’这类‘系统:观测码’写法。无效条件按不匹配处理。', 'Use SYSTEM:CODE, such as GPS:C1C or BDS:C2I. Invalid conditions do not match.'],
    ['请使用‘GPS:C1C’、‘BDS:C2I’这类‘系统:观测码’写法。', 'Use SYSTEM:CODE, such as GPS:C1C or BDS:C2I.'],
    ['无效条件按不匹配处理。', 'Invalid conditions do not match.'],
    ['频点筛选需要可用的 Site Log Plus 频点知识库', 'Observation-code filtering requires the Site Log Plus database'],
    ['当前没有可匹配数据。', 'No matching records are available.'],
    ['频点筛选目前只对 IGS/MGEX 的 Site Log Plus 记录生效', 'Observation-code filtering applies to IGS/MGEX Site Log Plus records'],
    ['其他站网没有可验证的系统+观测码区间。', 'Other networks have no verified system and observation-code intervals.'],
    ['每日覆盖完整性条件未匹配任何日期。', 'No dates meet the daily coverage condition.'],
    ['观测质量记录来自共享的 IGS/MGEX 日 QC 知识库', 'Daily quality records come from the shared IGS/MGEX quality database'],
    ['启用质量条件后，没有可匹配 QC 记录的其他站网测站会被排除', 'When quality conditions are enabled, stations without matching quality records are excluded'],
    ['日期范围内没有测站满足当前筛选条件。', 'No stations meet the current filters in this date range.'],
    ['当前查询继续使用上一次完整高速快照', 'This query uses the last complete search snapshot'],
    ['当前下载清单按上一次完整高速快照复核。', 'The download list is verified against the last complete search snapshot.'],
    ['未找到 Site Log Plus 频点知识库（site_log_plus_equipment.sqlite）。', 'The Site Log Plus database (site_log_plus_equipment.sqlite) was not found.'],
    ['Site Log Plus 知识库缺少 signal_capability_intervals 表。', 'The Site Log Plus database is missing signal_capability_intervals.'],
    ['Site Log Plus 频点知识库可读取，但没有可匹配到高速索引测站的区间。', 'The Site Log Plus database is readable, but has no intervals matching the search-index stations.'],
    ['相关文件将被排除。', 'Files without matching metadata will be excluded.'],
    ['超出知识库日期的候选沿用最后已知日信息，完整性为预估', 'Candidates beyond the indexed dates use the last known day; coverage is estimated'],
    ['下载时在线确认实际文件。', 'Actual files are verified online during download.'],
    ['未查到所选测站的1秒文件。', 'No 1-second files were found for the selected stations.'],
    ['IGS 本地知识库中的该日期是旧版或部分七源记录；界面已按本地记录显示，且没有访问网络。请使用知识库构建器补齐。', 'The IGS index contains older or partial seven-source records for this date. Local records are displayed without network access. Update the index with the builder.'],
    ['IGS 七个数据中心均无法刷新，暂时使用待复扫的本地记录。', 'None of the seven IGS data centers could be refreshed; cached records pending a rescan are used.'],
    ['RGNA：官方公共 SFTP 月目录读取成功，但当天没有发布日观测文件。', 'RGNA: the public SFTP monthly directory was read, but no daily observation files were published for this date.'],
    ['REGNA-ROU：官方当年匿名 FTP 已查询，但当天没有发布日观测文件。', 'REGNA-ROU: the current-year anonymous FTP directory was checked, but no daily observation files were published for this date.'],
    ['REGNA-ROU：官方历史 SFTP 月目录已查询，但当天没有发布日观测文件。', 'REGNA-ROU: the historical SFTP monthly directory was checked, but no daily observation files were published for this date.'],
    ['意大利 RING：目录可读，但当天没有可识别的日观测文件。', 'Italy RING: the directory is readable, but no recognized daily observation files were found for this date.'],
    ['加拿大 CACS：官方目录可读，但当天没有可识别的RINEX日观测文件。', 'Canada CACS: the official directory is readable, but no recognized daily RINEX observation files were found for this date.'],
    ['加拿大 CACS：日文件已完整返回，但 NRCan 当前坐标接口暂时不可用；少数地图点可能缺少坐标。', 'Canada CACS: daily files were returned, but the NRCan coordinate service is temporarily unavailable. Some stations may lack map coordinates.'],
    ['新西兰 GeoNet/PositioNZ：文件目录完整，但官方坐标API暂时不可用；少数地图点可能缺少坐标。', 'New Zealand GeoNet/PositioNZ: the file listing is complete, but the coordinate API is temporarily unavailable. Some stations may lack map coordinates.'],
    ['KASI-NGII：知识库只写入 KASI 匿名目录中实际枚举到的文件；NGII GNSSData 的动态 ZIP 门户没有完整日清单，未写入未经验证的站日。', 'KASI-NGII: the index contains files listed by the KASI anonymous archive. The NGII GNSSData ZIP portal has no complete daily listing; unverified station-days are excluded.'],
    ['TU Delft DLAB：官方目录可读，但当天没有可识别的RINEX观测文件。', 'TU Delft DLAB: the official directory is readable, but no recognized RINEX observation files were found for this date.'],
    ['Nevada MAGNET：官方目录可读，但当天没有可识别的RINEX观测文件。', 'Nevada MAGNET: the official directory is readable, but no recognized RINEX observation files were found for this date.'],
    ['PANGA/CWU：官方目录可读，但当天没有可识别的RINEX观测文件。', 'PANGA/CWU: the official directory is readable, but no recognized RINEX observation files were found for this date.'],
    ['：该日期属于最近7天暂定记录，构建器下次运行会重新扫描。', ': this date has provisional records from the last seven days and will be rescanned on the next builder run.'],
    ['没有日期满足当前空间天气筛选条件。', 'No dates meet the current space-weather conditions.'],
    ['天未达到严格七源；已使用当天现有的可用来源，完全无索引的日期按无数据跳过', 'days lack complete seven-source coverage; available sources are used, and unindexed dates are skipped'],
    ['天知识库为缺失、失败或暂定状态，已按无数据跳过', 'days have missing, failed or provisional index records and are skipped'],
    ['当前完整性阈值高于 GPS 周可提供的 7/7，未找到符合条件的测站。', 'The coverage threshold exceeds the seven days available in a GPS week; no stations meet the condition.']
  ]);
  const diagnosticPrefixes = new Map([
    ['坐标覆盖文件过大，已忽略：', 'Coordinate override file is too large and was ignored: '],
    ['无法读取坐标覆盖文件，已忽略：', 'Unable to read coordinate override file; ignored: '],
    ['坐标覆盖文件 JSON 无效，已忽略：', 'Coordinate override file contains invalid JSON; ignored: '],
    ['无法读取 Site Log Plus 频点知识库：', 'Unable to read the Site Log Plus database: '],
    ['读取 Site Log Plus 频点区间失败：', 'Unable to read Site Log Plus observation-code intervals: '],
    ['读取 Site Log Plus 频点区间时发生 SQLite 错误：', 'Database error while reading Site Log Plus observation-code intervals: '],
    ['读取频点已知时段失败：', 'Unable to read observation-code validity intervals: '],
    ['读取频点对应的日文件可用性失败：', 'Unable to read daily availability for observation-code intervals: '],
    ['读取 GIM 测站列表失败', 'Unable to read GIM station records'],
    ['IGS 本地索引暂不可用，已回退在线查询（', 'The IGS local index is unavailable; using an online query ('],
    ['目录未纳入本次并集（', 'The directory was excluded from the combined result ('],
    ['IGS 在线查询成功，但写入本地索引失败（', 'The IGS online query succeeded, but the local index could not be written (']
  ]);
  const diagnosticPatterns = [
    [/^已加载 ([\d,]+) 个只读坐标覆盖[（(](.+)[）)]。?$/, (_, count, file) => `Loaded ${count} coordinate overrides (${file}).`],
    [/^坐标覆盖文件过大，已忽略[:：]\s*(.+)$/, (_, file) => `Coordinate override file is too large and was ignored: ${file}`],
    [/^无法读取坐标覆盖文件，已忽略[:：]\s*(.+)$/, (_, file) => `Unable to read coordinate override file; ignored: ${file}`],
    [/^坐标覆盖文件 JSON 无效，已忽略[:：]\s*(.+)$/, (_, file) => `Coordinate override file contains invalid JSON; ignored: ${file}`],
    [/^部分所选站网[（(]掩码\s*(0x[\dA-Fa-f]+)[）)]尚未完整扫描这一日期范围$/, (_, mask) => `Some selected networks (${mask}) are not fully indexed for this date range`],
    [/^已逐日检查 IGS Network 质量记录，([\d,]+) 个测站的合格日期因质量条件或缺少记录而减少$/, (_, count) => `Daily IGS Network quality records checked; ${count} stations have fewer qualified dates because of quality conditions or missing records`],
    [/^(.+?) 方案使用官方核心测站名册[（(]有效期 (\S+) 至 (\S+?)[）)]。[ ]*([\s\S]*)$/, (_, frame, start, end, rest, translate) => `${frame} uses the official core-station roster (valid ${start} to ${end === '今' ? 'present' : end}).${rest ? ' ' + translate(rest) : ''}`],
    [/^无法读取 GIM 测站知识库[（(](.+)[）)]$/, (_, file) => `Unable to read the GIM station database (${file})`],
    [/^中央高速观测索引已过期[^：]*：([^。]+)。([\s\S]*)$/, (_, file, rest, translate) => `The observation search index is stale: ${file}. ${translate(rest)}`]
  ];
  function translateDiagnostic(value, translate = text => text) {
    const paths = [];
    const raw = String(value).trim().replace(/(?:[A-Za-z]:[\\/]|(?:https?|ftps?|sftp):\/\/)[^\s<>"；）]+/g, path => {
      paths.push(path); return `\uE100${paths.length - 1}\uE101`;
    });
    const restore = text => text.replace(/\uE100(\d+)\uE101/g, (_, index) => paths[Number(index)]);
    const heading = /^(?:部分来源未完成|查询提示)[:：]\s*/.exec(raw);
    const body = heading ? raw.slice(heading[0].length) : raw;
    const clause = text => {
      const value = text.trim();
      if (diagnosticPhrases.has(value)) return diagnosticPhrases.get(value);
      for (const [pattern, format] of diagnosticPatterns) {
        const match = pattern.exec(value);
        if (match) return format(...match, translate);
      }
      for (const [prefix, english] of diagnosticPrefixes)
        if (value.startsWith(prefix)) return english + translate(value.slice(prefix.length));
      return null;
    };
    const full = clause(body);
    if (full !== null) return restore((heading ? 'Query notes: ' : '') + full);
    const parts = body.split(/\s*[\r\n；]+\s*|(?<=。)\s*(?=[\u3400-\u9fff])/).filter(Boolean);
    const translated = parts.map(clause);
    if (!heading && !translated.some(value => value !== null)) return null;
    // Preserve every diagnostic; known clauses must not hide later warnings.
    return restore((heading ? 'Query notes: ' : '') + parts
      .map((text, index) => translated[index] ?? translate(text)).join('; '));
  }
  function create(clock = () => Date.now()) {
    const recent = new Map(), failures = new Map(), progress = new Map();
    function consider(message, kind = 'notice', options = {}) {
      let text = String(message ?? '').replace(/\s+/g, ' ').trim();
      if (!text) return null;
      text = text.replace(/^精密产品查询范围\s+(.+?)[:：]\s*(\d+)\s*个(?:逻辑)?产品.*$/,
        '精密产品查询完成：$1；$2 个产品。');
      const now = clock(), stream = options.stream || 'observation';
      const terminal = /下载(?:结束|完成)|任务(?:已)?(?:完成|取消)|已取消|下载已取消|查询完成|^方案[:：]/i.test(text);
      const errorText = text.replace(/^部分来源未完成[:：]\s*/, '')
        .replace(/失败[ ：:]*0(?=[ ，,。；;]|$)/g, '');
      const warning = kind === 'warning' || /失败|错误|无法|未找到|未完成|缺少|尚未|没有|请先|error|warning/i.test(errorText);
      if (warning) kind = 'warning';
      const file = /\.(?:gz|Z|bz2|zip|crx|rnx|sp3|clk|atx|snx|obx|bia)\b|\.[0-9]{2}[odngp]\b/i.test(text);
      // Progress belongs in the task dock; history records results and errors.
      if (!terminal && !warning && (options.progress || file && options.busy)) return null;
      if (!terminal && !warning && /^(?:正在|生成下载清单|读取下载路线|已自动选择|选择数据源和日期后)|Site Log|站点坐标[:：]|知识库已有索引|读取耗时|已命中所选站网/.test(text)) return null;
      let key = '';
      if (!terminal && warning && options.busy && file) {
        const count = (failures.get(stream) || 0) + 1;
        failures.set(stream, count);
        key = 'file-errors:'+stream;
        text = `下载文件异常累计 ${count} 条；最近：${text}`;
      } else if (!terminal && kind === 'download' && options.busy) {
        const previous = progress.get(stream);
        if (previous !== undefined && now-previous < 20000) return null;
        progress.set(stream, now);
        key = 'progress:'+stream;
      }
      if (terminal) { failures.delete(stream); progress.delete(stream); }
      const fullText = text;
      const limit = kind === 'warning' ? 360 : 260;
      if (text.length > limit) text = text.slice(0, limit-1)+'…';
      const fingerprint = stream+'|'+kind+'|'+text;
      const previous = recent.get(fingerprint);
      if (previous !== undefined && now-previous < 15000) return null;
      recent.set(fingerprint, now);
      if (recent.size > 100) for (const [id,time] of recent) if (now-time > 15000) recent.delete(id);
      return { text, fullText, limit, kind, key };
    }
    return { consider };
  }
  const api = { create, translateDiagnostic, translateMessage };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.GnssRuntimeLog = api;
})(typeof window === 'object' ? window : globalThis);
