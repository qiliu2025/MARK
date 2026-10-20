(() => {
  'use strict';

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const bridge = window.chrome && window.chrome.webview;
  const state = {
    mode: 'observation', latest: {}, observationRows: [], preciseRows: [], precisePage: 0,
    mapPoints: [], renderedMapPoints: [], countryLabels: [], mapView: 'all', resultNeedle: '', ascending: false,
    sortColumn:'completeness', preciseSortDirection:'desc', detailRequestSerial:0, stationDetailData:null, stationDetailRequest:'',
    detailProduct:null, detailProductFamily:null, productCoverage:null, productCoverageRequest:'', coverageViews:{},
    rowsSelectedOnly: false, mapHoverStation: '', mapHoverDetail: null, mapHoverPosition: null,
    dense: false, progress: 0, infoFilter: 'all', info: [], infoSerial: 0,
    actionStation: null, detailStation: null, sourceNeedle: '', focusedMapStation: '',
    countryOptions: [], selectedCountries: [], pendingCountries: [], countryNeedle: '',
    advancedChoices: {
      systems: ['GPS','GLO','GAL','BDS','QZS','SBS','IRN'],
      receivers: ['TRIMBLE NETR9','TRIMBLE ALLOY','LEICA GR50','SEPT POLARX5'],
      antennas: ['TRM59800.00','TRM57971.00','LEIAR20.R4','SEPCHOKE_B3E6'],
      signals: ['GPS:C1C','GPS:L1C','GLO:C1C','GAL:C1X','BDS:C2I'],
      gimProviders: ['IGS','CODE','ESA','JPL','UPC','CAS','EMR','WHU'],
      gimProductGrades: []
    },
    advancedChoicesLoading: false, advancedChoiceKey: '', pendingAdvancedChoiceKey: '', activeChoiceInput: '', choiceNeedle: '', choiceGroup: 'all',
    customPresets: [], editingCustomPreset: '', planSignature: null,
    planVisible: { observation: 100, precise: 100 },
    sourceChipsSignature: null, sourcePickerSignature: null,
    observationRowsSignature: null, preciseRowsSignature: null,
    countryOptionsSignature: null, productLevelSignature: null,
    map: { scale: 1, x: 0, y: 0, dragging: false, selecting: false, start: null }
  };
  const familyValues = ['all','sp3','clk','erp/eop','dcb','orbex','ionex','tro','sinex','antex','broadcast','osb','upd','vmf'];
  const preciseFamilyLabels = {sp3:'SP3',clk:'CLK','erp/eop':'ERP/EOP',dcb:'DCB',orbex:'ORBEX',ionex:'GIM',tro:'TRO',sinex:'SINEX',antex:'ANTEX',broadcast:'BRDC',osb:'OSB',upd:'UPD',vmf:'VMF'};
  function preciseProductTypeLabel(family) {
    const raw = String(family || '').trim(), key = raw.toLowerCase();
    if (preciseFamilyLabels[key]) return preciseFamilyLabels[key];
    if (raw === '对流层') return 'TRO';
    if (raw === '广播星历') return 'BRDC';
    const token = key.split(/[\s/]/)[0];
    const aliases = {erp:'erp/eop',eop:'erp/eop',att:'orbex',gim:'ionex',brdc:'broadcast',vmf1:'vmf',vmf3:'vmf'};
    return preciseFamilyLabels[aliases[token] || token] || raw;
  }
  let toastTimer = 0;
  let filterTimer = 0;
  let mapDrawFrame = 0;
  let mapHoverTimer = 0;
  let lastStatus = '';
  let lastQuerySummary = '';
  let pointerMoved = false;
  let firstFrameScheduled = false;
  let firstFrameAnnounced = false;

  const esc = value => String(value ?? '').replace(/[&<>"']/g, ch => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch]));
  const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
  const post = (type, payload = {}) => {
    if (bridge) bridge.postMessage(JSON.stringify({ type, ...payload }));
  };
  function scheduleFirstFrameReady() {
    if (!bridge || firstFrameScheduled || firstFrameAnnounced) return;
    firstFrameScheduled = true;
    const announce = () => {
      if (firstFrameAnnounced) return;
      firstFrameAnnounced = true;
      post('first-frame-ready');
    };
    // Two animation frames normally place the fully styled state on the
    // compositor before native code swaps out the startup surface. Hidden
    // WebView controllers may throttle rAF, so the timer is a safe fallback.
    requestAnimationFrame(() => requestAnimationFrame(announce));
    setTimeout(announce, 260);
  }
  const toast = message => {
    const node = $('#toast');
    node.textContent = localizeUiText(message);
    node.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => node.classList.remove('show'), 2200);
  };
  // All text assigned after the initial DOM pass goes through the same
  // dictionary as static labels.  This keeps live status/count strings from
  // leaking Chinese into the English UI (the MutationObserver cannot always
  // see text inserted into an innerHTML template before it is painted).
  const setNodeText = (node, value) => {
    if (!node) return;
    const text = localizeUiText(value);
    if (node.textContent !== text) node.textContent = text;
  };
  const setText = (selector, value) => setNodeText($(selector), value);
  const setFilterBadge = (selector, count) => {
    const node = $(selector); if (!node) return;
    const text = count > 0 ? String(count) : '';
    if (node.textContent !== text) node.textContent = text;
    node.classList.toggle('hidden', count <= 0);
  };
  const setValue = (selector, value) => { const node = $(selector); if (node && value !== undefined && value !== null && node.value !== String(value)) node.value = value; };
  const setSelectedIndex = (selector, value) => {
    const node = $(selector), index = Number(value);
    if (node && Number.isFinite(index) && index >= 0 && index < node.options.length) node.selectedIndex = index;
  };
  const timeText = () => new Date().toLocaleTimeString('zh-CN', { hour12:false, hour:'2-digit', minute:'2-digit', second:'2-digit' });

  const runtimeLog = window.GnssRuntimeLog.create();
  let infoRenderFrame = 0;
  function addInfo(message, kind = 'notice', options = {}) {
    const accepted = runtimeLog.consider(message, kind, { stream:state.mode, ...options });
    if (!accepted) return;
    const previous = accepted.key && state.info.find(item => item.key === accepted.key);
    const entry = { time:timeText(), kind:accepted.kind, message:accepted.fullText || accepted.text, key:accepted.key, limit:accepted.limit };
    if (previous) Object.assign(previous, entry);
    else state.info.push({ id:++state.infoSerial, ...entry });
    if (state.info.length > 80) state.info.splice(0, state.info.length-80);
    if (!infoRenderFrame) infoRenderFrame = requestAnimationFrame(() => {
      infoRenderFrame = 0; renderInfo();
    });
  }

  const runtimeMessageText = (item, clip = false) => {
    const text = localizeUiText(item.message), limit = item.limit || (item.kind === 'warning' ? 360 : 260);
    return clip && text.length > limit ? text.slice(0, limit - 1) + '…' : text;
  };
  function renderInfo() {
    const labels = { notice:localizeUiText('提示'), warning:localizeUiText('警告'), download:localizeUiText('下载') };
    const rows = state.infoFilter === 'all' ? state.info : state.info.filter(item => item.kind === state.infoFilter);
    setText('#infoAllCount', state.info.length);
    setText('#infoNoticeCount', state.info.filter(item => item.kind === 'notice').length);
    setText('#infoWarningCount', state.info.filter(item => item.kind === 'warning').length);
    setText('#infoDownloadCount', state.info.filter(item => item.kind === 'download').length);
    setText('#infoSummary', state.info.length ? `共 ${state.info.length} 条运行信息` : '运行提示、筛选结果和下载状态');
    const scroll = $('#infoScroll');
    scroll.innerHTML = rows.length ? rows.map(item => `<div class="info-line ${item.kind}" data-info-id="${item.id}"><time>${esc(item.time)}</time><span class="info-kind">${labels[item.kind] || localizeUiText('提示')}</span><span class="info-message">${esc(runtimeMessageText(item, true))}</span><button type="button" class="info-dismiss" data-info-dismiss="${item.id}" title="${localizeUiText('删除此条信息')}" aria-label="${localizeUiText('删除此条信息')}">×</button></div>`).join('') : `<div class="info-empty">${localizeUiText('当前分类暂无信息')}</div>`;
    const newest = rows.length ? scroll.lastElementChild : null;
    if (newest) newest.scrollIntoView({ block:'start' });
  }

  function currentFilterSummary(current) {
    if (!current) return [];
    const tags=[];
    if (state.mode === 'precise') {
      const presetSelect=$('#quickPreset'),preset=presetSelect?.selectedOptions?.[0]?.textContent?.trim();
      if (preset && presetSelect.value!=='none') tags.push(`预设：${preset}`);
      if (current.preciseStartDate||current.preciseEndDate) tags.push(`日期：${current.preciseStartDate||'—'} 至 ${current.preciseEndDate||'—'} UTC`);
      const family=$('#productFamily')?.selectedOptions?.[0]?.textContent?.trim();
      const latency=$('#preciseLatency')?.selectedOptions?.[0]?.textContent?.trim();
      const center=$('#preciseCenter')?.selectedOptions?.[0]?.textContent?.trim();
      if (family) tags.push(`产品：${family}`);
      if (latency && !/全部/.test(latency)) tags.push(`等级：${latency}`);
      if (center && !/全部/.test(center)) tags.push(`机构：${center}`);
      const completenessMode=String(current.preciseCompletenessMode||$('#preciseCompletenessMode')?.value||'ignore');
      if (completenessMode !== 'ignore') {
        const value=current.preciseCompletenessValue ?? $('#preciseCompletenessValue')?.value ?? '';
        tags.push(`完整性：${value}${completenessMode==='days'?' 天':'%'}`);
      }
      if (String(current.preciseSearch||'').trim()) tags.push(`搜索：${current.preciseSearch}`);
      return tags;
    }
    const preset=$('#observationPreset')?.selectedOptions?.[0]?.textContent?.trim();
    if(preset) tags.push(`预设：${preset}`);
    if(current.startDate||current.endDate) tags.push(`日期：${current.startDate||'—'} 至 ${current.endDate||'—'} UTC`);
    const networks=Array.isArray(current.networks)?current.networks.filter(item=>item.checked).map(item=>item.name):[];
    if(networks.length) tags.push(`站网：${networks.length<=3?networks.join('、'):`${networks.length} 个`}`);
    const country=String(current.country||'').trim();
    if(country&&country!=='All countries') tags.push(`国家：${country}`);
    const bounds=[];
    if(String(current.latMin||'').trim()||String(current.latMax||'').trim()) bounds.push(`纬度 ${current.latMin||'—'}° 至 ${current.latMax||'—'}°`);
    if(String(current.lonMin||'').trim()||String(current.lonMax||'').trim()) bounds.push(`经度 ${current.lonMin||'—'}° 至 ${current.lonMax||'—'}°`);
    if(bounds.length) tags.push(bounds.join('，'));
    if(String(current.filter||'').trim()) tags.push(`搜索：${current.filter}`);
    const advanced=current.advanced||{}, dateAdvanced=current.dateAdvanced||{};
    const advancedLabels=[];
    if(advanced.completenessMode&&advanced.completenessMode!=='ignore') advancedLabels.push('完整性');
    if(Number(advanced.rinexVersion)>0) advancedLabels.push(`RINEX ${advanced.rinexVersion}`);
    if(advanced.referenceFrame&&advanced.referenceFrame!=='ignore') advancedLabels.push('ITRF/IGS 框架');
    if(advanced.gimStationMode&&advanced.gimStationMode!=='ignore') advancedLabels.push('GIM 测站');
    ['systemsInclude','systemsExclude','receiverInclude','receiverExclude','antennaInclude','antennaExclude','signalCodesInclude','signalCodesExclude'].forEach(key=>{if(String(advanced[key]||'').trim())advancedLabels.push('高级筛选');});
    if(advancedLabels.length) tags.push(`高级：${[...new Set(advancedLabels)].join('、')}`);
    if(['kpMode','dstMode','f107Mode'].some(key=>dateAdvanced[key]&&dateAdvanced[key]!=='ignore')) tags.push('空间天气日期筛选');
    return tags;
  }
  function renderActiveFilters(current=state.latest) {
    const root=$('#infoActiveFilters'); if(!root)return;
    const tags=currentFilterSummary(current);
    const presetSelect=$('#quickPreset');
    const customPresetTag=state.mode==='precise'&&presetSelect?.value.startsWith('custom:')
      ?`预设：${presetSelect.selectedOptions[0]?.textContent?.trim()}`:null;
    root.innerHTML=`<b>当前筛选</b>${tags.length?tags.map(tag=>{
      const custom=tag===customPresetTag;
      const text=custom&&uiPrefs.language==='en'?tag.replace(/^预设：/,'Preset: '):tag;
      return `<span class="filter-chip"${custom?' data-no-translate':''}>${esc(text)}</span>`;
    }).join(''):'<span class="filter-chip muted">未设置额外筛选</span>'}`;
  }

  function setMode(mode, notify = true) {
    if (mode !== 'observation' && mode !== 'precise') return;
    state.mode = mode;
    $$('.mode-tab').forEach(button => button.classList.toggle('active', button.dataset.mode === mode));
    $('#observationPage').classList.toggle('active', mode === 'observation');
    $('#precisePage').classList.toggle('active', mode === 'precise');
    const infoPanel = $('#informationPanel');
    const infoSlot = $(mode === 'precise' ? '#preciseInfoSlot' : '#observationInfoSlot');
    if (infoPanel && infoSlot && infoPanel.parentElement !== infoSlot) infoSlot.appendChild(infoPanel);
    setText('#headerNote', mode === 'observation' ? '观测数据检索 · 空间筛选 · 批量下载' : '轨道 · 钟差 · 偏差 · 姿态 · 大气产品一站式下载');
    if (notify) post('mode', { mode });
    renderDownloadPlan();
    updateDownloadDock();
    requestAnimationFrame(() => { renderInfo(); scheduleMapDraw(); });
  }

  function renderSources(networks = []) {
    const root = $('#sourceChips');
    if (!networks.length) {
      if (state.sourceChipsSignature !== 'empty') {
        root.innerHTML = '<button class="source-more" disabled>等待本地索引…</button>';
        state.sourceChipsSignature = 'empty';
      }
      renderSourcePicker([]);
      return;
    }
    // 启动阶段 WebView 可能先以尚未最大化的宽度完成第一次渲染。这里固定展示前 7 个
    // 常用站网，避免窗口稍后最大化或点击任一站网后才从 5 个跳成 7 个。
    const featured = networks.slice(0,7);
    const remaining = networks.length - featured.length;
    const signature = JSON.stringify([remaining, featured.map(source => source.name)]);
    if (signature !== state.sourceChipsSignature) {
      root.innerHTML = featured.map(source => `<button class="source-chip ${source.checked ? 'checked' : ''} ${source.available ? '' : 'unavailable'}" data-source="${esc(source.name)}" title="${source.available ? '本地知识库可用' : '本地知识库尚未收录'}">${esc(source.name)}</button>`).join('')
        + (remaining > 0 ? `<button class="source-more" id="openSourcePicker" type="button">更多站网 +${remaining}</button>` : '');
      state.sourceChipsSignature = signature;
    } else {
      const chips=$$('.source-chip',root);
      featured.forEach((source,index)=>{
        const chip=chips[index];if(!chip)return;
        chip.classList.toggle('checked',Boolean(source.checked));
        chip.classList.toggle('unavailable',!source.available);
        const title=localizeUiText(source.available?'本地知识库可用':'本地知识库尚未收录');
        if(chip.title!==title)chip.title=title;
      });
    }
    renderSourcePicker(networks);
  }

  function renderSourcePicker(networks = [], force = false) {
    const root=$('#sourcePickerGrid');
    if(!root)return;
    const needle=state.sourceNeedle.trim().toLocaleLowerCase();
    const rows=networks.filter(source=>!needle||String(source.name||'').toLocaleLowerCase().includes(needle));
    const available=networks.filter(source=>source.available).length;
    const checked=networks.filter(source=>source.checked).length;
    const selectedAvailable=networks.filter(source=>source.available&&source.checked).length;
    setText('#sourceSelectAvailable',available>0&&selectedAvailable===available?'取消全选所有站网':'全选所有站网');
    setText('#sourcePickerSummary',`${networks.length} 个数据源 · ${available} 个本地可用 · 已选择 ${checked} 个${needle?` · 当前显示 ${rows.length} 个`:''}`);
    if (!force && $('#sourceLayer').classList.contains('hidden')) return;
    const signature=JSON.stringify([needle,rows.map(source=>source.name)]);
    if (signature !== state.sourcePickerSignature) {
      root.innerHTML=rows.length?rows.map(source=>`<button class="source-picker-item ${source.checked?'checked':''} ${source.available?'':'unavailable'}" type="button" data-source="${esc(source.name)}"><span class="source-check">✓</span><b title="${esc(source.name)}">${esc(source.name)}</b><em>${esc(localizeUiText(source.available?'可用':'未索引'))}</em></button>`).join(''):'<div class="detail-empty"><span><svg class="ico" aria-hidden="true"><use href="#i-search"></use></svg></span><b>没有匹配的数据源</b><small>请尝试其他站网名称</small></div>';
      state.sourcePickerSignature=signature;
    } else {
      const buttons=$$('.source-picker-item',root);
      rows.forEach((source,index)=>{
        const button=buttons[index];if(!button)return;
        button.classList.toggle('checked',Boolean(source.checked));
        button.classList.toggle('unavailable',!source.available);
        setNodeText($('em',button),source.available?'可用':'未索引');
      });
    }
  }

  function openSourceSettings() {
    state.sourceNeedle='';setValue('#sourceSearch','');renderSourcePicker(Array.isArray(state.latest.networks)?state.latest.networks:[],true);
    closeRowMenu();closeStationDetail();
    const layer=$('#sourceLayer');layer.classList.remove('hidden');layer.setAttribute('aria-hidden','false');
    setTimeout(()=>$('#sourceSearch').focus(),40);
  }

  const countryContinents = [
    ['亚洲',new Set('AFG ARM AZE BHR BGD BTN BRN KHM CHN CYP GEO HKG IND IDN IRN IRQ ISR JPN JOR KAZ PRK KOR KWT KGZ LAO LBN MAC MYS MDV MNG MMR NPL OMN PAK PHL QAT SAU SGP LKA SYR TJK THA TLS TKM TUR ARE UZB VNM YEM PSE'.split(' '))],
    ['欧洲',new Set('ALB AND AUT BLR BEL BIH BGR HRV CZE DNK EST FIN FRA DEU GRC VAT HUN ISL IRL ITA LVA LIE LTU LUX MLT MDA MCO MNE NLD MKD NOR POL PRT ROU RUS SMR SRB SVK SVN ESP SWE CHE UKR GBR XKK'.split(' '))],
    ['非洲',new Set('DZA AGO BEN BWA BFA BDI CPV CMR CAF TCD COM COG COD CIV DJI EGY GNQ ERI SWZ ETH GAB GMB GHA GIN GNB KEN LSO LBR LBY MDG MWI MLI MRT MUS MAR MOZ NAM NER NGA RWA STP SEN SYC SLE SOM ZAF SSD SDN TZA TGO TUN UGA ZMB ZWE ESH SHN'.split(' '))],
    ['北美洲',new Set('ATG BHS BRB BLZ CAN CRI CUB DMA DOM SLV GRD GTM HTI HND JAM MEX NIC PAN KNA LCA VCT TTO USA ABW AIA BES BMU VGB CYM CUW GLP MTQ MSR PRI SXM TCA VIR GRL SPM MAF'.split(' '))],
    ['南美洲',new Set('ARG BOL BRA CHL COL ECU GUY PRY PER SUR URY VEN FLK GUF SGS'.split(' '))],
    ['大洋洲',new Set('AUS FJI KIR MHL FSM NRU NZL PLW PNG WSM SLB TON TUV VUT NCL PYF GUM ASM COK NIU MNP PCN TKL WLF UMI NFK CXR CCK'.split(' '))],
    ['南极洲',new Set('ATA ATF BVT HMD'.split(' '))]
  ];
  function countryContinent(code) {
    const group=countryContinents.find(([,codes])=>codes.has(code));
    return group ? group[0] : '其他地区';
  }
  function normalizeCountryOptions(countries = []) {
    const seen=new Set();
    return countries.map(item=>typeof item==='string'?{code:item,flagIndex:-1}:item||{})
      .map(item=>{const area=window.MarkCountries.areaCode(item.code||item.value||'');
        const code=window.MarkCountries.selectableRegion(area)?area:window.MarkCountries.countryCode(area);
        return {code,flagIndex:window.MarkCountries.flagIndex(code)};})
      .filter(item=>item.code.length===3&&item.code!=='ALL'&&!seen.has(item.code)&&seen.add(item.code))
      .map(item=>({...item,flagIndex:Number.isInteger(item.flagIndex)?item.flagIndex:-1,continent:countryContinent(item.code)}))
      .sort((a,b)=>a.continent.localeCompare(b.continent,'zh-CN')||a.code.localeCompare(b.code));
  }
  function updateCountryTrigger() {
    const count=state.selectedCountries.length;
    const label=!count?'全部国家/地区':count<=3?state.selectedCountries.join('、'):`已选择 ${count} 个国家/地区`;
    setText('#countrySelectionLabel',label);
    const trigger=$('#openCountryPicker');
    if(trigger){trigger.classList.toggle('active',count>0);trigger.title=count?`已选择：${state.selectedCountries.join('、')}`:'未限定国家';}
  }
  function renderCountries(countries = []) {
    const signature=JSON.stringify(countries);
    if(signature!==state.countryOptionsSignature){
      state.countryOptions=normalizeCountryOptions(countries);
      state.countryOptionsSignature=signature;
    }
    const available=new Set(state.countryOptions.map(item=>item.code));
    state.selectedCountries=state.selectedCountries.filter(code=>available.has(code));
    updateCountryTrigger();
  }
  function countryOptionFlag(option) {
    return option.flagIndex>=0?`<span class="country-flag" style="--flag-index:${option.flagIndex}" role="img" aria-label="${esc(fullCountryName(option.code))} ${uiPrefs.language === 'en' ? 'flag' : '旗帜'}"></span>`:'<span class="country-flag fallback" aria-hidden="true">?</span>';
  }
  function renderCountryPicker() {
    const root=$('#countryPickerGroups');if(!root)return;
    const needle=state.countryNeedle.trim().toUpperCase();
    const rows=state.countryOptions.filter(item=>!needle||window.MarkCountries.searchText(item.code).toUpperCase().includes(needle));
    const selected=new Set(state.pendingCountries);
    const regions=state.countryOptions.filter(item=>window.MarkCountries.selectableRegion(item.code)||window.MarkCountries.neutral(item.code)).length;
    const countries=state.countryOptions.length-regions;
    setText('#countryPickerSummary',uiPrefs.language==='en'
      ? `${countries} countries${regions?` · ${regions} regions`:''} · ${selected.size} selected${needle?` · ${rows.length} shown`:''}`
      : `${countries} 个国家${regions?` · ${regions} 个地区`:''} · 已选择 ${selected.size} 项${needle?` · 当前显示 ${rows.length} 项`:''}`);
    const groupNames=[...countryContinents.map(([name])=>name),'其他地区'];
    root.innerHTML=groupNames.map(name=>{
      const items=rows.filter(item=>item.continent===name).sort((a,b)=>name==='南极洲'?(a.code==='ATF'?-1:b.code==='ATF'?1:a.code==='ATA'?-1:b.code==='ATA'?1:0):0);if(!items.length)return '';
      const allItems=state.countryOptions.filter(item=>item.continent===name);
      const allSelected=allItems.length>0&&allItems.every(item=>selected.has(item.code));
      const selectLabel=uiPrefs.language==='en'?(allSelected?'Clear continent':'Select all'):(allSelected?'清空本洲':'全选本洲');
      return `<section class="country-continent"><header><b>${name}</b><span>${items.length}</span><button class="country-continent-select" type="button" data-continent-select="${esc(name)}">${selectLabel}</button></header><div class="country-option-grid">${items.map(item=>`<button class="country-option ${selected.has(item.code)?'checked':''}" type="button" data-country-code="${item.code}" title="${esc(fullCountryName(item.code))}">${countryOptionFlag(item)}<span class="country-option-label"><b>${item.code}</b><small>${esc(fullCountryName(item.code))}</small></span><span class="country-option-check">✓</span></button>`).join('')}</div></section>`;
    }).join('')||'<div class="detail-empty"><span><svg class="ico" aria-hidden="true"><use href="#i-search"></use></svg></span><b>没有匹配的国家/地区</b><small>请输入名称或 ISO 三字母地区代码</small></div>';
  }
  function openCountrySettings() {
    state.pendingCountries=[...state.selectedCountries];state.countryNeedle='';setValue('#countrySearch','');renderCountryPicker();
    closeRowMenu();closeStationDetail();
    const layer=$('#countryPickerLayer');layer.classList.remove('hidden');layer.setAttribute('aria-hidden','false');
    setTimeout(()=>$('#countrySearch').focus(),40);
  }

  function normalizedObservation(row) {
    const pct = clamp(row[20] !== undefined ? Number(row[20]) : parseFloat(String(row[6] || '0').replace('%','')) || 0, 0, 100);
    return {
      id: row[0] || '—', country: window.MarkCountries.countryCode(row[2]) || '—', area: window.MarkCountries.areaCode(row[21] || row[2]) || '—',
      receiver: row[3] || '—', system: row[19] || '—', antenna: antennaModelLabel(row[4], row[22]), radome: row[22] || '—',
      sources: row[5] || '—', percent: pct, status: row[7] || 'Missing',
      selected: Boolean(row[8]), lat: Number(row[9]), lon: Number(row[10]),
      hasCoordinate: Boolean(row[11]), network: row[12] || '—', format: row[13] || '—',
      interval: row[14] || '—', coverage: row[17] || `${pct}%`,
      flagIndex: window.MarkCountries.flagIndex(row[21] || row[2]), raw: row
    };
  }

  function antennaModelLabel(model, radome) {
    const text=String(model||'').trim(), cover=String(radome||'').trim();
    if(cover && text.toUpperCase().endsWith(cover.toUpperCase())) {
      const boundary=text.length-cover.length;
      if(boundary>0 && /\s/.test(text[boundary-1])) return text.slice(0,boundary).trim()||'—';
    }
    return text||'—';
  }

  function countryFlag(row, extraClass = '') {
    if (row.flagIndex < 0) return `<span class="country-flag fallback ${extraClass}" aria-hidden="true">?</span>`;
    return `<span class="country-flag ${extraClass}" style="--flag-index:${row.flagIndex}" role="img" aria-label="${esc(stationRecordedCountryName(row.area||row.country))} ${uiPrefs.language === 'en' ? 'flag' : '旗帜'}"></span>`;
  }

  function findObservation(stationId) {
    return state.observationRows.map(normalizedObservation)
      .find(row => row.id.toUpperCase() === String(stationId || '').toUpperCase());
  }

  function splitTags(value) {
    const parts = String(value || '—').split(/[,;/]/).map(item => item.trim()).filter(Boolean);
    const shown = parts.slice(0, 2).map(item => `<span class="tag">${esc(item)}</span>`).join('');
    return `<span class="tag-list">${shown}${parts.length > 2 ? `<span class="tag">+${parts.length - 2}</span>` : ''}</span>`;
  }

  function statusClass(status, percent) {
    const lower = String(status).toLowerCase();
    if (lower.includes('pending') || lower.includes('unverified') || lower.includes('待核实')) return 'partial';
    if (lower.includes('missing') || percent <= 0) return 'missing';
    if (lower.includes('partial') || percent < 99) return 'partial';
    return '';
  }

  function observationCoverageLabel(value) {
    const raw = String(value || '—');
    if (uiPrefs.language !== 'en') return raw;
    const estimated = /^预估\s*([\d.]+%)\s*[·•]\s*(\d+)\s*天待确认$/.exec(raw);
    return estimated ? `Estimated ${estimated[1]} · ${estimated[2]} days unverified` : raw;
  }

  function observationCoverageMarkup(value) {
    const raw = String(value || '—');
    const estimated = uiPrefs.language === 'en'
      ? /^预估\s*([\d.]+%)\s*[·•]\s*(\d+)\s*天待确认$/.exec(raw) : null;
    return estimated
      ? `<span class="station-coverage-copy"><b>Est. ${esc(estimated[1])}</b><small>${esc(estimated[2])} days unverified</small></span>`
      : `<span class="station-coverage-copy">${esc(observationCoverageLabel(raw))}</span>`;
  }

  function setupTableCellTooltips() {
    const table = $('#stationTable');
    const tooltip = document.createElement('div');
    tooltip.id = 'tableCellTooltip';
    tooltip.className = 'table-cell-tooltip hidden';
    tooltip.setAttribute('role', 'tooltip');
    tooltip.setAttribute('data-no-translate', '');
    document.body.appendChild(tooltip);
    let hoveredCell = null, timer = null, x = 0, y = 0;
    const hide = () => {
      clearTimeout(timer);
      hoveredCell?.removeAttribute('aria-describedby');
      hoveredCell = null;
      tooltip.classList.add('hidden');
    };
    const position = () => {
      const size = tooltip.getBoundingClientRect();
      const left = Math.max(12, Math.min(x + 12, innerWidth - size.width - 12));
      const top = y + size.height + 24 <= innerHeight ? y + 12 : Math.max(12, y - size.height - 12);
      tooltip.style.left = `${left}px`;
      tooltip.style.top = `${top}px`;
    };
    table.addEventListener('pointerover', event => {
      if (event.pointerType === 'touch') return;
      const cell = event.target.closest('td[data-full-text]');
      if (!cell || cell === hoveredCell) return;
      hide();
      const clipped = [cell, ...cell.querySelectorAll('*')].some(node => node.scrollWidth > node.clientWidth + 1);
      if (!clipped || !cell.dataset.fullText) return;
      hoveredCell = cell; x = event.clientX; y = event.clientY;
      timer = setTimeout(() => {
        if (hoveredCell !== cell || !cell.isConnected) return;
        tooltip.textContent = cell.dataset.fullText;
        tooltip.classList.remove('hidden');
        cell.setAttribute('aria-describedby', tooltip.id);
        position();
      }, 180);
    });
    table.addEventListener('pointermove', event => {
      if (!hoveredCell) return;
      x = event.clientX; y = event.clientY;
      if (!tooltip.classList.contains('hidden')) position();
    });
    table.addEventListener('pointerout', event => {
      if (hoveredCell && !hoveredCell.contains(event.relatedTarget)) hide();
    });
    table.addEventListener('pointerdown', hide);
    document.addEventListener('scroll', hide, true);
    document.addEventListener('keydown', event => { if (event.key === 'Escape') hide(); });
    window.addEventListener('blur', hide);
    window.addEventListener('resize', hide);
    state.hideTableCellTooltip = hide;
  }

  function renderObservationRows() {
    const signature=JSON.stringify([
      state.observationRows,state.resultNeedle,state.ascending,state.sortColumn,
      state.rowsSelectedOnly,[...hiddenColumns].sort(),uiPrefs.language
    ]);
    if(signature===state.observationRowsSignature)return;
    state.observationRowsSignature=signature;
    state.hideTableCellTooltip?.();
    const needle = state.resultNeedle.trim().toLowerCase();
    let rows = state.observationRows.map(normalizedObservation);
    if (needle) rows = rows.filter(row => row.raw.some(value => String(value).toLowerCase().includes(needle)));
    rows.sort((a,b) => (state.ascending ? 1 : -1) * ((state.sortColumn==='completeness' && a.percent!==b.percent) ? a.percent-b.percent : a.id.localeCompare(b.id, 'en')));
    syncSortHeader('#sortStationCompleteness',state.sortColumn==='completeness'?(state.ascending?'asc':'desc'):null);
    const body = $('#stationRows');
    if (!rows.length) {
      const columnCount = $('#stationTable').tHead.rows[0].cells.length;
      body.innerHTML = `<tr class="placeholder-row"><td colspan="${columnCount}"><div class="empty-table"><span><svg class="ico" aria-hidden="true"><use href="#i-search"></use></svg></span><b>暂无测站结果</b><small>查询后，结果会在这里显示且可独立滚动</small></div></td></tr>`;
      return;
    }
    body.innerHTML = rows.map(row => {
      const klass = statusClass(row.status, row.percent);
      const status = /pending|unverified|待核实/i.test(row.status)
        ? (uiPrefs.language === 'en' ? 'Unverified' : '待核实')
        : klass === 'missing' ? '不可用' : klass === 'partial' ? '部分可用' : '可用';
      return `<tr data-station="${esc(row.id)}" class="${row.selected ? 'is-selected' : ''}" tabindex="0" title="${esc(localizeUiText('单击查看测站详情'))}">
        <td class="check-col"><button class="row-check ${row.selected ? 'checked' : ''}" data-check="${esc(row.id)}" aria-label="${row.selected ? '取消选择' : '选择'}测站 ${esc(row.id)}" aria-pressed="${row.selected ? 'true' : 'false'}"></button></td>
        <td data-col="station" class="station-code" title="" data-full-text="${esc(row.id)}">${esc(row.id)}</td><td data-col="network" title="" data-full-text="${esc(row.network)}">${esc(row.network)}</td>
        <td data-col="country"><span class="country-cell" title="${esc(stationRecordedCountryName(row.area||row.country))}">${countryFlag(row)}${esc(window.MarkCountries.flagLabelCode(row.area||row.country))}</span></td><td data-col="format" title="" data-full-text="${esc(row.format)}">${esc(row.format)}</td><td data-col="interval" title="" data-full-text="${esc(row.interval)}">${esc(row.interval)}</td>
        <td data-col="system" title="" data-full-text="${esc(row.system)}">${esc(row.system)}</td><td data-col="receiver" title="" data-full-text="${esc(row.receiver)}">${esc(row.receiver)}</td><td data-col="antenna" title="" data-full-text="${esc(row.antenna)}">${esc(row.antenna)}</td><td data-col="radome" title="" data-full-text="${esc(row.radome)}">${esc(row.radome)}</td>
        <td data-col="availability" title="" data-full-text="${esc(observationCoverageLabel(row.coverage))}"><span class="availability ${klass}"><span class="availability-track"><i style="width:${row.percent}%"></i></span>${observationCoverageMarkup(row.coverage)}</span></td><td data-col="status" title="" data-full-text="${esc(localizeUiText(status))}"><span class="status-pill ${klass}">${esc(localizeUiText(status))}</span></td>
        <td class="action-col"><button class="row-more" data-row-menu="${esc(row.id)}" title="测站操作" aria-label="${esc(row.id)} 操作"><svg class="ico" aria-hidden="true"><use href="#i-ellipsis"></use></svg></button></td>
      </tr>`;
    }).join('');
    applyColumnVisibility();
  }

  function syncSortHeader(selector, direction) {
    const button=$(selector); if(!button)return;
    button.querySelector('span').textContent=direction==='asc'?'↑':direction==='desc'?'↓':'↕';
    button.closest('th').setAttribute('aria-sort',direction==='asc'?'ascending':direction==='desc'?'descending':'none');
    button.title=uiPrefs.language==='en'?'Click to reverse completeness order':'点击切换范围完整性升序／降序';
  }
  function renderCoverage(root, value) {
    if(window.GnssCoverage) window.GnssCoverage.mountCompact(root,value,uiPrefs.language==='en');
  }
  function stationFieldMarkup(key, value) {
    if(key==='国家')value=fullCountryName(value);
    if(key==='国家/地区'||key==='测站所属国家')value=stationRecordedCountryName(value);
    const missing=uiPrefs.language==='en'?'Not indexed':'未收录';
    return `<div class="detail-field"><span>${esc(localizeUiText(key))}</span><b ${value?'data-no-translate':''}>${esc(value||missing)}</b></div>`;
  }
  function stationStaticMarkup(detail, row) {
    const fields=Array.isArray(detail?.fields)?detail.fields:[];
    // Keep older cached detail messages compatible with the current single field.
    const hasCountry=fields.some(([key])=>key==='国家/地区');
    const visible=fields.filter(([key])=>key!=='原始国别代码'
      && !(hasCountry&&['测站所属国家','Site Log 国别记录'].includes(key)));
    return `<div class="station-detail-grid station-static-info">${visible.map(([key,value])=>{
      if(key==='Site Log 国别记录'||key==='测站所属国家')return stationFieldMarkup('国家/地区',row?.area||row?.country||value);
      return stationFieldMarkup(key,value);
    }).join('')}</div>`;
  }
  function equipmentPeriodMarkup(item) {
    const fields=[['接收机型号',item.receiver],['接收机序列号',item.receiverSerial],['固件版本',item.firmware],
      ['天线型号',antennaModelLabel(item.antenna,item.radome)],['天线序列号',item.antennaSerial],['天线罩',item.radome],['天线参考点',item.referencePoint]];
    const codes=Array.isArray(item.signals)?item.signals:[];
    const grouped={};
    for(const code of codes) {const [system,value]=String(code).split(':');(grouped[system]||=[]).push(value);}
    const names={G:'GPS',R:'GLONASS',E:'Galileo',C:'BeiDou',J:'QZSS',S:'SBAS',I:'NavIC'};
    const codeText=codes.length?Object.entries(grouped).map(([system,values])=>`<div class="period-code-group" data-no-translate><b>${esc(names[system]||system)}</b><span>${esc(values.join(' · '))}</span></div>`).join(''):`<p class="coverage-note">${esc(localizeUiText('未收录'))}</p>`;
    return `<div class="station-detail-grid">${fields.map(([key,value])=>stationFieldMarkup(key,value)).join('')}</div><details class="period-observation-codes"><summary>${esc(localizeUiText('观测码'))} · ${codes.length}</summary>${codeText}</details>`;
  }
  function detailExtraMarkup(detail) {
    const periods=Array.isArray(detail.periods)?detail.periods:[];
    const label=zh=>esc(localizeUiText(zh));
    return `<section class="station-index-details"><h3>${label('设备历史')} · ${periods.length} ${label('稳定区间')}</h3><p class="coverage-note">${label('设备历史使用完整收录时间线；观测码按知识库能力区间展示。')}</p><p class="coverage-note">${label('各项相同的相邻区间已合并；未收录的信息显示未收录。')}</p><div class="equipment-history">${periods.map((item,index)=>`<details class="equipment-period" data-period-index="${index}"><summary><b data-no-translate>${esc(item.start)} — ${esc(item.end)}</b><small data-no-translate>${esc(item.receiver||'—')} · ${esc(item.firmware||'—')}</small></summary><div class="equipment-period-body"></div></details>`).join('')||`<p class="coverage-note">${label('未收录')}</p>`}</div>${detail.error?`<p class="coverage-note">${esc(localizeUiText(detail.error))}</p>`:''}</section>`;
  }
  function requestStationDetail() {
    if(!state.detailStation)return;
    state.stationDetailData=null;state.stationDetailRequest=String(++state.detailRequestSerial);
    post('station-detail',{station:state.detailStation,request:state.stationDetailRequest});
  }
  function handleStationDetail(message) {
    if(message.station!==state.detailStation||message.request!==state.stationDetailRequest)return;
    state.stationDetailData=message;
    const row=findObservation(state.detailStation);
    if(!row)return;
    $('#stationDetailBody').innerHTML=stationDetailMarkup(row);
    renderCoverage($('#stationDateCoverage'),message.coverage);
  }
  function handleProductCoverage(message) {
    if(message.product!==state.detailProduct||message.request!==state.productCoverageRequest)return;
    state.productCoverage=message;
    renderCoverage($('#productDateCoverage'),message.coverage);
    renderClkAntex(message.antex);
    renderUpdInputs(message.upd);
    if(message.error) setText('#productCoverageNote',message.error);
  }

  function renderUpdInputs(info) {
    const target = $('#updInputsInfo');
    if (!target) return;
    if (!info) {
      target.innerHTML = `<p class="coverage-note">${esc(localizeUiText('未收录 UPD 配套文件声明'))}</p>`;
      return;
    }
    target.innerHTML = `<span>${esc(localizeUiText('UPD 文件头声明的配套产品'))}</span><p class="coverage-note">${esc(localizeUiText('所选范围内最新记录'))}: ${esc(info.date)}</p><b data-no-translate>${esc(info.fileName)}</b>`
      + (info.inputs || []).map(input => `<div class="clk-antex-group"><small data-no-translate>${esc(input.type)}</small><b data-no-translate title="${esc(input.fileName)}">${esc(input.fileName || '—')}</b></div>`).join('');
  }

  function fullCountryName(value, area) {
    return window.MarkCountries.displayName(area || value, uiPrefs.language);
  }
  function stationCountryName(value, area) {
    if(!(area||value))return '';
    return stationRecordedCountryName(area||value);
  }
  function stationRecordedCountryName(value) {
    const raw=String(value||'').trim();
    if(!raw||/^(?:N\/A|UNKNOWN|UNSPECIFIED)$/i.test(raw))return '';
    const code=/FALKLAND|MALVINAS/i.test(raw)?'FLK':window.MarkCountries.areaCode(raw);
    if(['TWN','HKG','MAC'].includes(code))return window.MarkCountries.countryName('CHN',uiPrefs.language);
    const rule=window.MarkCountries.areas[code];
    if(rule)return uiPrefs.language==='en'?rule.en:rule.zh;
    const name=window.MarkCountries.countryName(code,uiPrefs.language);
    return name===code?raw:name;
  }
  function renderClkAntex(info) {
    const target=$('#clkAntexInfo');if(!target)return;
    if(!info){target.innerHTML='<p class="coverage-note">'+esc(localizeUiText('CLK 头信息尚未接入'))+'</p>';return;}
    const labels={exact:'已声明文件',model_only:'仅声明模型',undeclared:'完整头未声明 ANTEX',failed:'头读取失败',retryable:'头读取待重试',unscanned:'尚未扫描'};
    const counts=Object.entries(info.counts||{}).map(([k,v])=>`${localizeUiText(labels[k]||k)} ${v} ${localizeUiText('天')}`).join(' · ');
    const note='按 CLK 文件头和注释收录；仅声明模型时无法确认历史文件版本。当前别名的内容可能随更新改变。';
    target.innerHTML=`<span>${esc(localizeUiText('CLK 使用的 ANTEX'))}</span>${info.error?`<p class="coverage-note">${esc(localizeUiText(info.error))}</p>`:''}<p class="coverage-note">${esc(counts)}</p><p class="coverage-note">${esc(localizeUiText(note))}</p>`+(info.groups||[]).map(g=>{
      const declarations=[...new Map((g.declarations||[]).map(d=>[`${d.system}|${d.model}|${d.exact}|${d.kind}`,d])).values()];
      const models=declarations.map(d=>`<b data-no-translate>${esc(d.model)}</b><small>${esc(d.system||'—')} · ${esc(localizeUiText(d.kind==='sys_pcvs_applied'?'SYS / PCVS APPLIED 声明':'CLK 注释中的引用'))} · ${esc(localizeUiText(d.exact?'文件名声明':'模型声明，版本未确认'))}</small>`).join('')||`<b>${esc(localizeUiText(labels[g.status]||g.status))}</b>`;
      const ranges=g.ranges||[];
      const rangeText=ranges.map(([a,b])=>a===b?a:`${a} — ${b}`).join('；');
      return `<div class="clk-antex-group">${models}<details><summary>${esc(localizeUiText('展开查看'))} · ${ranges.length} ${esc(localizeUiText('日期区间'))}</summary><p data-no-translate>${esc(rangeText)}</p></details></div>`;
    }).join('');
  }

  function stationDetailMarkup(row) {
    const klass=statusClass(row.status,row.percent);
    const status=/pending|unverified|待核实/i.test(row.status)
      ? (uiPrefs.language === 'en' ? 'Unverified' : '待核实')
      : klass==='missing'?'不可用':klass==='partial'?'部分可用':'可用';
    const pct=clamp(Number(row.percent)||0,0,100);
    const detail=state.stationDetailData;
    return `<section class="station-detail-hero">${countryFlag(row,'large')}<div><span data-no-translate>${esc(stationCountryName(row.country,row.area))}</span><h2 data-no-translate>${esc(row.id)}</h2><p data-no-translate>${esc(row.network)}</p></div><span class="status-pill ${klass}">${esc(localizeUiText(status))}</span></section>
      <section class="station-info">${detail?stationStaticMarkup(detail,row):`<p class="coverage-note">${esc(localizeUiText('正在读取测站信息…'))}</p>`}<div class="station-detail-grid"><div class="detail-field wide"><span>${esc(localizeUiText('范围完整性'))}</span><b>${esc(observationCoverageLabel(row.coverage))}</b><div class="detail-progress"><i style="width:${pct}%"></i></div><div id="stationDateCoverage" class="coverage-compact-slot"><p class="coverage-note">${esc(localizeUiText('正在读取日期覆盖…'))}</p></div></div></div>${detail?detailExtraMarkup(detail):''}</section>`;
  }
  function showStationDetail(row) {
    if (!row) return;
    state.detailStation = row.id;state.stationDetailRevision=state.latest.stationRevision;
    requestStationDetail();
    $('#stationDetailTitle').textContent = `${row.id} 测站详情`;
    $('#stationDetailBody').innerHTML = stationDetailMarkup(row);
    $('#detailToggleSelection').textContent = row.selected ? '取消选择' : '选择测站';
    $('#detailLocate').disabled = !row.hasCoordinate;
    const layer = $('#stationDetailLayer');
    layer.classList.remove('hidden');
    layer.setAttribute('aria-hidden', 'false');
    closeRowMenu();
  }

  function closeStationDetail() {
    state.detailStation = null;
    state.stationDetailData=null;state.stationDetailRequest='';
    const layer = $('#stationDetailLayer');
    layer.classList.add('hidden');
    layer.setAttribute('aria-hidden', 'true');
  }

  function openRowMenu(button, row) {
    if (!button || !row) return;
    state.actionStation = row.id;
    const menu = $('#rowActionMenu');
    $('#rowToggleLabel').textContent = row.selected ? '取消选择' : '选择测站';
    menu.classList.remove('hidden');
    const box = button.getBoundingClientRect();
    const width = 184, height = menu.offsetHeight || 148;
    menu.style.left = `${clamp(box.right - width, 8, window.innerWidth - width - 8)}px`;
    menu.style.top = `${clamp(box.bottom + 5, 8, window.innerHeight - height - 8)}px`;
  }

  function closeRowMenu() {
    state.actionStation = null;
    $('#rowActionMenu').classList.add('hidden');
  }

  function locateStation(row) {
    if (!row || !row.hasCoordinate) { toast('该测站暂无可用坐标'); return; }
    setMode('observation');
    state.mapView = 'all';
    $$('.segmented [data-map-view]').forEach(button => button.classList.toggle('active', button.dataset.mapView === 'all'));
    renderMapPoints();
    const [x,y] = project(row.lon,row.lat), scale = 5;
    Object.assign(state.map,{scale,x:500-x*scale,y:250-y*scale});
    state.focusedMapStation = row.id;
    applyMapTransform();
    $('#mapCard').scrollIntoView({behavior:'smooth',block:'center'});
    toast(`已定位 ${row.id} 测站`);
  }

  async function copyStation(row) {
    if (!row) return;
    const coordinates = row.hasCoordinate ? `${row.lat.toFixed(5)}, ${row.lon.toFixed(5)}` : '暂无';
    const text = [`测站：${row.id}`,`地图归属：${stationCountryName(row.country,row.area)}`,`站网：${row.network}`,`格式：${row.format}`,`采样率：${row.interval}`,`系统：${row.system}`,`接收机：${row.receiver}`,`天线：${row.antenna}`,`天线罩：${row.radome}`,`数据源：${row.sources}`,`范围完整性：${row.coverage}`,`坐标：${coordinates}`].join('\n');
    try { await navigator.clipboard.writeText(text); toast(`${row.id} 测站信息已复制`); }
    catch (_) { toast('当前环境无法访问剪贴板'); }
  }

  function hasPreciseValue(value) {
    const text = String(value ?? '').trim();
    return Boolean(text && text !== '—' && text !== '-');
  }

  function preciseAccess(product, pct) {
    if (product.localInventoryOnly) return { label:'本地记录', klass:'partial', detail:'未配置 UPD 在线下载地址；覆盖率表示本地存档记录。' };
    const status = String(product.status || '');
    const dateResolved = /按日期|日期读取|高速索引/.test(status) ||
      (!hasPreciseValue(product.preferredFileName) && !hasPreciseValue(product.sources));
    if (pct <= 0) return { label:'不可用', klass:'missing', detail:'所选日期范围内没有可下载记录' };
    if (dateResolved) return { label:'按日期解析', klass:pct < 99 ? 'partial' : '', detail:'下载时按所选日期解析实际文件名与可用镜像' };
    if (pct < 99) return { label:'部分可用', klass:'partial', detail:'已解析固定文件与镜像，但所选日期范围不完整' };
    return { label:'已解析', klass:'', detail:'文件名与候选镜像已经解析' };
  }

  function preciseStatusSummary(product, fallback) {
    if (product.localInventoryOnly) return '未配置 UPD 在线下载地址；覆盖率表示本地存档记录。';
    const parts = String(product.status || '').split(/[；;]/).map(part=>part.trim()).filter(Boolean)
      .filter(part=>!/^(?:查询)?范围(?:可用|完整)(?:[：:\s\d]|$)|^\d+\s*\/\s*\d+\s*天/.test(part));
    return parts.join('；') || fallback;
  }

  function preciseRangeLabel(availability, pct) {
    const text = String(availability || '').trim();
    if (!text) return { range:`${pct}%`, percent:'' };
    const withoutPercent = text.replace(/\s*[（(]\s*\d+(?:\.\d+)?%\s*[）)]\s*$/, '').trim();
    if (withoutPercent !== text || /天/.test(text)) return { range:withoutPercent || `${pct}%`, percent:`${pct}%` };
    return { range:text, percent:text === `${pct}%` ? '' : `${pct}%` };
  }

  function sinexPeriodLabel(product) {
    const labels={DAILY:'日解',WEEKLY:'周解',CUMULATIVE:'累计解',CUMULATIVE_LEGACY:'累计解',MULTI_DAY:'多日解'};
    return product.solutionClass ? localizeUiText(labels[product.solutionClass] || '其他解') : '';
  }
  function updateProductLevelOptions(reset=false) {
    const isSinex=$('#productFamily').value==='sinex', select=$('#preciseLatency');
    const signature=`${isSinex}:${uiPrefs.language}`;
    if(signature===state.productLevelSignature){
      if(reset&&select.selectedOptions[0]?.disabled)select.value='all';
      return;
    }
    state.productLevelSignature=signature;
    const periods=['all','daily','weekly','cumulative','multi-day','solution-other'];
    for(const option of select.options) {
      const allowed=isSinex?periods.includes(option.value):!['cumulative','multi-day','solution-other'].includes(option.value);
      option.hidden=!allowed;option.disabled=!allowed;
      if(option.value==='all')setNodeText(option,isSinex?'全部解算周期':'全部等级');
      if(option.value==='daily')setNodeText(option,isSinex?'日解':'每日');
      if(option.value==='weekly')setNodeText(option,isSinex?'周解':'每周');
    }
    setNodeText(select.closest('label').querySelector('span'),isSinex?'解算周期':'产品等级');
    const heading=$('#preciseTable th.product-latency-col');
    setNodeText(heading,isSinex?'周期':'等级');
    if(reset && select.selectedOptions[0]?.disabled)select.value='all';
  }

  function preciseSatelliteSystems(product) {
    const value = String(product.constellation || '').trim();
    // GIM identifies an ionosphere map, not a GNSS constellation.
    return /^IONEX\b/i.test(String(product.family || '')) || /^GIM$/i.test(value) ? '—' : value || '—';
  }

  function renderPreciseRows() {
    const pageSize = 100;
    state.precisePage = Math.min(state.precisePage, Math.max(0, Math.ceil(state.preciseRows.length / pageSize) - 1));
    const signature=JSON.stringify([state.preciseRows,state.precisePage,state.preciseSortDirection,uiPrefs.language]);
    if(signature===state.preciseRowsSignature)return;
    state.preciseRowsSignature=signature;
    const rows = state.preciseRows.map((product,index)=>({product,index}));
    if(state.preciseSortDirection) rows.sort((a,b)=>{
      const pct=p=>Number(p.expectedDays)>0?100*Number(p.availableDays)/Number(p.expectedDays):Number(p.completeness)||0;
      const difference=pct(a.product)-pct(b.product);
      // Only coverage follows the requested direction. Equal coverage keeps
      // centres and product IDs ascending, avoiding a WUM-heavy reverse-ID page.
      return (state.preciseSortDirection==='asc'?1:-1)*difference
        || String(a.product.center).localeCompare(String(b.product.center),'en')
        || String(a.product.id).localeCompare(String(b.product.id),'en')
        || String(a.product.family).localeCompare(String(b.product.family),'zh');
    });
    syncSortHeader('#sortPreciseCompleteness',state.preciseSortDirection);
    const body = $('#preciseRows');
    const offset = state.precisePage * pageSize;
    const end = Math.min(rows.length, offset + pageSize);
    setText('#preciseRowRange', rows.length ? `${offset + 1}–${end} / ${rows.length}` : '0–0');
    $('#precisePreviousRows').disabled = offset === 0;
    $('#preciseNextRows').disabled = end >= rows.length;
    if (!rows.length) {
      body.innerHTML = '<tr class="placeholder-row"><td colspan="9"><div class="empty-table"><span><svg class="ico ico-lg" aria-hidden="true"><use href="#i-cube"></use></svg></span><b>暂无产品结果</b><small>选择日期与筛选条件后查询</small></div></td></tr>';
      return;
    }
    body.innerHTML = rows.slice(offset, end).map(({product,index}) => {
      const pct = clamp(Number(product.completeness) || 0,0,100);
      const klass = pct >= 99 ? '' : pct > 0 ? 'partial' : 'missing';
      const availability = product.availability || `${pct}%`;
      const access = preciseAccess(product,pct);
      const range = preciseRangeLabel(availability,pct);
      return `<tr data-product-index="${index}" class="${product.selected ? 'is-selected' : ''}"><td class="check-col"><button class="row-check ${product.selected ? 'checked' : ''}" data-product-check="${index}" aria-label="${product.selected ? '取消选择' : '选择'}产品 ${esc(product.id)}" aria-pressed="${product.selected ? 'true' : 'false'}"></button></td><td class="station-code" title="${esc(localizeUiText(product.name || product.id))}">${esc(product.id)}</td><td>${esc(preciseProductTypeLabel(product.family))}</td><td>${esc(product.center)}</td><td>${esc(sinexPeriodLabel(product) || product.latency || '—')}</td><td>${esc(preciseSatelliteSystems(product))}</td><td>${esc(product.interval || '—')}</td><td class="product-access-cell"><span class="status-pill ${access.klass}" title="${esc(localizeUiText(access.detail))}">${esc(localizeUiText(access.label))}</span></td><td class="product-availability-cell" title="${esc(availability)}"><span class="availability ${klass}"><span class="availability-track"><i style="width:${pct}%"></i></span><span class="availability-copy"><b>${esc(range.range)}</b>${range.percent ? `<small>${esc(range.percent)}</small>` : ''}</span></span></td></tr>`;
    }).join('');
  }

  function showProductDetail(product) {
    if (!product) return;
    state.detailProduct=product.id;state.detailProductFamily=product.family;state.productCoverage=null;
    state.productCoverageRequest=String(++state.detailRequestSerial);
    const pct = clamp(Number(product.completeness) || 0,0,100);
    const access = preciseAccess(product,pct);
    const hasFile = hasPreciseValue(product.preferredFileName);
    const hasSources = hasPreciseValue(product.sources);
    const resolvedFields = hasFile || hasSources
      ? `${hasFile ? `<div class="detail-field wide"><span>首选文件名</span><b title="${esc(product.preferredFileName)}">${esc(product.preferredFileName)}</b></div>` : ''}${hasSources ? `<div class="detail-field wide"><span>候选数据源</span><b title="${esc(product.sources)}">${esc(product.sources)}</b></div>` : ''}`
      : `<div class="detail-field wide"><span>文件与下载源</span><b>${esc(localizeUiText(access.detail))}</b></div>`;
    const statusSummary = preciseStatusSummary(product,access.detail);
    const updFields = /^UPD/.test(product.family) ? `<div class="detail-field wide" id="updInputsInfo"><span>${esc(localizeUiText('UPD 文件头声明的配套产品'))}</span></div>` : '';
    const intervalField=product.formatVersion?`<div class="detail-field"><span>格式版本</span><b>${esc(product.formatVersion)}</b></div>`:`<div class="detail-field"><span>采样间隔</span><b>${esc(product.interval || '—')}</b></div>`;
    $('#productDetail').innerHTML = `<div class="detail-id">${esc(product.id)}</div>${product.name ? `<p class="detail-name">${esc(localizeUiText(product.name))}</p>` : ''}<section class="product-info"><div class="detail-grid"><div class="detail-field"><span>产品类型</span><b>${esc(preciseProductTypeLabel(product.family))}</b></div><div class="detail-field"><span>分析中心</span><b>${esc(preciseCenterFullName(product.center))}</b></div><div class="detail-field"><span>${esc(localizeUiText(product.solutionClass?'发布等级':'产品等级'))}</span><b>${esc(product.latency || '—')}</b></div>${product.solutionClass?`<div class="detail-field"><span>解算周期</span><b>${esc(sinexPeriodLabel(product))}</b></div>`:''}<div class="detail-field"><span>卫星系统</span><b>${esc(preciseSatelliteSystems(product))}</b></div>${intervalField}<div class="detail-field"><span>获取方式</span><b>${esc(localizeUiText(access.label))}</b></div><div class="detail-field wide"><span>${esc(localizeUiText(product.localInventoryOnly ? '本地记录覆盖率' : '范围完整性'))}</span><b>${esc(product.availability || `${pct}%`)}</b><div class="detail-progress"><i style="width:${pct}%"></i></div><div id="productDateCoverage" class="coverage-compact-slot"><p class="coverage-note">${esc(localizeUiText('正在读取日期覆盖…'))}</p></div><p id="productCoverageNote" class="coverage-note"></p></div>${resolvedFields}${updFields}${/^CLK/.test(product.family)?`<div class="detail-field wide clk-antex-info" id="clkAntexInfo"><span>CLK 使用的 ANTEX</span><p class="coverage-note">正在读取 CLK 头信息…</p></div>`:''}<div class="detail-field wide"><span>索引说明</span><b>${esc(localizeUiText(statusSummary))}</b></div></div></section>`;
    post('product-coverage',{product:product.id,family:product.family,request:state.productCoverageRequest});
  }

  function syncObservation(current) {
    state.observationRows = Array.isArray(current.rows) ? current.rows : [];
    const mapChanged = Array.isArray(current.mapPoints);
    if (mapChanged) state.mapPoints = current.mapPoints;
    const mapSelectionChanged = Array.isArray(current.mapSelection);
    if (mapSelectionChanged) {
      const selected = new Set(current.mapSelection.map(value => String(value).toUpperCase()));
      for (const point of state.mapPoints) point[3] = selected.has(String(point[0]).toUpperCase());
    }
    if (current.filter !== undefined) {
      state.resultNeedle = String(current.filter || '');
      setValue('#stationFilter', state.resultNeedle);
      setValue('#resultSearch', state.resultNeedle);
    }
    if (current.sortAscending !== undefined) {
      state.ascending = Boolean(current.sortAscending);
      state.sortColumn = current.sortColumn || 'completeness';
      setText('#sortStationsLabel', state.ascending ? '站号 A–Z' : '站号 Z–A');
    }
    if (current.rowsSelectedOnly !== undefined) {
      state.rowsSelectedOnly = Boolean(current.rowsSelectedOnly);
      const selectedOnlyButton = $('#selectedRowsOnly');
      selectedOnlyButton.classList.toggle('active', state.rowsSelectedOnly);
      selectedOnlyButton.setAttribute('aria-pressed', state.rowsSelectedOnly ? 'true' : 'false');
      setNodeText(selectedOnlyButton,state.rowsSelectedOnly ? '显示全部' : '仅看已选');
    }
    renderSources(Array.isArray(current.networks) ? current.networks : []);
    if(current.country!==undefined){
      const countryValue=String(current.country||'');
      state.selectedCountries=countryValue&&countryValue!=='All countries'
        ? [...new Set(countryValue.split(',').map(code=>{const area=window.MarkCountries.areaCode(code);return window.MarkCountries.selectableRegion(area)?area:window.MarkCountries.countryCode(area);}).filter(code=>code.length===3))]
        : [];
    }
    renderCountries(Array.isArray(current.countries) ? current.countries : []);
    setValue('#observationPreset', current.observationPreset || 'normal');
    if (current.selectionCount !== undefined) setValue('#selectionCount', current.selectionCount);
    if (current.minDistance !== undefined) setValue('#minimumDistance', current.minDistance);
    if (current.maxNeighbor !== undefined) setValue('#maximumNeighbor', current.maxNeighbor);
    setText('#sourceSummary', `${current.sources || 0}/${(current.networks || []).length || 0} 已选择 · ${current.availableSources || 0} 个本地可用`);
    const advanced=current.advanced||{}, dateAdvanced=current.dateAdvanced||{};
    const qualityModes=['observationCompletenessMode','latencyMode','positionTimeSeriesMode','residualsMode','multipathMode','cycleSlipsMode'];
    const qualityActive=qualityModes.some(key=>advanced[key]&&advanced[key]!=='ignore');
    const advancedActive=(advanced.completenessMode&&advanced.completenessMode!=='ignore')||Boolean(advanced.sampleInterval)||Number(advanced.rinexVersion)>0||(advanced.referenceFrame&&advanced.referenceFrame!=='ignore')||qualityActive||((advanced.gimStationMode&&advanced.gimStationMode!=='ignore')&&String(advanced.gimProviders||'').trim())||['systemsInclude','systemsExclude','receiverInclude','receiverExclude','antennaInclude','antennaExclude','signalCodesInclude','signalCodesExclude'].some(key=>String(advanced[key]||'').trim());
    const dateAdvancedActive=['kpMode','dstMode','f107Mode'].some(key=>dateAdvanced[key]&&dateAdvanced[key]!=='ignore');
    const dateAdvancedCount=['kpMode','dstMode','f107Mode'].filter(key=>dateAdvanced[key]&&dateAdvanced[key]!=='ignore').length;
    const advancedCount=[
      advanced.completenessMode&&advanced.completenessMode!=='ignore',
      Boolean(advanced.sampleInterval),
      Number(advanced.rinexVersion)>0,
      advanced.referenceFrame&&advanced.referenceFrame!=='ignore',
      ...qualityModes.map(key=>advanced[key]&&advanced[key]!=='ignore'),
      ...['systemsInclude','systemsExclude','receiverInclude','receiverExclude','antennaInclude','antennaExclude','signalCodesInclude','signalCodesExclude'].map(key=>String(advanced[key]||'').trim()),
      ((advanced.gimStationMode&&advanced.gimStationMode!=='ignore')&&String(advanced.gimProviders||'').trim())
    ].filter(Boolean).length;
    setFilterBadge('#stationAdvancedBadge',advancedCount);setFilterBadge('#dateAdvancedBadge',dateAdvancedCount);
    setValue('#startDate', current.startDate || ''); setValue('#endDate', current.endDate || '');
    setValue('#latMin', current.latMin || ''); setValue('#latMax', current.latMax || ''); setValue('#lonMin', current.lonMin || ''); setValue('#lonMax', current.lonMax || '');
    const hasBounds = [current.latMin,current.latMax,current.lonMin,current.lonMax].some(value => String(value || '').trim());
    const countryScope = state.selectedCountries.length ? (state.selectedCountries.length<=3?state.selectedCountries.join('、'):`${state.selectedCountries.length} 个国家/地区`) : '';
    const inScope = hasBounds || Boolean(countryScope);
    const scopeText = hasBounds ? '自定义经纬度范围' : countryScope || '全球范围';
    setText('#scopeHint', inScope ? `已应用${scopeText}` : '默认全球范围'); setText('#mapScope', scopeText);
    setText('#selectedBadge', `${current.selected || 0} 已选择`);
    $('#addObservationPlan').disabled = Number(current.selected || 0) <= 0;
    setText('#mapSelectedCount', current.selected || 0);
    const rowOffset = Number(current.rowOffset || 0), rowTotal = Number(current.rowTotal ?? current.visible ?? 0);
    const rowEnd = Math.min(rowTotal, rowOffset + state.observationRows.length);
    setText('#resultSummary', current.stations
      ? uiPrefs.language === 'en'
        ? state.rowsSelectedOnly ? `${rowTotal} selected stations` : `${current.visible || 0} stations match`
        : state.rowsSelectedOnly ? `${rowTotal} 个已选测站` : `${current.visible || 0} 个测站满足条件`
      : '尚未查询');
    setText('#rowRange', rowTotal ? `${rowOffset + 1}–${rowEnd} / ${rowTotal}` : '0–0');
    $('#previousRows').disabled = rowOffset <= 0; $('#nextRows').disabled = rowEnd >= rowTotal;
    setText('#mapSubtitle', current.stations ? `${current.visible || 0} 个测站满足当前条件` : '查询后显示满足条件的测站');
    renderObservationRows(); if (mapChanged || mapSelectionChanged) renderMapPoints();
    if (state.detailStation) {
      const detail = findObservation(state.detailStation);
      if (detail) {
        if(current.stationRevision!==state.stationDetailRevision) {
          state.stationDetailRevision=current.stationRevision;
          $('#stationDetailBody').innerHTML=stationDetailMarkup(detail);requestStationDetail();
        }
        $('#detailToggleSelection').textContent = detail.selected ? '取消选择' : '选择测站';
      } else closeStationDetail();
    }
  }

  function syncPrecise(current) {
    const previous = state.preciseRows;
    const changed=state.preciseDetailRevision!==current.preciseRevision;
    state.preciseDetailRevision=current.preciseRevision;
    state.preciseRows = Array.isArray(current.preciseProducts) ? current.preciseProducts : [];
    if (previous.length !== state.preciseRows.length
        || (previous.length && state.preciseRows.length
          && (previous[0].id !== state.preciseRows[0].id
            || previous[previous.length - 1].id !== state.preciseRows[state.preciseRows.length - 1].id))) {
      state.precisePage = 0;
    }
    setValue('#preciseStartDate', current.preciseStartDate || ''); setValue('#preciseEndDate', current.preciseEndDate || ''); setValue('#preciseSearch', current.preciseSearch || '');
    if (current.preciseFamily !== undefined) setValue('#productFamily', familyValues[Number(current.preciseFamily)] || 'all');
    updateProductLevelOptions();setSelectedIndex('#preciseLatency', current.preciseLatency); setSelectedIndex('#preciseCenter', current.preciseCenter); setValue('#preciseCompletenessMode',current.preciseCompletenessMode||'ignore');setValue('#preciseCompletenessValue',current.preciseCompletenessValue??0);updatePreciseCompletenessInput();if(!$('#quickPreset').value.startsWith('custom:'))setSelectedIndex('#quickPreset', current.preciseQuickPreset);
    setText('#preciseSelectedBadge', `${current.preciseSelected || 0} 已选择`);
    $('#addPrecisePlan').disabled = !state.preciseRows.some(product => product.selected && !product.localInventoryOnly && Number(product.availableDays) > 0);
    setText('#preciseResultSummary', `${state.preciseRows.length} 个产品`);
    renderPreciseRows();
    if(state.detailProduct && (changed||!state.preciseRows.some(p=>p.id===state.detailProduct&&p.family===state.detailProductFamily))) {
      const currentProduct=state.preciseRows.find(product=>product.id===state.detailProduct&&product.family===state.detailProductFamily);
      if(currentProduct) showProductDetail(currentProduct);
      else {
        state.detailProduct=null;state.detailProductFamily=null;state.productCoverage=null;state.productCoverageRequest='';
        $('#productDetail').innerHTML=`<div class="detail-empty"><b>${esc(localizeUiText('尚未选择产品'))}</b></div>`;
      }
    }
  }

  function downloadPlanQueue(mode = state.mode) {
    const key = mode === 'precise' ? 'preciseQueue' : 'observationQueue';
    return Array.isArray(state.latest[key]) ? state.latest[key] : [];
  }

  function closeDownloadPlan() {
    const layer = $('#downloadPlanLayer');
    if (layer.classList.contains('hidden')) return;
    layer.classList.add('hidden');
    layer.setAttribute('aria-hidden', 'true');
    $('#openDownloadPlan').setAttribute('aria-expanded', 'false');
    $('#openDownloadPlan').focus();
  }

  function openDownloadPlan() {
    closeSettings();
    closeRowMenu();
    const layer = $('#downloadPlanLayer');
    layer.classList.remove('hidden');
    layer.setAttribute('aria-hidden', 'false');
    $('#openDownloadPlan').setAttribute('aria-expanded', 'true');
    state.planSignature = null;
    renderDownloadPlan();
    layer.querySelector('[data-close-download-plan].icon-btn').focus();
  }

  function renderDownloadPlan() {
    const queue = downloadPlanQueue();
    const pending = queue.filter(item => item.status === 'pending');
    const selected = queue.filter(item => item.selected);
    const running = queue.filter(item => item.status === 'running');
    const itemWord = uiPrefs.language === 'en' ? 'items' : '项';
    setText('#downloadPlanTitle', state.mode === 'precise' ? '精密产品下载计划' : '观测数据下载计划');
    setText('#downloadPlanSubtitle', state.mode === 'precise' ? '查询时可继续向计划中添加产品' : '查询时可继续向计划中添加测站');
    setText('#downloadPlanCount', `${queue.length} ${itemWord}`);
    setText('#downloadPlanHint', uiPrefs.language === 'en'
      ? `${selected.length} selected · ${pending.length} pending · ${running.length} running`
      : `已选 ${selected.length} 项 · 待下载 ${pending.length} 项 · 进行中 ${running.length} 项`);
    const all = $('#downloadPlanSelectAll');
    all.disabled = queue.length === 0;
    all.checked = queue.length > 0 && selected.length === queue.length;
    all.indeterminate = selected.length > 0 && selected.length < queue.length;
    $('#downloadPlanRemove').disabled = selected.length === 0;
    const layer = $('#downloadPlanLayer');
    if (layer.classList.contains('hidden')) return;

    // 大批测站只绘制当前需要查看的部分，避免每次进度更新重建上千行。
    const signature = `${state.mode}:${state.planVisible[state.mode]}:${queue.map(item => `${item.id}:${item.status}:${item.selected}`).join('|')}`;
    const root = $('#downloadPlanList');
    if (signature !== state.planSignature) {
      const rank = item => item.status === 'running' ? 0 : item.status === 'pending'
        ? (item.selected ? 1 : 2) : item.status === 'failed' ? 3 : item.status === 'cancelled' ? 4 : 5;
      const visible = [...queue].sort((a, b) => rank(a) - rank(b)).slice(0, state.planVisible[state.mode]);
      const scrollTop = root.scrollTop;
      state.planSignature = signature;
      const statusText = { pending: '待下载', running: '下载中', done: '已完成', failed: '失败', cancelled: '已取消' };
      root.innerHTML = visible.length ? visible.map(item => {
        const percent = clamp(Number(item.percent) || 0, 0, 100);
        const range = item.range ? `${esc(item.range)} · ` : '';
        return `<div class="download-plan-item" data-plan-id="${esc(item.id)}" data-status="${esc(item.status)}">
          <input type="checkbox" data-plan-select ${item.selected ? 'checked' : ''} aria-label="${esc(localizeUiText('选择'))} ${esc(item.label)}">
          <div class="download-plan-item-main"><b title="${esc(item.label)}">${esc(item.label)}</b>
          <small>${range}<span data-plan-status>${esc(localizeUiText(statusText[item.status] || item.status))}</span> · <span data-plan-percent>${percent}%</span></small>
          <div class="download-plan-item-progress"><i style="width:${percent}%"></i></div></div>
          <div class="download-plan-item-actions">
          <button type="button" data-plan-action="remove">${esc(localizeUiText('移除'))}</button></div></div>`;
      }).join('') : `<div class="download-plan-empty">${esc(localizeUiText('从查询结果中选择内容，然后点击“添加下载计划”'))}</div>`;
      if (visible.length < queue.length) {
        root.insertAdjacentHTML('beforeend', `<button class="btn soft download-plan-more" type="button" id="downloadPlanMore">${esc(localizeUiText('显示更多'))} (${queue.length - visible.length})</button>`);
      }
      root.scrollTop = scrollTop;
    } else {
      const rows = new Map([...root.querySelectorAll('[data-plan-id]')].map(node => [node.dataset.planId, node]));
      for (const item of queue) {
        const row = rows.get(String(item.id));
        if (!row) continue;
        const percent = clamp(Number(item.percent) || 0, 0, 100);
        const label = row.querySelector('[data-plan-percent]');
        const bar = row.querySelector('.download-plan-item-progress i');
        if (label) label.textContent = `${percent}%`;
        if (bar) bar.style.width = `${percent}%`;
      }
    }
  }

  function updateDownloadDock() {
    const current = state.latest;
    const observationRunning = Boolean(current.downloading);
    const preciseRunning = Boolean(current.preciseDownloading);
    for (const [mode, running, percent] of [
      ['observation', observationRunning, state.progress],
      ['precise', preciseRunning, Number(current.preciseProgress || 0)]
    ]) {
      const tab = $(`.mode-tab[data-mode="${mode}"]`);
      if (!tab) continue;
      tab.classList.toggle('running', running);
      tab.dataset.progress = running ? `${clamp(percent, 0, 100)}%` : '';
    }
    const precise = state.mode === 'precise';
    const processing = Boolean(current.postprocessing);
    const busy = precise ? Boolean(current.preciseDownloading) : Boolean(current.downloading || processing);
    const queueRunning = precise ? Boolean(current.preciseQueueRunning) : Boolean(current.observationQueueRunning);
    const paused = precise ? Boolean(current.precisePaused) : Boolean(current.paused);
    const queue = downloadPlanQueue();
    const selected = queue.filter(item => item.selected && item.status !== 'running').length;
    const running = queue.filter(item => item.status === 'running');
    const active = running[0];
    const folder = precise ? current.preciseFolder : current.folder;
    const status = precise ? current.preciseStatus : current.status;
    const percent = precise ? Number(current.preciseProgress || 0) : state.progress;
    // Keep the dock summary short; the plan and tooltip show the full item ID.
    const selectionText = active
      ? (uiPrefs.language === 'en' ? `${paused ? 'Paused' : 'Downloading'} · ${running.length} active${selected ? ` · ${selected} waiting` : ''}` : `${paused ? '已暂停' : '下载中'} · ${running.length} 项进行中${selected ? ` · ${selected} 项等待` : ''}`)
      : selected ? `已选 ${selected} ${precise ? '个产品' : '个测站'}` : '下载计划为空';
    setText('#downloadSelection', selectionText);
    const badge = $('#downloadPlanBadge');
    const outstanding = queue.filter(item => item.status === 'pending' || item.status === 'running').length;
    badge.textContent = outstanding > 99 ? '99+' : String(outstanding);
    badge.classList.toggle('hidden', outstanding === 0);
    setText('#downloadFolder', folder || '尚未设置保存目录'); $('#downloadFolder').title = folder || '';
    setText('#downloadStatus', status || (busy ? '正在处理任务' : '等待任务'));
    $('#downloadStatus').title = localizeUiText(status || (busy ? '正在处理任务' : '等待任务'));
    $('#downloadSelection').title = active ? `${active.label} · ${localizeUiText(selectionText)}` : localizeUiText(selectionText);
    setText('#downloadPercent', `${clamp(percent,0,100)}%`); $('#downloadProgress').style.width = `${clamp(percent,0,100)}%`;
    $('#browseFolder').disabled = false; $('#openDownloadFolder').disabled = !folder;
    $('#startDownload').disabled = busy || processing || queueRunning || selected <= 0 || (precise && current.preciseBusy);
    $('#downloadPlanStart').disabled = $('#startDownload').disabled;
    $('#pauseDownload').disabled = precise ? !busy : !current.canPause; $('#cancelDownload').disabled = !busy;
    setText('#pauseDownload', paused ? '继续' : '暂停'); setText('#startDownload', processing && !precise ? '正在后处理' : busy || queueRunning ? '下载进行中' : '开始下载');
    setText('#downloadPlanStart', busy || queueRunning ? '下载进行中' : '开始下载');
    setText('#taskSummarySelection', selectionText);
    setText('#taskSummaryStatus', status || (busy ? '正在处理任务' : '等待任务'));
    setText('#taskSummaryPercent', `${clamp(percent,0,100)}%`);
    const summaryBar = $('#taskSummaryProgress'); if (summaryBar) summaryBar.style.width = `${clamp(percent,0,100)}%`;
    // 查询测站始终保持主按钮样式，选中测站和下载进度不改变颜色。
    const queryButton = $('#queryStations');
    if (queryButton) {
      queryButton.classList.add('primary');
      queryButton.classList.remove('soft');
    }
  }

  function handleState(current) {
    if (current.downloading && !state.latest.downloading) state.progress = 0;
    state.latest = current;
    refreshCustomRuleOptions();
    const downloadSettings = current.downloadSettings || { layout: 'date', postprocess: 'off' };
    $('#downloadStorageLayout').value = downloadSettings.layout;
    $('#askObservationPostprocess').checked = downloadSettings.postprocess === 'ask';
    const settingsBusy = Boolean(current.downloading || current.observationQueueRunning || current.preciseBusy || current.postprocessing);
    $('#downloadStorageLayout').disabled = settingsBusy;
    $('#askObservationPostprocess').disabled = settingsBusy;
    setText('#downloadSettingsHint', settingsBusy ? '任务进行中，结束后可更改设置。' : '设置对后续开始的下载任务生效。');
    window.MarkSelection.onState(current);
    syncObservation(current); syncPrecise(current); renderDownloadPlan(); updateDownloadDock(); renderActiveFilters(current);
    const indexText=current.availableSources>0?'本地索引已就绪':'等待本地索引';setText('.index-state span',indexText);$('.index-state').classList.toggle('waiting',current.availableSources<=0);
    // Apply intermediate filter results; log the query summary once.
    const queryReady = !current.querying && !current.stationSelection?.metadataLoading;
    if(queryReady&&current.querySummary&&current.querySummary!==lastQuerySummary){lastQuerySummary=current.querySummary;addInfo(current.querySummary,'notice');}
    const status = state.mode === 'precise' ? current.preciseStatus : current.status;
    const queryCompletion = /^查询完成[：:]/.test(status || '');
    if (status && status !== lastStatus && (!queryCompletion || queryReady)) {
      lastStatus = status;
      const warning = /失败|错误|尚未|没有|请先|warning|error/i.test(status);
      const download = current.downloading || current.preciseBusy || /下载/.test(status);
      if (status !== current.querySummary) {
        addInfo(status, download ? 'download' : warning ? 'warning' : 'notice', { busy:current.downloading || current.preciseDownloading });
      }
    }
    scheduleFirstFrameReady();
  }

  function handleProgress(message) {
    state.progress = clamp(Number(message.percent) || 0,0,100);
    const active = String(state.latest.activeObservationQueueId || '');
    const item = downloadPlanQueue('observation').find(entry => String(entry.id) === active);
    if (item && item.status === 'running') {
      item.percent = state.progress;
      if (state.mode === 'observation') renderDownloadPlan();
    }
    if (message.message) {
      state.latest.status = message.message;
      if (state.mode === 'observation' && /完成|失败|已存在|已取消/.test(message.message)) {
        addInfo(message.message, 'download', { progress:true, busy:state.latest.downloading, stream:'observation' });
      }
    }
    updateDownloadDock();
  }

  const project = (lon, lat) => [(Number(lon) + 180) / 360 * 1000, (90 - Number(lat)) / 180 * 500];
  const unproject = (x, y) => [x / 1000 * 360 - 180, 90 - y / 500 * 180];
  function pathFromLine(line) { return line.map((coord, index) => { const [x,y] = project(coord[0],coord[1]); return `${index ? 'L' : 'M'}${x.toFixed(2)},${y.toFixed(2)}`; }).join(''); }
  function pathFromRing(ring) { return pathFromLine(ring) + 'Z'; }
  function geometryPath(geometry) {
    if (!geometry) return '';
    if (geometry.type === 'GeometryCollection') return (geometry.geometries || []).map(geometryPath).join('');
    if (geometry.type === 'Polygon') return geometry.coordinates.map(pathFromRing).join('');
    if (geometry.type === 'MultiPolygon') return geometry.coordinates.flatMap(polygon => polygon.map(pathFromRing)).join('');
    // Border and coastline paths stay open; closing them invents extra edges.
    if (geometry.type === 'LineString') return pathFromLine(geometry.coordinates);
    if (geometry.type === 'MultiLineString') return geometry.coordinates.map(pathFromLine).join('');
    return '';
  }
  function drawGraticule() {
    const lines = [];
    for (let lon = -175; lon <= 175; lon += 5) {
      const [x] = project(lon,0), major = lon % 30 === 0, subminor = lon % 10 !== 0;
      lines.push(`<path class="graticule ${major ? 'major' : subminor ? 'subminor' : ''}" d="M${x},0V500"/>`);
    }
    for (let lat = -85; lat <= 85; lat += 5) {
      const [,y] = project(0,lat), major = lat % 30 === 0, subminor = lat % 10 !== 0;
      lines.push(`<path class="graticule ${major ? 'major' : subminor ? 'subminor' : ''}" d="M0,${y}H1000"/>`);
    }
    const references = [
      [66.5634,'polar'], [23.4366,'tropic'], [0,'equator'],
      [-23.4366,'tropic'], [-66.5634,'polar']
    ];
    for (const [lat,kind] of references) {
      const [,y] = project(0,lat);
      lines.push(`<path class="geo-reference ${kind}" d="M0,${y.toFixed(3)}H1000"/>`);
    }
    lines.push('<rect class="map-neatline" x="0" y="0" width="1000" height="500"/>');
    $('#graticuleLayer').innerHTML = lines.join('');
  }
async function loadMap() {
    drawGraticule();
    $('#landLayer').innerHTML = '<rect class="map-water" x="0" y="0" width="1000" height="500"/>';
    try {
      const response = await fetch('../ne_50m_admin_0_countries.geojson', {cache:'no-store'});
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const geo = await response.json();
      if (geo.MARK_RENDERING === 'reference-lines') {
        const render = (feature, className) => `<path class="${className}" d="${geometryPath(feature.geometry)}"></path>`;
        const land = geo.features.filter(feature => feature.properties?.MARK_KIND === 'land');
        const coast = geo.features.filter(feature => feature.properties?.MARK_KIND === 'coastline');
        const borders = geo.features.filter(feature => feature.properties?.MARK_KIND === 'boundary');
        $('#landLayer').innerHTML += land.map(feature => render(feature, 'map-reference-land')).join('');
        $('#countryLayer').innerHTML = coast.map(feature => render(feature, 'map-reference-coast')).join('') + borders.map(feature => {
          const type = ['international','undetermined','control'].includes(feature.properties.MARK_BOUNDARY_TYPE) ? feature.properties.MARK_BOUNDARY_TYPE : 'undetermined';
          return `<path class="map-reference-border ${type}" data-boundary-type="${type}" data-source-id="${esc(feature.properties.NE_ID)}" d="${geometryPath(feature.geometry)}"></path>`;
        }).join('');
      } else {
      const countryPath=feature=>`<path class="country-shape ${feature.properties?.MARK_KIND==='indicator'?'reference-indicator':''} ${feature.properties?.MARK_STATUS==='disputed'?'disputed-area':''}" data-country-code="${esc(feature.properties?.MARK_COUNTRY||'')}" d="${geometryPath(feature.geometry)}"></path>`;
      const countryFeatures=geo.features.filter(feature=>!['region','island'].includes(feature.properties?.MARK_KIND));
      const countries=countryFeatures.filter(feature=>feature.properties?.MARK_COUNTRY!=='CHN').map(countryPath).join('');
      const disputed=geo.features.filter(feature=>feature.properties?.MARK_STATUS==='disputed').map(feature=>{
        const d=geometryPath(feature.geometry);
        return `<path class="disputed-boundary-halo" d="${d}"></path><path class="disputed-boundary" d="${d}"></path>`;
      }).join('');
      // Draw this outline after the background strokes.
      const china=countryFeatures.filter(feature=>feature.properties?.MARK_COUNTRY==='CHN').map(countryPath).join('');
      $('#countryLayer').innerHTML = countries+disputed+china;
      }
      const countryDisplayName = (properties, language) => {
        if(['indicator','land','coastline','boundary'].includes(properties.MARK_KIND))return '';
        const prescribed=properties[language==='en'?'MARK_NAME_EN':'MARK_NAME_ZH'];
        if(prescribed)return prescribed;
        const area=window.MarkCountries.areaCode(properties.ISO_A3&&properties.ISO_A3!=='-99'?properties.ISO_A3:properties.ADM0_A3);
        if(window.MarkCountries.areas[area])return window.MarkCountries.displayName(area,language);
        if(area==='PSE')return language==='en'?'Palestine':'巴勒斯坦';
        return language==='en'?(properties.NAME_EN||properties.NAME||''):(properties.NAME_ZH||properties.NAME_EN||properties.NAME||'');
      };
      state.countryLabels = geo.features.map(feature => {
    const properties = feature.properties || {};
    return {
      nameZh: countryDisplayName(properties, 'zh'),
      nameEn: countryDisplayName(properties, 'en'),
          lon: Number(properties.LABEL_X), lat: Number(properties.LABEL_Y),
          rank: Number(properties.LABELRANK) || 9,
          island: properties.MARK_KIND==='island',
          population: Number(properties.POP_EST) || 0,
          minimumZoom: Number(properties.MIN_LABEL) || 1
        };
      }).filter(label => (label.nameZh || label.nameEn) && Number.isFinite(label.lon) && Number.isFinite(label.lat));
      scheduleMapDraw();
    } catch (_) {
      try {
        const response = await fetch('../ne_110m_land.geojson', {cache:'no-store'});
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const geo = await response.json();
        const land = geo.features.map(feature => `<path class="map-reference-land" d="${geometryPath(feature.geometry)}"></path>`).join('');
        $('#countryLayer').innerHTML = land;
      } catch (_) {
        $('#countryLayer').innerHTML = '<path class="land-shape" d="M65 180L180 90 310 120 330 210 265 260 240 390 190 315 135 285ZM450 115L555 70 650 105 720 95 900 145 920 245 830 285 735 230 665 300 620 425 555 365 530 250 465 215ZM805 350L900 330 950 390 895 445 820 415Z"></path>';
      }
    }
  }

  function renderMapPoints() {
    const points = state.mapView === 'selected' ? state.mapPoints.filter(point => point[3]) : state.mapPoints;
    state.renderedMapPoints = points.map(point => { const [x,y] = project(point[2],point[1]); return { raw:point, x, y, selected:Boolean(point[3]) }; });
    if (state.focusedMapStation && !state.renderedMapPoints.some(point => point.raw[0] === state.focusedMapStation)) state.focusedMapStation = '';
    if (state.mapHoverStation && !state.renderedMapPoints.some(point => point.raw[0] === state.mapHoverStation)) hideMapStationTooltip();
    setText('#mapCount', points.length); $('#mapEmpty').classList.toggle('hidden', points.length > 0);
    applyMapTransform();
  }
  function scheduleMapDraw() {
    if (mapDrawFrame) return;
    mapDrawFrame = requestAnimationFrame(() => { mapDrawFrame = 0; drawMapPoints(); });
  }
  function mapFrame(rect) {
    const fit = Math.max(.001, Math.min(rect.width / 1000, rect.height / 500));
    return { fit, left:(rect.width - 1000 * fit) / 2, top:(rect.height - 500 * fit) / 2 };
  }
  function mapScreenPoint(point, frame) {
    const map = state.map;
    return [
      frame.left + (map.x + point.x * map.scale) * frame.fit,
      frame.top + (map.y + point.y * map.scale) * frame.fit
    ];
  }
  function shortenedCountryName(name) {
    const aliases = {
      '中华人民共和国':'中国', '大韩民国':'韩国', '朝鲜民主主义人民共和国':'朝鲜',
      '刚果民主共和国':'刚果（金）', '中非共和国':'中非',
      '阿拉伯联合酋长国':'阿联酋', '多民族玻利维亚国':'玻利维亚',
      'Democratic Republic of the Congo':'DR Congo',
      'Democratic Republic of Congo':'DR Congo',
      'Central African Republic':'Central Africa',
      'United Arab Emirates':'UAE',
      'Plurinational State of Bolivia':'Bolivia'
    };
    return aliases[name] || name;
  }
  function drawCountryLabels(context, frame, width, height) {
    if (!state.countryLabels.length) return;
    const scale = state.map.scale;
    const candidates = state.countryLabels.filter(label => {
      if(label.island)return scale>=label.minimumZoom;
      if (scale < 1.8) return label.rank <= 2 && label.population >= 20000000;
      if (scale < 3.2) return label.rank <= 3 && label.population >= 2500000;
      return scale + .8 >= label.minimumZoom;
    }).sort((left,right) => left.rank - right.rank || right.population - left.population);
    const occupied = [];
    const fontSize = scale >= 8 ? 15.5 : scale >= 2.5 ? 14 : 13;
    context.font = `650 ${fontSize}px "Segoe UI","Microsoft YaHei UI",sans-serif`;
    context.textAlign = 'center'; context.textBaseline = 'middle';
    for (const label of candidates) {
      const [worldX,worldY] = project(label.lon,label.lat);
      const [x,y] = mapScreenPoint({x:worldX,y:worldY},frame);
      if (x < frame.left + 12 || x > frame.left + 1000 * frame.fit - 12 || y < frame.top + 10 || y > frame.top + 500 * frame.fit - 10 || x < 4 || x > width - 4 || y < 4 || y > height - 4) continue;
      if(label.island){
        context.beginPath();context.arc(x,y,2.3,0,Math.PI*2);
        context.fillStyle=isDarkUi()?'#b7cfe5':'#627f94';context.fill();
      }
      const text = shortenedCountryName(uiPrefs.language === 'en' ? label.nameEn : label.nameZh);
      const textWidth = context.measureText(text).width;
      const box = {left:x-textWidth/2-3,right:x+textWidth/2+3,top:y-fontSize/2-2,bottom:y+fontSize/2+2};
      if (occupied.some(item => !(box.right < item.left || box.left > item.right || box.bottom < item.top || box.top > item.bottom))) continue;
      occupied.push(box);
      context.lineWidth = isDarkUi() ? 2.1 : 2.0;
      context.strokeStyle = isDarkUi() ? 'rgba(8,14,22,.92)' : 'rgba(250,253,255,.96)';
      context.strokeText(text,x,y);
      context.fillStyle = isDarkUi() ? 'rgba(165,186,210,.88)' : 'rgba(43,62,84,.92)';
      context.fillText(text,x,y);
    }
  }
  function angularLabel(value, positive, negative) {
    if (value === 0) return '0°';
    return `${Math.abs(value)}°${value > 0 ? positive : negative}`;
  }
  function drawMapAxes(context, frame, width, height) {
    const map = state.map, scale = map.scale;
    const longitudeStep = scale >= 12 ? 5 : scale >= 6 ? 10 : scale >= 3 ? 15 : 30;
    const worldBottom = frame.top + (map.y + 500 * scale) * frame.fit;
    const labelY = clamp(worldBottom - 6, frame.top + 12, Math.min(height - 5, frame.top + 500 * frame.fit - 5));
    context.font = '650 12px "Segoe UI","Microsoft YaHei UI",sans-serif';
    context.textAlign = 'center'; context.textBaseline = 'bottom';
    // Keep the coordinate labels legible without the heavy white halo used by
    // the previous map renderer.
    context.lineWidth = 2.0; context.strokeStyle = isDarkUi() ? 'rgba(14,19,26,.92)' : 'rgba(250,253,255,.98)'; context.fillStyle = isDarkUi() ? '#93a6bd' : '#506b82';
    for (let lon = -180; lon <= 180; lon += longitudeStep) {
      const [worldX] = project(lon,0);
      const [x] = mapScreenPoint({x:worldX,y:250},frame);
      if (x < Math.max(24,frame.left+20) || x > Math.min(width-24,frame.left+1000*frame.fit-20)) continue;
      const text = angularLabel(lon,'E','W'); context.strokeText(text,x,labelY); context.fillText(text,x,labelY);
    }
    context.textAlign = 'left'; context.textBaseline = 'middle';
    const latitudeX = Math.max(5,frame.left + 5);
    for (let lat = -60; lat <= 60; lat += 30) {
      const [,worldY] = project(0,lat);
      const [,y] = mapScreenPoint({x:0,y:worldY},frame);
      if (y < frame.top+10 || y > Math.min(height-10,frame.top+500*frame.fit-10)) continue;
      const text = angularLabel(lat,'N','S'); context.strokeText(text,latitudeX,y); context.fillText(text,latitudeX,y);
    }
    const references = [
      [66.5634, uiPrefs.language === 'en' ? 'Arctic Circle 66°34′N' : '北极圈 66°34′N'],
      [23.4366, uiPrefs.language === 'en' ? 'Tropic of Cancer 23°26′N' : '北回归线 23°26′N'],
      [0, uiPrefs.language === 'en' ? 'Equator 0°' : '赤道 0°'],
      [-23.4366, uiPrefs.language === 'en' ? 'Tropic of Capricorn 23°26′S' : '南回归线 23°26′S'],
      [-66.5634, uiPrefs.language === 'en' ? 'Antarctic Circle 66°34′S' : '南极圈 66°34′S']
    ];
    context.textAlign = 'right'; context.font = '650 12px "Segoe UI","Microsoft YaHei UI",sans-serif';
    const referenceX = Math.min(width-5,frame.left + 1000 * frame.fit - 5);
    for (const [lat,text] of references) {
      const [,worldY] = project(0,lat), [,y] = mapScreenPoint({x:1000,y:worldY},frame);
      if (y < frame.top+10 || y > Math.min(height-10,frame.top+500*frame.fit-10)) continue;
      context.strokeText(text,referenceX,y); context.fillText(text,referenceX,y);
    }
  }
  function drawMapPoints() {
    const canvas = $('#stationCanvas'), viewport = $('#mapViewport');
    if (!canvas || !viewport) return;
    const rect = viewport.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return;
    const ratio = Math.min(window.devicePixelRatio || 1, 2);
    const width = Math.max(1, Math.round(rect.width * ratio));
    const height = Math.max(1, Math.round(rect.height * ratio));
    if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
    const context = canvas.getContext('2d', { alpha:true });
    context.setTransform(ratio,0,0,ratio,0,0);
    context.clearRect(0,0,rect.width,rect.height);
    const frame = mapFrame(rect);
    drawCountryLabels(context,frame,rect.width,rect.height);
    const paint = (selected, fill, radius, opacity) => {
      context.beginPath();
      for (const point of state.renderedMapPoints) {
        const displayedSelected=window.MarkSelection.dayView ? window.MarkSelection.isAvailable(point.raw) : point.selected;
        if (displayedSelected !== selected || point.raw[0] === state.focusedMapStation) continue;
        const [x,y] = mapScreenPoint(point,frame);
        if (x < -radius || y < -radius || x > rect.width + radius || y > rect.height + radius) continue;
        context.moveTo(x + radius,y); context.arc(x,y,radius,0,Math.PI*2);
      }
      context.globalAlpha = opacity; context.fillStyle = fill; context.fill(); context.globalAlpha = 1;
    };
    // Solid fills avoid the white halos that accumulated where thousands of
    // SVG/canvas outlines overlapped in dense national station networks.
    const mapScale = state.map.scale;
    const zoomSteps = Math.min(5,Math.log2(Math.max(1,mapScale)));
    const ordinaryRadius = 1.85 * (1 + zoomSteps * .14);
    const selectedRadius = 3.35 * (1 + zoomSteps * .10);
    const mapPalette = { normal:'#149b9b', chosen:'#ef715a', focused:'#169c9b', focusedChosen:'#f1785f' };
    paint(false,window.MarkSelection.dayView?'#9aaabd':mapPalette.normal,ordinaryRadius,window.MarkSelection.dayView ? .36 : .92);
    paint(true,mapPalette.chosen,selectedRadius,1);
    if(window.MarkSelection.dayView) {
      context.beginPath();
      for(const point of state.renderedMapPoints) if(window.MarkSelection.isUnknown(point.raw)) {
        const [x,y]=mapScreenPoint(point,frame);context.moveTo(x+selectedRadius,y);context.arc(x,y,selectedRadius,0,Math.PI*2);
      }
      context.fillStyle='#d99a39';context.fill();
    }
    const focused = state.renderedMapPoints.find(point => point.raw[0] === state.focusedMapStation);
    if (focused) {
      const [x,y] = mapScreenPoint(focused,frame);
      context.beginPath(); context.arc(x,y,Math.max(6,selectedRadius+2.5),0,Math.PI*2);
      context.fillStyle = focused.selected ? mapPalette.focusedChosen : mapPalette.focused; context.fill();
      context.strokeStyle = '#fff'; context.lineWidth = 2.2; context.stroke();
    }
    const hovered = state.renderedMapPoints.find(point => point.raw[0] === state.mapHoverStation);
    if (hovered && (!focused || hovered.raw[0] !== focused.raw[0])) {
      const [x,y] = mapScreenPoint(hovered,frame);
      const hoverRadius=(hovered.selected?selectedRadius:ordinaryRadius)+2.7;
      context.beginPath(); context.arc(x,y,hoverRadius,0,Math.PI*2);
      context.strokeStyle = hovered.selected ? mapPalette.chosen : mapPalette.normal; context.lineWidth = 2; context.stroke();
      context.beginPath(); context.arc(x,y,hoverRadius+2.5,0,Math.PI*2);
      context.strokeStyle = 'rgba(255,255,255,.9)'; context.lineWidth = 2; context.stroke();
    }
    drawMapAxes(context,frame,rect.width,rect.height);
  }
  function constrainMap() {
    const map = state.map;
    map.scale = clamp(map.scale,1,32);
    map.x = clamp(map.x,1000-1000*map.scale,0);
    map.y = clamp(map.y,500-500*map.scale,0);
    if (map.scale === 1) { map.x = 0; map.y = 0; }
  }
  function applyMapTransform() { const map = state.map; constrainMap(); $('#mapTransform').setAttribute('transform', `translate(${map.x} ${map.y}) scale(${map.scale})`); const zoom = map.scale < 10 ? `${map.scale.toFixed(1).replace('.0','')}×` : `${Math.round(map.scale)}×`; const label = $('#mapZoomLabel'); if (label && label.textContent !== zoom) label.textContent = zoom; scheduleMapDraw(); }
  function resetMap() { Object.assign(state.map,{scale:1,x:0,y:0,dragging:false,selecting:false,start:null}); $('#selectionRect').style.display = 'none'; $('#mapViewport').classList.remove('rectangle-mode','map-dragging'); $('#mapRectangleMode').classList.remove('active'); applyMapTransform(); }
  function zoomMap(factor, x = 500, y = 250) { const map = state.map, next = clamp(map.scale * factor,1,32), ratio = next / map.scale; map.x = x - (x - map.x) * ratio; map.y = y - (y - map.y) * ratio; map.scale = next; applyMapTransform(); }
  function svgPoint(event, invert = false) { const rect = $('#worldMap').getBoundingClientRect(), frame=mapFrame(rect); let x = (event.clientX - rect.left - frame.left) / frame.fit, y = (event.clientY - rect.top - frame.top) / frame.fit; if (invert) { x = (x - state.map.x) / state.map.scale; y = (y - state.map.y) / state.map.scale; } return {x,y}; }
  function mapPointAtEvent(event) {
    const rect = $('#worldMap').getBoundingClientRect(), frame=mapFrame(rect);
    // A visible marker is only 4–7 px wide, but a larger hit target makes
    // dense station maps usable on high-DPI screens and precision touchpads.
    const hitRadius=state.map.scale>=6?11:state.map.scale>=2?14:18;
    let nearest = null, nearestDistance = hitRadius * hitRadius;
    for (const point of state.renderedMapPoints) {
      const [localX,localY] = mapScreenPoint(point,frame), x=rect.left+localX, y=rect.top+localY;
      const dx = event.clientX - x, dy = event.clientY - y, distance = dx*dx + dy*dy;
      if (distance <= nearestDistance) { nearestDistance = distance; nearest = point; }
    }
    return nearest;
  }

  function hideMapStationTooltip() {
    clearTimeout(mapHoverTimer);mapHoverTimer=0;
    state.mapHoverStation='';state.mapHoverDetail=null;state.mapHoverPosition=null;
    const tooltip=$('#mapStationTooltip');if(tooltip)tooltip.classList.add('hidden');
    const viewport=$('#mapViewport');if(viewport)viewport.classList.remove('station-hover');
    scheduleMapDraw();
  }
  function renderMapStationTooltip() {
    const tooltip=$('#mapStationTooltip'),viewport=$('#mapViewport');
    if(!tooltip||!viewport||!state.mapHoverStation||!state.mapHoverPosition)return;
    const point=state.renderedMapPoints.find(item=>item.raw[0]===state.mapHoverStation);
    if(!point){hideMapStationTooltip();return;}
    const detail=state.mapHoverDetail&&state.mapHoverDetail.station===state.mapHoverStation?state.mapHoverDetail:null;
    const selected=Boolean(point.raw[3]);
    const coordinates=`${Number(point.raw[1]).toFixed(5)}°, ${Number(point.raw[2]).toFixed(5)}°`;
    tooltip.classList.toggle('selected',selected);
    tooltip.innerHTML=`<header><i></i><b>${esc(state.mapHoverStation)}</b><span>${selected?'已选择':'满足条件'}</span></header><div class="map-tooltip-grid"><span>站网</span><b>${esc(detail?.network||'正在读取…')}</b><span>国家/地区</span><b>${esc(detail?stationRecordedCountryName(detail.countryArea||detail.country):'正在读取…')}</b><span>采样率</span><b>${esc(detail?.interval||'—')}</b><span>接收机</span><b title="${esc(detail?.receiver||'—')}">${esc(detail?.receiver||'—')}</b><span>卫星系统</span><b title="${esc(detail?.system||'—')}">${esc(detail?.system||'—')}</b><span>坐标</span><b>${esc(coordinates)}</b></div>`;
    tooltip.classList.remove('hidden');
    const rect=viewport.getBoundingClientRect(),width=tooltip.offsetWidth||246,height=tooltip.offsetHeight||170;
    const left=clamp(state.mapHoverPosition.x-rect.left+14,8,Math.max(8,rect.width-width-8));
    const top=clamp(state.mapHoverPosition.y-rect.top+14,8,Math.max(8,rect.height-height-8));
    tooltip.style.left=`${Math.round(left)}px`;tooltip.style.top=`${Math.round(top)}px`;
  }
  function queueMapStationHover(event) {
    const position={x:event.clientX,y:event.clientY,clientX:event.clientX,clientY:event.clientY};
    const viewport=$('#mapViewport'),immediatePoint=mapPointAtEvent(position);
    if(viewport)viewport.classList.toggle('station-hover',Boolean(immediatePoint));
    clearTimeout(mapHoverTimer);
    mapHoverTimer=setTimeout(()=>{
      mapHoverTimer=0;
      if(state.map.dragging||state.map.selecting)return;
      const point=mapPointAtEvent(position);
      if(!point){hideMapStationTooltip();return;}
      const station=String(point.raw[0]);
      const changed=station!==state.mapHoverStation;
      state.mapHoverStation=station;state.mapHoverPosition=position;
      if(changed){
        state.mapHoverDetail=null;
        const local=findObservation(station);
        if(local)state.mapHoverDetail={station,network:local.network,country:local.country,countryArea:local.area,interval:local.interval,receiver:local.receiver,system:local.system};
        post('map-hover-detail',{station});
      }
      renderMapStationTooltip();scheduleMapDraw();
    },42);
  }
  function handleMapHoverDetail(message) {
    if(!message||!message.found||String(message.station||'')!==state.mapHoverStation)return;
    state.mapHoverDetail=message;renderMapStationTooltip();
  }

  function setupMapInteractions() {
    const svg = $('#worldMap'), viewport = $('#mapViewport'), selectButton = $('#mapRectangleMode');
    svg.addEventListener('wheel', event => { event.preventDefault(); const point = svgPoint(event); zoomMap(event.deltaY < 0 ? 1.18 : .84, point.x, point.y); }, { passive:false });
    svg.addEventListener('dblclick', event => { event.preventDefault(); resetMap(); });
    svg.addEventListener('pointerdown', event => {
      if (event.button !== 0) return; hideMapStationTooltip();pointerMoved = false; svg.setPointerCapture(event.pointerId);
      if (viewport.classList.contains('rectangle-mode')) { state.map.selecting = true; state.map.start = svgPoint(event,true); const box = $('#selectionRect'); box.style.display='block'; box.setAttribute('x',state.map.start.x); box.setAttribute('y',state.map.start.y); box.setAttribute('width',0); box.setAttribute('height',0); }
      else { state.map.dragging = true; state.map.start = { clientX:event.clientX,clientY:event.clientY,x:state.map.x,y:state.map.y }; viewport.classList.add('map-dragging'); }
    });
    svg.addEventListener('pointermove', event => {
      if (state.map.selecting) { const now=svgPoint(event,true), start=state.map.start, box=$('#selectionRect'); box.setAttribute('x',Math.min(start.x,now.x)); box.setAttribute('y',Math.min(start.y,now.y)); box.setAttribute('width',Math.abs(now.x-start.x)); box.setAttribute('height',Math.abs(now.y-start.y)); pointerMoved=true; }
      else if (state.map.dragging) { const rect=svg.getBoundingClientRect(), frame=mapFrame(rect), start=state.map.start; state.map.x=start.x+(event.clientX-start.clientX)/frame.fit; state.map.y=start.y+(event.clientY-start.clientY)/frame.fit; pointerMoved=true; applyMapTransform(); }
      else queueMapStationHover(event);
    });
    // Some WebView2 and precision-touchpad input paths emit the compatibility
    // mouse event without a pointermove.  Listening to both keeps hover cards
    // reliable; the short debounce coalesces the normal paired event stream.
    svg.addEventListener('mousemove',event=>{if(!state.map.dragging&&!state.map.selecting)queueMapStationHover(event);});
    svg.addEventListener('pointerup', event => {
      const clickedPoint = !state.map.selecting && !pointerMoved ? mapPointAtEvent(event) : null;
      if (state.map.selecting) { const now=svgPoint(event,true), start=state.map.start; state.map.selecting=false; $('#selectionRect').style.display='none'; viewport.classList.remove('rectangle-mode'); selectButton.classList.remove('active'); if (Math.abs(now.x-start.x)>3 && Math.abs(now.y-start.y)>3) { const [lon1,lat1]=unproject(start.x,start.y), [lon2,lat2]=unproject(now.x,now.y); const payload={latMin:Math.min(lat1,lat2).toFixed(4),latMax:Math.max(lat1,lat2).toFixed(4),lonMin:Math.min(lon1,lon2).toFixed(4),lonMax:Math.max(lon1,lon2).toFixed(4)}; Object.entries(payload).forEach(([key,value])=>setValue(`#${key}`,value)); post('apply-bounds',payload); addInfo('已从地图框选空间范围。','notice'); } }
      state.map.dragging=false; state.map.start=null; viewport.classList.remove('map-dragging');
      if (clickedPoint) post('select-station',{station:clickedPoint.raw[0],checked:!clickedPoint.selected});
    });
    svg.addEventListener('pointercancel',()=>{ state.map.dragging=false;state.map.selecting=false;state.map.start=null;viewport.classList.remove('map-dragging'); });
    svg.addEventListener('pointerleave',()=>{if(!state.map.dragging&&!state.map.selecting)hideMapStationTooltip();});
    $('#mapZoomIn').addEventListener('click',()=>zoomMap(1.25)); $('#mapZoomOut').addEventListener('click',()=>zoomMap(.8)); $('#mapReset').addEventListener('click',resetMap);
    selectButton.addEventListener('click',()=>{ const active=viewport.classList.toggle('rectangle-mode'); selectButton.classList.toggle('active',active); toast(active?'在地图上拖动鼠标框选范围':'已退出地图框选'); });
    if (window.ResizeObserver) new ResizeObserver(scheduleMapDraw).observe(viewport);
    else window.addEventListener('resize',scheduleMapDraw);
  }

  const columnDefinitions = [ ['network','站网'],['country','国家'],['format','格式'],['interval','采样率'],['system','系统'],['receiver','接收机'],['antenna','天线'],['radome','天线罩'],['availability','范围完整性'],['status','状态'] ];
  const hiddenColumns = new Set();
  function buildColumnMenu() { $('#columnMenu').innerHTML = columnDefinitions.map(([key,label]) => `<label><input type="checkbox" data-column="${key}" checked>${label}</label>`).join(''); $$('#columnMenu input').forEach(input=>input.addEventListener('change',()=>{ input.checked?hiddenColumns.delete(input.dataset.column):hiddenColumns.add(input.dataset.column);applyColumnVisibility(); })); }
  function applyColumnVisibility() { columnDefinitions.forEach(([key])=>$$(`[data-col="${key}"]`).forEach(node=>node.classList.toggle('hidden',hiddenColumns.has(key)))); }

  function updatePreciseCompletenessInput(){const mode=$('#preciseCompletenessMode').value,input=$('#preciseCompletenessValue'),unit=$('#preciseCompletenessUnit');input.disabled=mode==='ignore';input.max=mode==='ratio'?'100':'100000';input.placeholder=mode==='ratio'?'例如 95':'例如 30';setNodeText(unit,mode==='days'?'天':'%');input.classList.toggle('hidden',mode==='ignore');unit.classList.toggle('hidden',mode==='ignore');}
  function preciseFilterPayload() { return { family:$('#productFamily').value, center:$('#preciseCenter').value, latency:$('#preciseLatency').value, completeness:'any', completenessMode:$('#preciseCompletenessMode').value, completenessValue:$('#preciseCompletenessValue').value||'0', search:$('#preciseSearch').value.trim() }; }
  function preciseCenterFullName(code){
    const key=({EMR:'EMR/NRCan',WHU:'WHU/WUM',WUM:'WHU/WUM'})[code]||code;
    const option=[...($('#preciseCenter')?.options||[])].find(item=>item.value===key);
    return option?.textContent||code||'—';
  }
  const customPresetStorageKey='gnssDownloader.preciseCustomPresets.v1';
  const customFamilies=familyValues.filter(value=>value!=='all').map(value=>[value,preciseFamilyLabels[value]]);
  const customLatencies=[['all','全部等级'],['final','最终'],['rapid','快速'],['ultra-rapid','超快速'],['near real-time','近实时'],['real-time','实时流归档'],['predicted','预报'],['daily','每日'],['weekly','每周'],['static','静态'],['other','其他'],['cumulative','累计解'],['multi-day','多日解'],['solution-other','其他解']];
  const customCenters=[['All analysis centers','全部分析中心/全部机构'],['IGS','IGS / 国际 GNSS 服务'],['CODE','CODE / 欧洲定轨中心'],['EMR/NRCan','EMR/NRCan / 加拿大自然资源部'],['ESA','ESA / 欧洲航天局'],['GFZ','GFZ / 德国地学研究中心'],['GRG','GRG / 法国 GRGS'],['JPL','JPL / 喷气推进实验室'],['MIT','MIT / 麻省理工学院'],['NGS','NGS / 美国国家大地测量局'],['SIO','SIO / 斯克里普斯海洋研究所'],['USNO','USNO / 美国海军天文台'],['WHU/WUM','WHU/WUM / 武汉大学'],['JGX','JGX / 日本多 GNSS 联合分析'],['HUST','HUST / 华中科技大学'],['IERS','IERS / 国际地球自转与参考系服务'],['IAC','IAC / 俄罗斯信息分析中心'],['JAXA','JAXA / 日本宇宙航空研究开发机构'],['SHAO','SHAO / 上海天文台'],['BKG','BKG / 德国联邦制图与大地测量局'],['IGN','IGN / 法国国家地理院'],['DLR','DLR / 德国航空航天中心'],['CAS','CAS / 中国科学院'],['UPC','UPC / 加泰罗尼亚理工大学'],['OPTIMAP','OPTIMAP / OPTIMAP 分析中心'],['Other','Other / 其他机构'],['TU Wien','TU Wien / 维也纳理工大学'],["WCC", "WCC / 武汉组合中心"],["ASI", "ASI / 意大利航天局"],["AUS", "AUS / 澳大利亚地球科学局"],["BEK", "BEK / 巴伐利亚科学院大地测量与冰川学委员会"],["BEV", "BEV / 奥地利联邦计量与测绘局"],["CSR", "CSR / 德克萨斯大学奥斯汀分校空间研究中心"],["DEO", "DEO / 代尔夫特理工大学地球空间研究所"],["EUR", "EUR / EUREF / EPN 组合解"],["GOP", "GOP / 佩奇尼大地测量天文台"],["GSI", "GSI / 日本国土地理院"],["IGE", "IGE / 西班牙国家地理院"],["LPT", "LPT / 瑞士联邦地形局 swisstopo"],["MUT", "MUT / 波兰军事技术大学"],["NCL", "NCL / 纽卡斯尔大学"],["NKG", "NKG / 北欧大地测量委员会"],["OLG", "OLG / 格拉茨空间研究所"],["PDR", "PDR / GFZ / 德累斯顿理工大学联合重处理"],["PGC", "PGC / 加拿大太平洋地学中心"],["RGA", "RGA / 塞尔维亚共和国大地测量局"],["ROB", "ROB / 比利时皇家天文台"],["SGO", "SGO / 匈牙利卫星大地测量天文台"],["SUT", "SUT / 斯洛伐克工业大学"],["TUG", "TUG / 格拉茨工业大学"],["TUM", "TUM / 慕尼黑工业大学"],["ULR", "ULR / 拉罗谢尔大学 / IGN 联合分析"],["UPA", "UPA / 帕多瓦大学"],["WUT", "WUT / 华沙理工大学"],["GIA","GIA / 阿拉斯加大学费尔班克斯分校地球物理研究所"],["MCC","MCC / 俄罗斯任务控制中心"],["EPN","EPN / 欧洲永久 GNSS 站网"]];
  function loadCustomPresets(){try{const value=JSON.parse(localStorage.getItem(customPresetStorageKey)||'[]');state.customPresets=Array.isArray(value)?value.filter(item=>item&&item.id&&item.name&&Array.isArray(item.rules)):[];}catch(_){state.customPresets=[];}renderQuickPresetOptions();renderCustomPresetLibrary();}
  function saveCustomPresets(){try{const serialized=JSON.stringify(state.customPresets);localStorage.setItem(customPresetStorageKey,serialized);if(localStorage.getItem(customPresetStorageKey)!==serialized)throw new Error('custom preset storage verification failed');renderQuickPresetOptions();renderCustomPresetLibrary();return true;}catch(error){console.error('Unable to persist custom precision-product plans',error);toast('保存失败：无法写入本机方案存储');return false;}}
  function renderQuickPresetOptions(){
    const select=$('#quickPreset');if(!select)return;
    const current=select.value,toEn=uiPrefs.language==='en';
    $$('option[data-custom-preset]',select).forEach(option=>option.remove());
    state.customPresets.forEach(item=>{
      const option=document.createElement('option');option.value=`custom:${item.id}`;option.dataset.customPreset='1';
      // The prefix belongs to the UI; the saved name belongs to the user.
      option.dataset.noTranslate='';option.textContent=`${toEn?'Custom: ':'自定义：'}${item.name}`;
      select.appendChild(option);
    });
    if([...select.options].some(option=>option.value===current))select.value=current;
  }
  function renderCustomPresetLibrary(){
    const list=$('#customPresetList'),count=$('#customPresetCount'),toEn=uiPrefs.language==='en',total=state.customPresets.length;
    if(count){count.dataset.noTranslate='';count.textContent=toEn?`${total} ${total===1?'plan':'plans'}`:`${total} 个`;}
    if(!list)return;list.replaceChildren();
    if(!total){
      const empty=document.createElement('p');empty.className='custom-preset-empty';empty.dataset.noTranslate='';
      empty.textContent=localizeUiText('暂无方案，点击“新建方案”开始添加。');list.appendChild(empty);return;
    }
    state.customPresets.forEach(item=>{
      const button=document.createElement('button');button.type='button';button.dataset.customPresetId=item.id;button.dataset.noTranslate='';
      button.textContent=item.name;button.title=`${toEn?'Edit preset: ':'编辑方案：'}${item.name}`;
      button.classList.toggle('active',item.id===state.editingCustomPreset);list.appendChild(button);
    });
  }
  const customSamplingIntervals=[['','不限制'],['01S','1 s'],['05S','5 s'],['10S','10 s'],['15S','15 s'],['30S','30 s'],['01M','1 min'],['04M','4 min'],['05M','5 min'],['10M','10 min'],['15M','15 min'],['20M','20 min'],['30M','30 min'],['01H','1 h'],['02H','2 h'],['03H','3 h'],['06H','6 h'],['12H','12 h'],['01D','1 d'],['07D','7 d']];
  let customRuleCatalog={ready:false,signature:'',rows:[]};
  function customRuleSupportsInterval(family){return !['antex','broadcast','sinex'].includes(family);}
  function customRuleOptionPairs(values,labels,wildcard){
    const available=new Set(values.filter(value=>value!==wildcard));
    const pairs=labels.filter(([value])=>value===wildcard||available.has(value));
    const known=new Set(pairs.map(([value])=>value));
    [...available].filter(value=>!known.has(value)).sort().forEach(value=>pairs.push([value,value]));
    return pairs;
  }
  function customRuleIntervalPairs(values){
    const units={S:['s',1],M:['min',60],H:['h',3600],D:['d',86400],W:['week',604800]};
    const seconds=value=>{const match=/^(\d{1,3})([SMHDW])$/.exec(value);return match?Number(match[1])*units[match[2]][1]:Infinity;};
    return [['','不限制'],...[...new Set(values.filter(Boolean))].sort((a,b)=>seconds(a)-seconds(b)||a.localeCompare(b)).map(value=>{
      const known=customSamplingIntervals.find(([code])=>code===value);
      const match=/^(\d{1,3})([SMHDW])$/.exec(value);
      return [value,known?.[1]||(match?`${Number(match[1])} ${units[match[2]][0]}`:value)];
    })];
  }
  function customRuleChoices(rule={}){
    const family=String(rule.family||'sp3'),savedLatency=String(rule.latency||'final'),savedCenter=String(rule.center||'All analysis centers');
    const savedInterval=String(rule.interval||'').toUpperCase();
    const familyOptions=customRuleOptionPairs([...customFamilies.map(([value])=>value),...customRuleCatalog.rows.map(item=>item.family),family],customFamilies);
    // A startup snapshot may arrive before the full catalog. Keep saved values
    // until native code explicitly marks its complete option list as ready.
    const familyRows=customRuleCatalog.ready?customRuleCatalog.rows.filter(item=>item.family===family):[];
    const latencyOptions=customRuleOptionPairs(customRuleCatalog.ready?familyRows.map(item=>item.latency):[savedLatency],customLatencies,'all');
    const latency=latencyOptions.some(([value])=>value===savedLatency)?savedLatency:'all';
    const latencyRows=familyRows.filter(item=>latency==='all'||item.latency===latency);
    const centerOptions=customRuleOptionPairs(customRuleCatalog.ready?latencyRows.map(item=>item.center):[savedCenter],customCenters,'All analysis centers');
    const center=centerOptions.some(([value])=>value===savedCenter)?savedCenter:'All analysis centers';
    const centerRows=latencyRows.filter(item=>center==='All analysis centers'||item.center===center);
    const intervalOptions=customRuleIntervalPairs(customRuleCatalog.ready?centerRows.map(item=>item.interval):[savedInterval]);
    const supportsInterval=customRuleSupportsInterval(family)&&(!customRuleCatalog.ready||intervalOptions.length>1);
    const interval=supportsInterval&&intervalOptions.some(([value])=>value===savedInterval)?savedInterval:'';
    return {family,latency,center,interval,familyOptions,latencyOptions,centerOptions,intervalOptions,supportsInterval};
  }
  function customRuleOptionsMarkup(values,current){return values.map(([value,label])=>`<option value="${esc(value)}"${value===current?' selected':''}>${esc(localizeUiText(label))}</option>`).join('');}
  function customRuleMarkup(rule={family:'sp3',latency:'final',center:'All analysis centers'}) {
    const choices=customRuleChoices(rule),options=customRuleOptionsMarkup;
    return `<div class="custom-preset-rule"><label><span>产品</span><select data-custom-family>${options(choices.familyOptions,choices.family)}</select></label><label><span>等级</span><select data-custom-latency>${options(choices.latencyOptions,choices.latency)}</select></label><label><span>机构</span><select data-custom-center>${options(choices.centerOptions,choices.center)}</select></label><label><span>采样间隔</span><select data-custom-interval ${choices.supportsInterval?'':'disabled'}>${choices.supportsInterval?options(choices.intervalOptions,choices.interval):options([['','不适用']],'')}</select></label><button type="button" class="icon-btn" data-remove-custom-rule title="删除规则">×</button></div>`;
  }
  function refreshCustomRule(row){
    if(!row)return;
    const selects={family:$('[data-custom-family]',row),latency:$('[data-custom-latency]',row),center:$('[data-custom-center]',row),interval:$('[data-custom-interval]',row)};
    const choices=customRuleChoices(Object.fromEntries(Object.entries(selects).map(([key,select])=>[key,select.value])));
    ['family','latency','center','interval'].forEach(key=>{
      const disabled=key==='interval'&&!choices.supportsInterval;
      selects[key].innerHTML=customRuleOptionsMarkup(disabled?[['','不适用']]:choices[`${key}Options`],choices[key]);
      selects[key].value=choices[key];selects[key].disabled=disabled;
    });
  }
  function refreshCustomRuleOptions(){
    const values=state.latest.preciseRuleOptions,ready=state.latest.preciseRuleOptionsReady===true&&Array.isArray(values);
    const signature=ready?JSON.stringify(values):'';
    if(ready===customRuleCatalog.ready&&signature===customRuleCatalog.signature)return;
    customRuleCatalog={ready,signature,rows:ready?values.filter(item=>item&&item.family&&item.latency&&item.center).map(item=>({family:String(item.family),latency:String(item.latency),center:String(item.center),interval:String(item.interval||'').toUpperCase()})):[]};
    $$('.custom-preset-rule',$('#customPresetRules')).forEach(refreshCustomRule);
  }
  function addCustomRule(rule){$('#customPresetRules').insertAdjacentHTML('beforeend',customRuleMarkup(rule));}
  function setCustomPresetEditor(item){state.editingCustomPreset=item?.id||'';setValue('#customPresetName',item?.name||'');$('#customPresetRules').innerHTML='';(item?.rules?.length?item.rules:[{family:'sp3',latency:'final',center:'All analysis centers'}]).forEach(addCustomRule);$('#deleteCustomPreset').disabled=!item;renderCustomPresetLibrary();}
  function startNewCustomPreset(){setCustomPresetEditor(null);$('#customPresetName').focus();}
  function openCustomPresetManager(){const value=$('#quickPreset').value,item=value.startsWith('custom:')?state.customPresets.find(entry=>entry.id===value.slice(7)):null;setCustomPresetEditor(item);const layer=$('#customPresetLayer');layer.classList.remove('hidden');layer.setAttribute('aria-hidden','false');}
  function readCustomRules(){return $$('.custom-preset-rule',$('#customPresetRules')).map(row=>({family:$('[data-custom-family]',row).value,latency:$('[data-custom-latency]',row).value,center:$('[data-custom-center]',row).value,interval:$('[data-custom-interval]',row).disabled?'':$('[data-custom-interval]',row).value}));}
  function applySelectedQuickPreset(){state.preciseSortDirection='desc';state.precisePage=0;const value=$('#quickPreset').value;if(!value.startsWith('custom:')){post('precise-apply-preset');return;}const item=state.customPresets.find(entry=>entry.id===value.slice(7));if(!item)return toast('自定义方案不存在');const rules=item.rules.map(rule=>`${rule.family}~${rule.latency}~${rule.center}~${customRuleSupportsInterval(rule.family)?rule.interval||'':''}`).join(';');post('precise-custom-preset',{name:item.name,rules});}
  function scheduleObservationFilter(value) {
    state.resultNeedle = value;
    setValue('#stationFilter', value);
    setValue('#resultSearch', value);
    renderObservationRows();
    clearTimeout(filterTimer);
    filterTimer = setTimeout(()=>post('filter',{value}),180);
  }
  function closeSettings() {
    window.MarkWeatherPreview.close();
    closeAdvancedChoicePicker();
    $$('.settings-layer:not(#downloadPlanLayer)').forEach(layer=>{layer.classList.add('hidden');layer.setAttribute('aria-hidden','true');});
  }
  const splitChoiceValues = value => String(value || '').split(/[;,]+/).map(item=>item.trim()).filter(Boolean);
  const choiceSystemLabels = {GPS:'GPS',G:'GPS',GLO:'GLONASS',R:'GLONASS',GAL:'Galileo',E:'Galileo',BDS:'BeiDou',C:'BeiDou',QZS:'QZSS',J:'QZSS',SBS:'SBAS',S:'SBAS',IRN:'NavIC',I:'NavIC'};
  const gimProviderLabels = {IGS:'IGS / 国际 GNSS 服务',CODE:'CODE / 欧洲定轨中心',ESA:'ESA / 欧洲航天局',JPL:'JPL / 喷气推进实验室',UPC:'UPC / 加泰罗尼亚理工大学',CAS:'CAS / 中国科学院',EMR:'EMR / 加拿大自然资源部',WHU:'WHU / 武汉大学'};
  const signalGroupOrder = ['G','R','E','C','J','S','I','?'];
  const signalGroupNames = {G:'GPS',R:'GLONASS',E:'Galileo',C:'BeiDou',J:'QZSS',S:'SBAS',I:'NavIC','?':'其他'};
  const choiceKindNames = {systems:'卫星系统',receivers:'接收机型号',antennas:'天线型号',signals:'观测码 / 频点',gimProviders:'GIM 机构',gimProductGrades:'GIM 产品等级'};
  function signalGroup(value) {
    const token=String(value||'').split(':')[0].trim().toUpperCase();
    return ({GPS:'G',GLO:'R',GLONASS:'R',GAL:'E',BDS:'C',QZS:'J',SBS:'S',IRN:'I'}[token] || token || '?');
  }
  function signalCode(value) {
    const parts=String(value||'').split(':');
    return String(parts.length>1?parts.slice(1).join(':'):parts[0]).trim().toUpperCase();
  }
  function signalRinexVersion(value) {
    const code=signalCode(value);
    return /^[A-Z]\d$/.test(code) || code.length<=2 ? 2 : 3;
  }
  function signalSortKey(value) {
    const code=signalCode(value), version=signalRinexVersion(value);
    const band=Number(code.match(/\d/)?.[0] || 99);
    const kind={C:0,P:1,L:2,D:3,S:4}.hasOwnProperty(code[0])?{C:0,P:1,L:2,D:3,S:4}[code[0]]:8;
    const group=signalGroup(value), groupIndex=signalGroupOrder.indexOf(group);
    return [groupIndex<0?99:groupIndex,version,band,kind,code,choiceDisplay('signals',value)];
  }
  function compareSignalValues(left,right) {
    const a=signalSortKey(left),b=signalSortKey(right);
    for(let index=0;index<a.length;index++) {
      if(a[index]===b[index]) continue;
      if(typeof a[index]==='number'&&typeof b[index]==='number') return a[index]-b[index];
      return String(a[index]).localeCompare(String(b[index]),'en');
    }
    return 0;
  }
  function choiceDisplay(kind,value) {
    if (kind === 'systems') return choiceSystemLabels[value] && choiceSystemLabels[value] !== value ? `${choiceSystemLabels[value]} (${value})` : value;
    if (kind === 'gimProviders') return gimProviderLabels[value] || value;
    if (kind === 'gimProductGrades') {
      const [provider,grade]=String(value).split(':');
      const labels={FINAL:'最终 (FIN)',RAPID:'快速 (RAP)',PREDICTED:'预报 (PRE)',REAL_TIME:'实时 (RT)',OTHER:'其他 / 未分类'};
      return `${provider} · ${localizeUiText(labels[grade]||grade)}`;
    }
    if (kind === 'signals') {
      const parts=String(value).split(':');
      return parts.length===2 ? `${choiceSystemLabels[parts[0]] || parts[0]} · ${parts[1]}` : value;
    }
    return value;
  }
  function updateChoiceTrigger(inputId) {
    const input=$(`#${inputId}`), trigger=$(`.choice-trigger[data-choice-input="${inputId}"]`);
    if(!input||!trigger)return;
    const values=splitChoiceValues(input.value), kind=trigger.dataset.choiceKind;
    const needsProviders=kind==='gimProductGrades'&&!splitChoiceValues($('#advancedGimProviders')?.value).length;
    if(kind==='gimProductGrades')trigger.disabled=needsProviders;
    const emptyText=needsProviders?'请先选择 GIM 机构':kind==='gimProductGrades'?'未选择（全部等级）':trigger.closest('.exclude')?'未选择（不排除）':'未选择（不限制）';
    const summary=values.length ? (values.length<=2?values.map(value=>choiceDisplay(kind,value)).join('、'):`已选择 ${values.length} 项`) : emptyText;
    const label=$('b',trigger);if(label)setNodeText(label,summary);
    trigger.classList.toggle('has-value',values.length>0);
    trigger.title=values.length?values.map(value=>choiceDisplay(kind,value)).join('、'):emptyText;
  }
  function reconcileGimGrades(){
    const providers=new Set(splitChoiceValues($('#advancedGimProviders')?.value));
    const input=$('#advancedGimProductGrades');
    if(input)input.value=splitChoiceValues(input.value).filter(value=>providers.has(value.split(':')[0])).join(',');
    updateChoiceTrigger('advancedGimProductGrades');
  }
  function updateAllChoiceTriggers(){ $$('.choice-trigger').forEach(trigger=>updateChoiceTrigger(trigger.dataset.choiceInput)); }
  function activeChoiceTrigger(){return state.activeChoiceInput?$(`.choice-trigger[data-choice-input="${state.activeChoiceInput}"]`):null;}
  function closeAdvancedChoicePicker(){
    const popover=$('#advancedChoicePopover');if(popover)popover.classList.add('hidden');
    const trigger=activeChoiceTrigger();if(trigger)trigger.classList.remove('active');
    state.activeChoiceInput='';state.choiceNeedle='';state.choiceGroup='all';setValue('#advancedChoiceSearch','');
  }
  function positionAdvancedChoicePicker(trigger){
    const popover=$('#advancedChoicePopover');if(!popover||!trigger)return;
    const rect=trigger.getBoundingClientRect(),width=popover.offsetWidth||460,height=popover.offsetHeight||430;
    const left=Math.max(12,Math.min(rect.left,window.innerWidth-width-12));
    let top=rect.bottom+6;if(top+height>window.innerHeight-12)top=rect.top-height-6;
    top=Math.max(12,Math.min(top,window.innerHeight-height-12));
    popover.style.left=`${Math.round(left)}px`;popover.style.top=`${Math.round(top)}px`;
  }
  function renderAdvancedChoiceList(){
    const trigger=activeChoiceTrigger(),root=$('#advancedChoiceList');if(!trigger||!root)return;
    const kind=trigger.dataset.choiceKind,input=$(`#${state.activeChoiceInput}`),selected=new Set(splitChoiceValues(input?.value));
    const needle=state.choiceNeedle.trim().toLocaleLowerCase();
    const all=Array.isArray(state.advancedChoices[kind])?state.advancedChoices[kind]:[];
    const groups=$('#advancedChoiceGroups');
    let ordered=all;
    if(kind==='gimProductGrades'){
      const providers=new Set(splitChoiceValues($('#advancedGimProviders')?.value));
      const order=['FINAL','RAPID','PREDICTED','REAL_TIME','OTHER'];
      ordered=all.filter(value=>providers.has(value.split(':')[0])).sort((a,b)=>{
        const [pa,ga]=a.split(':'),[pb,gb]=b.split(':');
        return pa.localeCompare(pb)||order.indexOf(ga)-order.indexOf(gb);
      });
    }
    if(kind==='signals'){
      // Group candidates by system and version within one scrollable list.
      if(groups){groups.classList.add('hidden');groups.innerHTML='';}
      ordered=[...all].sort(compareSignalValues);
    }else{
      if(groups){groups.classList.add('hidden');groups.innerHTML='';}state.choiceGroup='all';
    }
    const matched=ordered.filter(value=>(kind!=='signals'||state.choiceGroup==='all'||signalGroup(value)===state.choiceGroup)&&(!needle||choiceDisplay(kind,value).toLocaleLowerCase().includes(needle)));
    const visible=kind==='signals' ? matched : matched.slice(0,200);
    if(state.advancedChoicesLoading&&kind!=='systems'&&!all.length)root.innerHTML='<div class="choice-option-empty">正在从本地索引读取候选项…</div>';
    else if(!visible.length)root.innerHTML='<div class="choice-option-empty">没有匹配的候选项</div>';
    else if(kind==='signals') {
      let previousHeader='';
      root.innerHTML=visible.map(value=>{
        const group=signalGroup(value), version=signalRinexVersion(value), header=`${group}|${version}`;
        const heading=header===previousHeader?'':`<div class="choice-option-group"><b>${esc(signalGroupNames[group]||group)}</b><span>RINEX ${version}</span></div>`;
        previousHeader=header;
        return `${heading}<button type="button" class="choice-option ${selected.has(value)?'checked':''}" data-choice-value="${esc(value)}" title="${esc(choiceDisplay(kind,value))}"><span>${esc(choiceDisplay(kind,value))}</span></button>`;
      }).join('');
    } else if(kind==='gimProductGrades') {
      let previousProvider='';
      root.innerHTML=visible.map(value=>{
        const provider=value.split(':')[0];
        const heading=provider===previousProvider?'':`<div class="choice-option-group"><b>${esc(localizeUiText(gimProviderLabels[provider]||provider))}</b></div>`;
        previousProvider=provider;
        return `${heading}<button type="button" class="choice-option ${selected.has(value)?'checked':''}" data-choice-value="${esc(value)}"><span>${esc(choiceDisplay(kind,value))}</span></button>`;
      }).join('');
    } else root.innerHTML=visible.map(value=>`<button type="button" class="choice-option ${selected.has(value)?'checked':''}" data-choice-value="${esc(value)}" title="${esc(choiceDisplay(kind,value))}"><span>${esc(choiceDisplay(kind,value))}</span></button>`).join('');
    const groupLabel=kind==='signals'&&state.choiceGroup!=='all'?` · ${signalGroupNames[state.choiceGroup]||state.choiceGroup}`:'';
    setText('#advancedChoiceSummary',`${choiceKindNames[kind] || '筛选项'}${groupLabel} · 可多选`);
    setText('#advancedChoiceHint',matched.length>visible.length?`显示前 ${visible.length}/${matched.length} 项，请搜索缩小范围`:`${selected.size} 项已选择 · 共 ${matched.length} 项`);
  }
  function openAdvancedChoicePicker(trigger){
    const previous=activeChoiceTrigger();if(previous)previous.classList.remove('active');
    state.activeChoiceInput=trigger.dataset.choiceInput;state.choiceNeedle='';state.choiceGroup='all';setValue('#advancedChoiceSearch','');trigger.classList.add('active');
    const label=trigger.closest('.settings-control')?.querySelector(':scope > span');setText('#advancedChoiceTitle',label?.textContent||'选择筛选项');
    const popover=$('#advancedChoicePopover');popover.classList.remove('hidden');renderAdvancedChoiceList();positionAdvancedChoicePicker(trigger);
    setTimeout(()=>$('#advancedChoiceSearch')?.focus(),30);
  }
  function handleAdvancedChoices(message){
    ['systems','receivers','antennas','signals','gimProviders','gimProductGrades'].forEach(kind=>{if(Array.isArray(message[kind]))state.advancedChoices[kind]=message[kind];});
    if(!state.advancedChoices.systems.length)state.advancedChoices.systems=['GPS','GLO','GAL','BDS','QZS','SBS','IRN'];
    state.advancedChoicesLoading=false;
    state.advancedChoiceKey=state.pendingAdvancedChoiceKey;state.pendingAdvancedChoiceKey='';
    if(message.error)addInfo(`高级筛选候选项读取提示：${message.error}`,'warning');
    if(state.activeChoiceInput){renderAdvancedChoiceList();positionAdvancedChoicePicker(activeChoiceTrigger());}
  }
  function updateAdvancedInputs() {
    $('#advancedCompletenessPercent').disabled = $('#advancedCompletenessMode').value !== 'ratio';
    $('#advancedMinimumDays').disabled = $('#advancedCompletenessMode').value !== 'days';
    $('#advancedObservationCompletenessPercent').disabled = $('#advancedObservationCompletenessMode').value === 'ignore';
    $('#advancedMaximumLatency').disabled = $('#advancedLatencyMode').value === 'ignore';
    $('#advancedMaximumDetrendedPosition').disabled = $('#advancedPositionTimeSeriesMode').value === 'ignore';
    $('#advancedMaximumResidual').disabled = $('#advancedResidualsMode').value === 'ignore';
    $('#advancedMaximumMultipath').disabled = $('#advancedMultipathMode').value === 'ignore';
    $('#advancedMaximumCycleSlips').disabled = $('#advancedCycleSlipsMode').value === 'ignore';
    $('#advancedCycleSlipsSystem').disabled = $('#advancedCycleSlipsMode').value === 'ignore';
    $('#advancedMultipathSystem').disabled = $('#advancedMultipathMode').value === 'ignore';
    $('#advancedObservationCompletenessSystem').disabled = $('#advancedObservationCompletenessMode').value === 'ignore';
  }
  function updateDateAdvancedInputs() {
    [['#dateKpMode','#dateKpValue','#dateKpUpper'],['#dateDstMode','#dateDstValue','#dateDstUpper'],['#dateF107Mode','#dateF107Value','#dateF107Upper']].forEach(([mode,value,upper])=>{const selected=$(mode).value,row=$(mode).closest('.weather-row'),separator=$('.range-separator',row),unit=$('em',row),range=selected==='range',active=selected!=='ignore';row.classList.toggle('weather-single',!range);row.classList.toggle('weather-inactive',!active);$(value).disabled=!active;$(upper).disabled=!range;$(value).classList.toggle('hidden',!active);$(upper).classList.toggle('hidden',!range);separator.classList.toggle('hidden',!range);unit.classList.toggle('hidden',!active);});
  }
  function openAdvancedSettings() {
    const value = state.latest.advanced || {};
    setValue('#advancedCompletenessMode',value.completenessMode || 'ignore');
    setValue('#advancedCompletenessPercent',value.completenessPercent ?? 90);
    setValue('#advancedMinimumDays',value.minimumDays ?? 0);
    setValue('#advancedSampleInterval',value.sampleInterval || '');
    setValue('#advancedRinexVersion',value.rinexVersion ?? 0);
    setValue('#advancedReferenceFrame',value.referenceFrame || 'ignore');
    setValue('#advancedGimStationMode',value.gimStationMode || 'ignore');
    setValue('#advancedGimProviders',value.gimProviders || '');
    setValue('#advancedGimProductGrades',value.gimProductGrades || '');
    setValue('#advancedObservationCompletenessMode',value.observationCompletenessMode || 'ignore');
    setValue('#advancedObservationCompletenessPercent',value.observationCompletenessPercent ?? 90);
    setValue('#advancedLatencyMode',value.latencyMode || 'ignore');
    setValue('#advancedMaximumLatency',value.maximumMeanLatency ?? 24);
    setValue('#advancedPositionTimeSeriesMode',value.positionTimeSeriesMode || 'ignore');
    setValue('#advancedMaximumDetrendedPosition',value.maximumMeanDetrendedPosition ?? 10);
    setValue('#advancedResidualsMode',value.residualsMode || 'ignore');
    setValue('#advancedMaximumResidual',value.maximumMeanResidual ?? 10);
    setValue('#advancedMultipathMode',value.multipathMode || 'ignore');
    setValue('#advancedMaximumMultipath',value.maximumMeanMultipath ?? 1);
    setValue('#advancedCycleSlipsMode',value.cycleSlipsMode || 'ignore');
    setValue('#advancedCycleSlipsSystem',value.cycleSlipsSystem || '');
    setValue('#advancedMultipathSystem',value.multipathSystem || '');
    setValue('#advancedObservationCompletenessSystem',value.observationCompletenessSystem || '');
    setValue('#advancedMaximumCycleSlips',value.maximumCycleSlips ?? 1000);
    setValue('#advancedSystemsInclude',value.systemsInclude || ''); setValue('#advancedSystemsExclude',value.systemsExclude || '');
    setValue('#advancedReceiverInclude',value.receiverInclude || ''); setValue('#advancedReceiverExclude',value.receiverExclude || '');
    setValue('#advancedAntennaInclude',value.antennaInclude || ''); setValue('#advancedAntennaExclude',value.antennaExclude || '');
    setValue('#advancedSignalsInclude',value.signalCodesInclude || ''); setValue('#advancedSignalsExclude',value.signalCodesExclude || '');
    updateAllChoiceTriggers();updateAdvancedInputs(); closeRowMenu(); closeStationDetail();
    const layer=$('#advancedLayer'); layer.classList.remove('hidden');layer.setAttribute('aria-hidden','false');
    const choiceKey=[$('#startDate').value,$('#endDate').value,...(Array.isArray(state.latest.networks)?state.latest.networks.filter(source=>source.checked).map(source=>source.name):[])].join('|');
    if(bridge&&choiceKey!==state.advancedChoiceKey&&!state.advancedChoicesLoading){state.advancedChoicesLoading=true;state.pendingAdvancedChoiceKey=choiceKey;post('request-advanced-choices');}
  }
  function openDateAdvancedSettings() {
    const value = state.latest.dateAdvanced || {};
    setValue('#dateKpMode',value.kpMode || 'ignore'); setValue('#dateKpValue',value.kpValue ?? 0);setValue('#dateKpUpper',value.kpUpper ?? 4);
    setValue('#dateDstMode',value.dstMode || 'ignore'); setValue('#dateDstValue',value.dstValue ?? -100);setValue('#dateDstUpper',value.dstUpper ?? -50);
    setValue('#dateF107Basis',value.f107Basis || 'adjusted');
    setValue('#dateF107Mode',value.f107Mode || 'ignore'); setValue('#dateF107Value',value.f107Value ?? 70);setValue('#dateF107Upper',value.f107Upper ?? 150);
    updateDateAdvancedInputs(); closeRowMenu(); closeStationDetail();
    const layer=$('#dateAdvancedLayer'); layer.classList.remove('hidden');layer.setAttribute('aria-hidden','false');
    window.MarkWeatherPreview.open();
  }
  function advancedPayload() {
    return {completenessMode:$('#advancedCompletenessMode').value,completenessPercent:$('#advancedCompletenessPercent').value||'0',minimumDays:$('#advancedMinimumDays').value||'0',dailyMode:'ignore',dailyStations:'0',dailyPercent:'0',sampleInterval:$('#advancedSampleInterval').value,rinexVersion:$('#advancedRinexVersion').value,referenceFrame:$('#advancedReferenceFrame').value,gimStationMode:$('#advancedGimStationMode').value,gimProviders:$('#advancedGimProviders').value.trim(),gimProductGrades:$('#advancedGimProductGrades').value.trim(),systemsInclude:$('#advancedSystemsInclude').value.trim(),systemsExclude:$('#advancedSystemsExclude').value.trim(),receiverInclude:$('#advancedReceiverInclude').value.trim(),receiverExclude:$('#advancedReceiverExclude').value.trim(),antennaInclude:$('#advancedAntennaInclude').value.trim(),antennaExclude:$('#advancedAntennaExclude').value.trim(),signalCodesInclude:$('#advancedSignalsInclude').value.trim(),signalCodesExclude:$('#advancedSignalsExclude').value.trim(),observationCompletenessMode:$('#advancedObservationCompletenessMode').value,observationCompletenessPercent:$('#advancedObservationCompletenessPercent').value||'0',latencyMode:$('#advancedLatencyMode').value,maximumMeanLatency:$('#advancedMaximumLatency').value||'0',positionTimeSeriesMode:$('#advancedPositionTimeSeriesMode').value,maximumMeanDetrendedPosition:$('#advancedMaximumDetrendedPosition').value||'0',residualsMode:$('#advancedResidualsMode').value,maximumMeanResidual:$('#advancedMaximumResidual').value||'0',multipathMode:$('#advancedMultipathMode').value,maximumMeanMultipath:$('#advancedMaximumMultipath').value||'0',observationCompletenessSystem:$('#advancedObservationCompletenessSystem').value,multipathSystem:$('#advancedMultipathSystem').value,cycleSlipsSystem:$('#advancedCycleSlipsSystem').value,cycleSlipsMode:$('#advancedCycleSlipsMode').value,maximumCycleSlips:$('#advancedMaximumCycleSlips').value||'0'};
  }
  function dateAdvancedPayload() {
    return {f107Basis:$('#dateF107Basis').value,kpMode:$('#dateKpMode').value,kpValue:$('#dateKpValue').value||'0',kpUpper:$('#dateKpUpper').value||'0',dstMode:$('#dateDstMode').value,dstValue:$('#dateDstValue').value||'0',dstUpper:$('#dateDstUpper').value||'0',f107Mode:$('#dateF107Mode').value,f107Value:$('#dateF107Value').value||'0',f107Upper:$('#dateF107Upper').value||'0'};
  }
  function configureDateInputs() {
    const maxYear=new Date().getFullYear()+1, maxDate=`${maxYear}-12-31`;
    const picker=document.createElement('div');
    picker.className='native-date-picker hidden';
    picker.setAttribute('role','dialog');
    picker.setAttribute('aria-label','日期选择器');
    document.body.appendChild(picker);
    let activeInput=null, viewYear=0, viewMonth=0, popupView='calendar';
    const closePicker=()=>{picker.classList.add('hidden');activeInput=null;};
    const isoDate=(year,month,day)=>`${year}-${String(month+1).padStart(2,'0')}-${String(day).padStart(2,'0')}`;
    const parseDate=value=>{
      const date=new Date(`${value||''}T00:00:00Z`);
      if(!Number.isNaN(date.getTime()))return date;
      const now=new Date(); return new Date(Date.UTC(now.getUTCFullYear(),now.getUTCMonth(),now.getUTCDate()));
    };
    const clampIso=value=>value<activeInput.min?activeInput.min:value>activeInput.max?activeInput.max:value;
    const commitDate=value=>{
      if(!activeInput)return;
      activeInput.value=clampIso(value);
      activeInput.dispatchEvent(new Event('change',{bubbles:true}));
    };
    const renderPicker=()=>{
      if(!activeInput)return;
      const english=document.documentElement.lang==='en';
      const weekdayLabels=english?['Mo','Tu','We','Th','Fr','Sa','Su']:['一','二','三','四','五','六','日'];
      const monthNames=english?['January','February','March','April','May','June','July','August','September','October','November','December']:['一月','二月','三月','四月','五月','六月','七月','八月','九月','十月','十一月','十二月'];
      const selected=activeInput.value?parseDate(activeInput.value):null;
      const selectedIso=activeInput.value||'';
      if(popupView==='year'){
        const years=[];for(let year=Number(activeInput.min.slice(0,4));year<=maxYear;year++)years.push(`<button type="button" data-date-year="${year}" class="${year===viewYear?' selected':''}">${year}</button>`);
        picker.innerHTML=`<div class="date-native-head"><b>${english?'Choose year':'选择年份'}</b><button type="button" class="date-native-back" data-date-back>${english?'Back':'返回'}</button></div><div class="date-native-year-grid">${years.join('')}</div>`;
        const selectedYearButton=picker.querySelector('.date-native-year-grid .selected');if(selectedYearButton)selectedYearButton.scrollIntoView({block:'center'});return;
      }
      if(popupView==='month'){
        picker.innerHTML=`<div class="date-native-head"><b>${english?'Choose month':'选择月份'}</b><button type="button" class="date-native-back" data-date-back>${english?'Back':'返回'}</button></div><div class="date-native-month-grid">${monthNames.map((month,index)=>`<button type="button" data-date-month="${index}" class="${index===viewMonth?' selected':''}">${english?month:`${index+1}月`}</button>`).join('')}</div>`;
        return;
      }
      const firstOffset=(new Date(Date.UTC(viewYear,viewMonth,1)).getUTCDay()+6)%7;
      const cells=[];
      for(let index=0;index<42;index++){
        const date=new Date(Date.UTC(viewYear,viewMonth,1-firstOffset+index));
        const year=date.getUTCFullYear(),month=date.getUTCMonth(),day=date.getUTCDate(),value=isoDate(year,month,day);
        const outside=month!==viewMonth, disabled=value<activeInput.min||value>activeInput.max;
        const selectedClass=value===selectedIso?' selected':'';
        const todayClass=value===isoDate(new Date().getUTCFullYear(),new Date().getUTCMonth(),new Date().getUTCDate())?' today':'';
        const mutedClass=outside?' muted':'';
        const disabledClass=disabled?' disabled':'';
        cells.push(`<button type="button" data-date-value="${value}" class="date-native-day${selectedClass}${todayClass}${mutedClass}${disabledClass}" ${disabled?'disabled':''}>${day}</button>`);
      }
      picker.innerHTML=`<div class="date-native-head"><div class="date-native-title"><button type="button" data-date-year-toggle>${english?viewYear:`${viewYear}年`}</button><button type="button" data-date-month-toggle>${english?monthNames[viewMonth]:`${String(viewMonth+1).padStart(2,'0')}月`}</button></div><div class="date-native-nav"><button type="button" data-date-prev aria-label="${english?'Previous month':'上个月'}"><svg viewBox="0 0 16 16" aria-hidden="true"><path d="M4 10 8 6l4 4"></path></svg></button><button type="button" data-date-next aria-label="${english?'Next month':'下个月'}"><svg viewBox="0 0 16 16" aria-hidden="true"><path d="m4 6 4 4 4-4"></path></svg></button></div></div><div class="date-native-week">${weekdayLabels.map(day=>`<span>${day}</span>`).join('')}</div><div class="date-native-grid">${cells.join('')}</div><div class="date-native-foot"><button type="button" data-date-clear>${english?'Clear':'清除'}</button><button type="button" data-date-today>${english?'Today':'今天'}</button></div>`;
      if(selected&&selected.getUTCFullYear()===viewYear&&selected.getUTCMonth()===viewMonth){const selectedDay=picker.querySelector('.date-native-day.selected');if(selectedDay)selectedDay.scrollIntoView({block:'nearest'});}
    };
    const openPicker=input=>{
      if(activeInput===input&&!picker.classList.contains('hidden')){closePicker();return;}
      activeInput=input;
      const date=parseDate(input.value); viewYear=date.getUTCFullYear(); viewMonth=date.getUTCMonth(); popupView='calendar'; renderPicker();
      const rect=input.getBoundingClientRect(),width=248,height=316;
      picker.style.left=Math.max(8,Math.min(rect.left,window.innerWidth-width-8))+'px';
      picker.style.top=Math.max(8,Math.min(rect.bottom+4,window.innerHeight-height-8))+'px';
      picker.classList.remove('hidden');
    };
    ['#startDate','#endDate','#preciseStartDate','#preciseEndDate'].forEach(selector=>{
      const input=$(selector); if(!input)return;
      const minYear=input.id.startsWith('precise')?1980:1994;
      input.min=`${minYear}-01-01`; input.max=maxDate; input.title=`范围：${minYear} 年至 ${maxYear} 年`;
      input.addEventListener('pointerdown',event=>{event.preventDefault();openPicker(input);});
      input.addEventListener('click',event=>{event.preventDefault();if(activeInput!==input||picker.classList.contains('hidden'))openPicker(input);});
    });
    picker.addEventListener('click',event=>{
      event.stopPropagation(); if(!activeInput)return;
      if(event.target.closest('[data-date-back]')){popupView='calendar';renderPicker();return;}
      if(event.target.closest('[data-date-year-toggle]')){popupView='year';renderPicker();return;}
      if(event.target.closest('[data-date-month-toggle]')){popupView='month';renderPicker();return;}
      if(event.target.closest('[data-date-prev]')){viewMonth--;if(viewMonth<0){viewMonth=11;viewYear--;}const minYear=Number(activeInput.min.slice(0,4));if(viewYear<minYear){viewYear=minYear;viewMonth=0;}renderPicker();return;}
      if(event.target.closest('[data-date-next]')){viewMonth++;if(viewMonth>11){viewMonth=0;viewYear++;}if(viewYear>maxYear){viewYear=maxYear;viewMonth=11;}renderPicker();return;}
      const year=event.target.closest('[data-date-year]');
      if(year){const date=parseDate(activeInput.value);viewYear=Number(year.dataset.dateYear);viewMonth=date.getUTCMonth();popupView='month';const day=Math.min(date.getUTCDate(),new Date(Date.UTC(viewYear,viewMonth+1,0)).getUTCDate());commitDate(isoDate(viewYear,viewMonth,day));renderPicker();return;}
      const month=event.target.closest('[data-date-month]');
      if(month){const date=parseDate(activeInput.value);viewMonth=Number(month.dataset.dateMonth);popupView='calendar';const day=Math.min(date.getUTCDate(),new Date(Date.UTC(viewYear,viewMonth+1,0)).getUTCDate());commitDate(isoDate(viewYear,viewMonth,day));renderPicker();return;}
      const day=event.target.closest('[data-date-value]');if(day&&!day.disabled){commitDate(day.dataset.dateValue);closePicker();return;}
      if(event.target.closest('[data-date-clear]')){activeInput.value='';activeInput.dispatchEvent(new Event('change',{bubbles:true}));closePicker();return;}
      if(event.target.closest('[data-date-today]')){const now=new Date();commitDate(isoDate(now.getUTCFullYear(),now.getUTCMonth(),now.getUTCDate()));closePicker();}
    });
    document.addEventListener('click',event=>{if(!picker.contains(event.target)&&!event.target.matches('input[type="date"]'))closePicker();});
    document.addEventListener('keydown',event=>{if(event.key==='Escape')closePicker();});
    window.addEventListener('resize',closePicker);
  }
  function bindEvents() {
    document.addEventListener('toggle',event=>{
      const node=event.target;if(!node.matches?.('.equipment-period')||!node.open)return;
      const item=state.stationDetailData?.periods?.[Number(node.dataset.periodIndex)];
      const body=node.querySelector('.equipment-period-body');
      if(item&&body&&!body.childElementCount)body.innerHTML=equipmentPeriodMarkup(item);
    },true);
    configureDateInputs();
    $$('.mode-tab').forEach(button=>button.addEventListener('click',()=>setMode(button.dataset.mode)));
    $('#sourceSearch').addEventListener('input',event=>{state.sourceNeedle=event.target.value;renderSourcePicker(Array.isArray(state.latest.networks)?state.latest.networks:[]);});
    $('#sourceSelectAvailable').addEventListener('click',()=>post('source-toggle-available'));
    $('#sourcePickerGrid').addEventListener('click',event=>{
      const button=event.target.closest('.source-picker-item');
      if(button)post('source',{value:button.dataset.source});
    });
    $('#sourceChips').addEventListener('click',event=>{const more=event.target.closest('#openSourcePicker');if(more){openSourceSettings();return;}const chip=event.target.closest('.source-chip');if(chip)post('source',{value:chip.dataset.source});});
    $('#observationPreset').addEventListener('change',event=>post('observation-preset',{value:event.target.value}));
    $('#dateAdvanced').addEventListener('click',openDateAdvancedSettings);
    $('#stationAdvanced').addEventListener('click',openAdvancedSettings);
    const syncQueryDates=()=>post('query-dates',{start:$('#startDate').value,end:$('#endDate').value});
    $('#startDate').addEventListener('change',syncQueryDates);
    $('#endDate').addEventListener('change',syncQueryDates);
    $('#stationFilter').addEventListener('input',event=>scheduleObservationFilter(event.target.value));
    $('#clearStationFilter').addEventListener('click',()=>scheduleObservationFilter(''));
    $('#queryStations').addEventListener('click',()=>post('query',{start:$('#startDate').value,end:$('#endDate').value}));
    $('#openCountryPicker').addEventListener('click',openCountrySettings);
    $('#countrySearch').addEventListener('input',event=>{state.countryNeedle=event.target.value;renderCountryPicker();});
    $('#countryPickerGroups').addEventListener('click',event=>{
      const continentButton=event.target.closest('[data-continent-select]');
      if(continentButton){
        const continent=continentButton.dataset.continentSelect;
        const codes=state.countryOptions.filter(item=>item.continent===continent).map(item=>item.code);
        const selected=new Set(state.pendingCountries);
        const allSelected=codes.length>0&&codes.every(code=>selected.has(code));
        codes.forEach(code=>allSelected?selected.delete(code):selected.add(code));
        state.pendingCountries=[...selected];
        renderCountryPicker();
        return;
      }
      const button=event.target.closest('[data-country-code]');if(!button)return;
      const code=button.dataset.countryCode;
      state.pendingCountries=state.pendingCountries.includes(code)?state.pendingCountries.filter(item=>item!==code):[...state.pendingCountries,code];
      renderCountryPicker();
    });
    $('#clearCountrySelection').addEventListener('click',()=>{state.pendingCountries=[];renderCountryPicker();});
    $('#applyCountrySelection').addEventListener('click',()=>{const order=new Map(state.countryOptions.map((item,index)=>[item.code,index]));state.selectedCountries=[...new Set(state.pendingCountries)].sort((a,b)=>(order.get(a)??9999)-(order.get(b)??9999));updateCountryTrigger();closeSettings();post('countries',{value:state.selectedCountries.join(',')});});
    $('#applyBounds').addEventListener('click',()=>post('apply-bounds',{latMin:$('#latMin').value,latMax:$('#latMax').value,lonMin:$('#lonMin').value,lonMax:$('#lonMax').value}));
    $('#clearScope').addEventListener('click',()=>{ ['latMin','latMax','lonMin','lonMax'].forEach(id=>setValue(`#${id}`,''));state.selectedCountries=[];state.pendingCountries=[];updateCountryTrigger();post('clear-scope'); });
    window.MarkSelection.install({post,toast,redraw:scheduleMapDraw,translate:localizeUiText});
    $$('.segmented [data-map-view]').forEach(button=>button.addEventListener('click',()=>{ state.mapView=button.dataset.mapView;$$('.segmented [data-map-view]').forEach(item=>item.classList.toggle('active',item===button));renderMapPoints(); }));
    $('#mapFullscreen').addEventListener('click',()=>{ $('#mapCard').classList.toggle('fullscreen');setTimeout(applyMapTransform,60); });
    $('#resultSearch').addEventListener('input',event=>scheduleObservationFilter(event.target.value));
    $('#selectedRowsOnly').addEventListener('click',()=>post('rows-selected-only',{enabled:!state.rowsSelectedOnly}));
    $('#sortStations').addEventListener('click',()=>{state.ascending=state.sortColumn==='station'?!state.ascending:true;state.sortColumn='station';setText('#sortStationsLabel',state.ascending?'站号 A–Z':'站号 Z–A');renderObservationRows();post('sort-stations',{direction:state.ascending?'asc':'desc',column:'station'});});
    $('#sortStationCompleteness').addEventListener('click',()=>{
      state.ascending=state.sortColumn==='completeness'?!state.ascending:false;state.sortColumn='completeness';
      renderObservationRows();post('sort-stations',{direction:state.ascending?'asc':'desc',column:'completeness'});
    });
    $('#sortPreciseCompleteness').addEventListener('click',()=>{
      state.preciseSortDirection=state.preciseSortDirection==='desc'?'asc':'desc';state.precisePage=0;renderPreciseRows();
    });
    $('#toggleColumns').addEventListener('click',()=>$('#columnMenu').classList.toggle('hidden'));
    $('#toggleDensity').addEventListener('click',()=>{state.dense=!state.dense;$('#stationTable').classList.toggle('dense',state.dense);setText('#toggleDensityLabel',state.dense?'紧凑密度':'舒适密度');});
    $('#viewMenuButton').addEventListener('click',()=>$('#viewMenu').classList.toggle('hidden'));
    $$('#viewMenu button').forEach(button=>button.addEventListener('click',()=>$('#viewMenu').classList.add('hidden')));
    $('#selectAllRows').addEventListener('click',()=>post('select-all')); $('#clearSelection').addEventListener('click',()=>post('clear-selection'));
    $('#addObservationPlan').addEventListener('click',()=>post('add-download-plan',{mode:'observation'}));
    $('#previousRows').addEventListener('click',()=>post('rows-page',{direction:'previous'})); $('#nextRows').addEventListener('click',()=>post('rows-page',{direction:'next'}));
    $('#stationRows').addEventListener('click',event=>{
      const check=event.target.closest('[data-check]');
      if(check){const row=findObservation(check.dataset.check);post('select-station',{station:check.dataset.check,checked:!(row&&row.selected)});return;}
      const menuButton=event.target.closest('[data-row-menu]');
      if(menuButton){event.stopPropagation();openRowMenu(menuButton,findObservation(menuButton.dataset.rowMenu));return;}
      const rowElement=event.target.closest('[data-station]');
      if(rowElement)showStationDetail(findObservation(rowElement.dataset.station));
    });
    $('#stationRows').addEventListener('keydown',event=>{if(event.key!=='Enter'&&event.key!==' ')return;const rowElement=event.target.closest('[data-station]');if(!rowElement)return;event.preventDefault();showStationDetail(findObservation(rowElement.dataset.station));});
    $('#rowActionMenu').addEventListener('click',event=>{const action=event.target.closest('[data-row-action]');if(!action)return;const row=findObservation(state.actionStation);if(!row){closeRowMenu();return;}if(action.dataset.rowAction==='detail')showStationDetail(row);else if(action.dataset.rowAction==='locate'){locateStation(row);closeRowMenu();}else if(action.dataset.rowAction==='toggle'){post('select-station',{station:row.id,checked:!row.selected});closeRowMenu();}else if(action.dataset.rowAction==='copy'){copyStation(row);closeRowMenu();}});
    $$('[data-close-station-detail]').forEach(button=>button.addEventListener('click',closeStationDetail));
    $('#detailLocate').addEventListener('click',()=>locateStation(findObservation(state.detailStation)));
    $('#detailToggleSelection').addEventListener('click',()=>{const row=findObservation(state.detailStation);if(row)post('select-station',{station:row.id,checked:!row.selected});});
    $$('[data-close-settings]').forEach(button=>button.addEventListener('click',closeSettings));
    $$('.choice-trigger').forEach(button=>button.addEventListener('click',()=>openAdvancedChoicePicker(button)));
    $('#advancedChoiceSearch').addEventListener('input',event=>{state.choiceNeedle=event.target.value;renderAdvancedChoiceList();});
    const advancedChoiceGroups=$('#advancedChoiceGroups');
    if(advancedChoiceGroups)advancedChoiceGroups.addEventListener('click',event=>{const group=event.target.closest('[data-choice-group]');if(!group)return;state.choiceGroup=group.dataset.choiceGroup;renderAdvancedChoiceList();});
    $('#advancedChoiceList').addEventListener('click',event=>{const option=event.target.closest('[data-choice-value]');if(!option||!state.activeChoiceInput)return;event.preventDefault();event.stopPropagation();const input=$(`#${state.activeChoiceInput}`),values=splitChoiceValues(input.value),value=option.dataset.choiceValue,index=values.indexOf(value);if(index>=0)values.splice(index,1);else values.push(value);input.value=values.join(',');if(state.activeChoiceInput==='advancedGimProviders')reconcileGimGrades();updateChoiceTrigger(state.activeChoiceInput);renderAdvancedChoiceList();});
    $('#clearAdvancedChoice').addEventListener('click',()=>{if(!state.activeChoiceInput)return;setValue(`#${state.activeChoiceInput}`,'');if(state.activeChoiceInput==='advancedGimProviders')reconcileGimGrades();updateChoiceTrigger(state.activeChoiceInput);renderAdvancedChoiceList();});
    $('#closeAdvancedChoice').addEventListener('click',closeAdvancedChoicePicker);$('#doneAdvancedChoice').addEventListener('click',closeAdvancedChoicePicker);
    ['#advancedCompletenessMode','#advancedObservationCompletenessMode','#advancedLatencyMode','#advancedPositionTimeSeriesMode','#advancedResidualsMode','#advancedMultipathMode','#advancedCycleSlipsMode'].forEach(selector=>$(selector).addEventListener('change',updateAdvancedInputs));
    ['#dateKpMode','#dateDstMode','#dateF107Mode','#dateF107Basis'].forEach(selector=>$(selector).addEventListener('change',updateDateAdvancedInputs));
    $('#resetAdvanced').addEventListener('click',()=>{setValue('#advancedCompletenessMode','ignore');setValue('#advancedCompletenessPercent','90');setValue('#advancedMinimumDays','0');setValue('#advancedSampleInterval','30S');setValue('#advancedRinexVersion','0');setValue('#advancedReferenceFrame','ignore');setValue('#advancedGimStationMode','ignore');setValue('#advancedGimProviders','');setValue('#advancedGimProductGrades','');setValue('#advancedObservationCompletenessMode','ignore');setValue('#advancedObservationCompletenessPercent','90');setValue('#advancedLatencyMode','ignore');setValue('#advancedMaximumLatency','24');setValue('#advancedPositionTimeSeriesMode','ignore');setValue('#advancedMaximumDetrendedPosition','10');setValue('#advancedResidualsMode','ignore');setValue('#advancedMaximumResidual','10');setValue('#advancedMultipathMode','ignore');setValue('#advancedMaximumMultipath','1');setValue('#advancedCycleSlipsMode','ignore');setValue('#advancedMaximumCycleSlips','1000');setValue('#advancedCycleSlipsSystem','');setValue('#advancedMultipathSystem','');setValue('#advancedObservationCompletenessSystem','');['#advancedSystemsInclude','#advancedSystemsExclude','#advancedReceiverInclude','#advancedReceiverExclude','#advancedAntennaInclude','#advancedAntennaExclude','#advancedSignalsInclude','#advancedSignalsExclude'].forEach(selector=>setValue(selector,''));updateAllChoiceTriggers();updateAdvancedInputs();});
    $('#resetDateAdvanced').addEventListener('click',()=>{setValue('#dateKpMode','ignore');setValue('#dateDstMode','ignore');setValue('#dateF107Mode','ignore');setValue('#dateKpValue','0');setValue('#dateKpUpper','4');setValue('#dateDstValue','-100');setValue('#dateDstUpper','-50');setValue('#dateF107Value','80');setValue('#dateF107Upper','150');setValue('#dateF107Basis','adjusted');updateDateAdvancedInputs();});
    $('#advancedForm').addEventListener('submit',event=>{event.preventDefault();if(!event.currentTarget.reportValidity())return;post('apply-advanced',advancedPayload());closeSettings();addInfo('已提交高级测站筛选，正在刷新本地结果。','notice');});
    $('#dateAdvancedForm').addEventListener('submit',event=>{event.preventDefault();if(!event.currentTarget.reportValidity())return;post('apply-date-advanced',dateAdvancedPayload());closeSettings();addInfo('已提交日期高级筛选，正在按空间天气条件刷新。','notice');});

  $('#quickPreset').addEventListener('change',event=>{if(!event.target.value.startsWith('custom:'))post('precise-preset',{value:event.target.value});}); $('#applyQuickPreset').addEventListener('click',applySelectedQuickPreset);
    $('#manageQuickPresets').addEventListener('click',openCustomPresetManager);
    $('#newCustomPreset').addEventListener('click',startNewCustomPreset);
    $('#customPresetList').addEventListener('click',event=>{const button=event.target.closest('[data-custom-preset-id]');if(!button)return;const item=state.customPresets.find(entry=>entry.id===button.dataset.customPresetId);if(item)setCustomPresetEditor(item);});
    $('#addCustomRule').addEventListener('click',()=>addCustomRule());
    $('#customPresetRules').addEventListener('click',event=>{const button=event.target.closest('[data-remove-custom-rule]');if(!button)return;button.closest('.custom-preset-rule')?.remove();if(!$('.custom-preset-rule',$('#customPresetRules')))addCustomRule();});
    $('#customPresetRules').addEventListener('change',event=>{if(event.target.matches('[data-custom-family],[data-custom-latency],[data-custom-center]'))refreshCustomRule(event.target.closest('.custom-preset-rule'));});
    $('#customPresetForm').addEventListener('submit',event=>{event.preventDefault();if(!event.currentTarget.reportValidity())return;const name=$('#customPresetName').value.trim(),rules=readCustomRules();if(!rules.length)return toast('请至少添加一种产品');const id=state.editingCustomPreset||`${Date.now()}-${Math.random().toString(36).slice(2,7)}`,item={id,name,rules},previous=state.customPresets.map(entry=>({...entry,rules:entry.rules.map(rule=>({...rule}))}));const index=state.customPresets.findIndex(entry=>entry.id===id);if(index>=0)state.customPresets[index]=item;else state.customPresets.push(item);if(!saveCustomPresets()){state.customPresets=previous;return;}setValue('#quickPreset',`custom:${id}`);closeSettings();toast('自定义方案已保存');});
    $('#deleteCustomPreset').addEventListener('click',()=>{if(!state.editingCustomPreset)return;const previous=state.customPresets;state.customPresets=state.customPresets.filter(item=>item.id!==state.editingCustomPreset);if(!saveCustomPresets()){state.customPresets=previous;return;}setValue('#quickPreset','none');state.editingCustomPreset='';closeSettings();post('precise-preset',{value:'none'});toast('自定义方案已删除');});
    $('#queryProducts').addEventListener('click',()=>{state.preciseSortDirection='desc';state.precisePage=0;setValue('#quickPreset','none');post('products',{start:$('#preciseStartDate').value,end:$('#preciseEndDate').value,...preciseFilterPayload()});});
    ['preciseStartDate','preciseEndDate'].forEach(id=>$("#"+id).addEventListener('change',()=>post('precise-dates',{start:$('#preciseStartDate').value,end:$('#preciseEndDate').value})));
    $('#productFamily').addEventListener('change',event=>{if($('#quickPreset').value.startsWith('custom:'))setValue('#quickPreset','none');updateProductLevelOptions(true);post('product-family',{value:event.target.value});post('precise-filters',preciseFilterPayload());}); $('#preciseCenter').addEventListener('change',event=>{if($('#quickPreset').value.startsWith('custom:'))setValue('#quickPreset','none');post('center',{value:event.target.value});});
    $('#preciseCompletenessMode').addEventListener('change',updatePreciseCompletenessInput);
    $('#preciseSelectAll').addEventListener('click',()=>post('precise-select-all')); $('#preciseClear').addEventListener('click',()=>post('precise-clear'));
    $('#addPrecisePlan').addEventListener('click',()=>post('add-download-plan',{mode:'precise'}));
    $('#precisePreviousRows').addEventListener('click',()=>{state.precisePage=Math.max(0,state.precisePage-1);renderPreciseRows();});
    $('#preciseNextRows').addEventListener('click',()=>{state.precisePage++;renderPreciseRows();});
    $('#preciseRows').addEventListener('click',event=>{const row=event.target.closest('[data-product-index]');if(!row)return;const product=state.preciseRows[Number(row.dataset.productIndex)];if(!product)return;showProductDetail(product);if(event.target.closest('[data-product-check]'))post('select-product',{product:product.id,checked:!product.selected});});

    $$('.info-tabs button').forEach(button=>button.addEventListener('click',()=>{state.infoFilter=button.dataset.infoFilter;$$('.info-tabs button').forEach(item=>item.classList.toggle('active',item===button));renderInfo();}));
    $('#infoScroll').addEventListener('click',event=>{const button=event.target.closest('[data-info-dismiss]');if(!button)return;const id=Number(button.dataset.infoDismiss);state.info=state.info.filter(item=>item.id!==id);renderInfo();});
    $('#clearInfo').addEventListener('click',()=>{state.info=[];renderInfo();});
    $('#copyInfo').addEventListener('click',async()=>{const text=state.info.map(item=>`[${item.time}] ${runtimeMessageText(item)}`).join('\n');try{await navigator.clipboard.writeText(text);toast('信息已复制');}catch(_){toast('当前环境无法访问剪贴板');}});
    $('#browseFolder').addEventListener('click',()=>post(state.mode==='precise'?'precise-browse':'browse-folder'));
    $('#openDownloadFolder').addEventListener('click',()=>post('open-folder',{mode:state.mode}));
    const startPlan = () => post('start-download-plan',{mode:state.mode});
    $('#startDownload').addEventListener('click',startPlan);
    $('#downloadPlanStart').addEventListener('click',startPlan);
    $('#downloadPlanRemove').addEventListener('click',()=>post('download-plan-item',{
      mode:state.mode,id:'0',action:'remove-selected'
    }));
    $('#pauseDownload').addEventListener('click',()=>post('pause')); $('#cancelDownload').addEventListener('click',()=>post('cancel'));
    $('#openDownloadPlan').addEventListener('click',openDownloadPlan);
    $('#downloadPlanLayer').addEventListener('click',event=>{if(event.target.closest('[data-close-download-plan]'))closeDownloadPlan();});
    $('#downloadPlanSelectAll').addEventListener('change',event=>post('download-plan-item',{
      mode:state.mode,id:'0',action:event.target.checked?'select':'unselect'
    }));
    $('#downloadPlanList').addEventListener('change',event=>{
      const checkbox=event.target.closest('[data-plan-select]');
      const row=checkbox?.closest('[data-plan-id]');
      if(row)post('download-plan-item',{mode:state.mode,id:row.dataset.planId,action:checkbox.checked?'select':'unselect'});
    });
    $('#downloadPlanList').addEventListener('click',event=>{
      if(event.target.closest('#downloadPlanMore')){
        state.planVisible[state.mode]+=100;
        state.planSignature=null;
        renderDownloadPlan();
        return;
      }
      const button=event.target.closest('[data-plan-action]');
      const row=button?.closest('[data-plan-id]');
      if(row)post('download-plan-item',{mode:state.mode,id:row.dataset.planId,action:button.dataset.planAction});
    });
    document.addEventListener('click',event=>{if(!event.target.closest('#toggleColumns')&&!event.target.closest('#columnMenu'))$('#columnMenu').classList.add('hidden');if(!event.target.closest('#viewMenuWrap'))$('#viewMenu').classList.add('hidden');if(!event.target.closest('#rowActionMenu')&&!event.target.closest('[data-row-menu]'))closeRowMenu();if(state.activeChoiceInput&&!event.target.closest('#advancedChoicePopover')&&!event.target.closest('.choice-trigger'))closeAdvancedChoicePicker();});
    document.addEventListener('keydown',event=>{if(event.key==='Escape'){closeDownloadPlan();closeRowMenu();closeStationDetail();closeSettings();}});
  }

  function showNativeBridgeRequired() {
    const message = '请运行 MARK.exe 以查询本地知识库。';
    addInfo(message, 'notice');
    setText('.index-state span', message);
    setText('#resultSummary', message);
    setText('#preciseResultSummary', message);
    document.querySelectorAll('#queryStations, #queryProducts, #startDownload, #startPreciseDownload').forEach(button => { button.disabled = true; });
  }

  /* ===== 界面偏好：语言 / 皮肤（持久化到 localStorage） ===== */
  const uiPrefsKey = 'igs.ui.prefs';
  const uiLanguageKey = window.markUiLanguageKey || 'igs.ui.language.portable';
  const enDict = {
    '知识库数据截至':'Knowledge-base data through', '显示全部':'Show all', 
    'SP3 索引':'SP3 index', 'CLK 索引':'CLK index', 'SP3 / CLK 索引':'SP3 / CLK index', 'GIM 产品索引':'GIM product index',
    "软件版本": "Application version",
    "构建日期": "Build date",
    "GitHub 仓库": "GitHub repository",
    "知识库覆盖日期": "Knowledge-base coverage",
    "观测数据索引": "Observation index",
    "精密产品索引": "Precise product index",
    "IGS / MGEX 质量指标": "IGS / MGEX quality metrics",
    "空间天气数据": "Space-weather data",
    "各站网和产品的实际覆盖范围不同，以查询结果为准。": "Coverage varies by network and product; refer to query results.",
    "更新说明": "Release notes",
    "完整率、多路径与相位周跳支持独立选择卫星系统。": "Choose constellations independently for completeness, multipath and cycle slips.",
    "安装进度显示用时及预计剩余时间。": "Setup shows elapsed time and an estimated time remaining.",
    "下载任务保存文件结果及汇总日志。": "Download tasks save file results and a summary log.",
    "支持测站高级筛选、自定义精密产品方案与下载计划。": "Advanced station filters, custom precise-product presets and download plans.",
    "支持本地元数据检索与多来源文件下载。": "Local metadata searches and file downloads from multiple providers.",
    "逐日筛选 · IGS / MGEX": "Daily filters · IGS / MGEX",
    "测站整体": "Station-wide",
    "全部系统": "All systems",
    "条件": "Condition",
    "完整率 ≥": "Completeness ≥",
    "最低完整率 (%)": "Minimum (%)",
    "延迟 ≤": "Latency ≤",
    "上限 (h)": "Maximum (h)",
    "上限 (mm)": "Maximum (mm)",
    "上限 (cm)": "Maximum (cm)",
    "上限 (次/日)": "Maximum (/day)",
    "平均绝对值 ≤": "Mean absolute value ≤",
    "平均值 ≤": "Mean ≤",
    "总数 ≤": "Total ≤",
    "1 · 观测完整率": "1 · Observation completeness",
    "2 · RINEX 文件延迟": "2 · RINEX file latency",
    "6 · 相位周跳": "6 · Phase cycle slips",
    '空间范围':'Spatial scope',
    '选站方案':'Station selection',
    '测站选择':'Station selection',
    '从当前候选测站中选择固定的一组测站':'Choose one fixed set from the current candidates',
    '每日可用约束':'Daily availability',
    '每日站数 ≥':'Daily count ≥',
    '每日比例 ≥':'Daily ratio ≥',
    '每天至少（站）':'Daily minimum (stations)',
    '每天至少（%）':'Daily minimum (%)',
    '检查当前选站':'Check selected set',
    '取消计算':'Cancel calculation',
    '区域组网与随机复现':'Regional network & reproducibility',
    '最大连边距离（km）':'Maximum link (km)',
    '随机种子':'Random seed',
    '尚未检查选站覆盖':'Selection coverage not checked',
    '覆盖详情':'Coverage details',
    '选站覆盖检查':'Station-set coverage',
    '已确认可用':'Confirmed available',
    '待核实不计入可用站数':'Unverified records do not count as available',
    '不足日期':'Dates below threshold',
    '全部日期':'All dates',
    '待核实日期':'Dates with unverified records',
    '已排除日期':'Excluded dates',
    '评估日期':'Evaluation dates',
    '每日最少可用':'Minimum daily available',
    '平均每日可用':'Average daily available',
    '查看当天地图':'View this day on map',
    '返回完整地图':'Return to full map',
    '导出检查结果':'Export report',
    '日期':'Date',
    '所选可用':'Selected available',
    '所选待核实':'Selected unverified',
    '候选可用':'Candidate available',
    '候选待核实':'Candidate unverified',
    '日期筛选排除':'Excluded by date filter',
    '当前页无匹配日期':'No matching dates on this page',
    '所选测站':'Selected stations',
    '实际最小间距':'Actual minimum spacing',
    '候选方案尚未采用':'Candidate plan has not been applied',
    '条件或选站已变化，请重新检查':'Inputs or selection changed; check again',
    '正在后台计算选站方案…':'Calculating station selection in the background…',
    '请选择测站后检查':'Select stations before checking',
    '检查结果尚未就绪':'Report is not ready',
    '未知记录':'Unverified records',
    '不足日期不会自动排除':'Dates below threshold are retained',
    '方案满足全部约束':'All selection constraints satisfied',
    '方案未满足全部约束':'Selection constraints not fully satisfied',
    '请重新查询或检查当前选站':'Query or check the selected set again',
    '国家/地区':'Country / region','选择国家/地区':'Select countries / areas',
    '全部国家/地区':'All countries / regions','应用国家/地区范围':'Apply country / region scope',
    '可选择一个或多个国家/地区。':'Select one or more countries or regions.',
    '搜索国家/地区名称或代码，例如 格陵兰、GRL、DNK':'Search country / region name or code, e.g. Greenland, GRL, DNK',
    '不选择任何国家/地区时使用全球范围；经纬度条件仍可叠加。':'No country / region selection uses global scope; latitude and longitude can also apply.',
    '没有匹配的国家/地区':'No matching country / region',
    '地区按国家归属合并，地位未定地区单列；可按名称或国别代码搜索':'Regions are grouped by country affiliation; unresolved areas are listed separately. Search by name or code.',
    '搜索国家名称或代码，例如 中国、CHN、DNK':'Search country name or code, e.g. China, CHN, DNK',
    '每天的可用子集仍可能存在空间偏差，可点击日期检查地图。':'The available subset may have spatial gaps on individual days; inspect dates on the map.',
    '同站跨数据源只计一次；按当前采样率、RINEX 与元数据条件匹配的记录统计。':'Each physical station counts once across sources; records follow the current interval, RINEX and metadata filters.',
    '日期覆盖':'Date coverage','测站信息':'Station information','产品信息':'Product information','详情视图':'Detail view',
    '知识库详细信息':'Indexed station information','设备历史':'Equipment history','未收录':'Not indexed',
    '完整测站标识':'Full station identifier','四字符站号':'Four-character identifier','测站名称':'Station name',
    '城市':'City','Site Log 国家':'Site Log country','负责机构':'Responsible agency','运行单位':'Operator',
    '测站所属国家':'Station country',
    '接收机型号':'Receiver model','接收机序列号':'Receiver serial','固件版本':'Firmware',
    '接收机安装日期':'Receiver installation','天线型号':'Antenna model','天线序列号':'Antenna serial',
    '天线罩':'Radome','天线参考点':'Antenna reference point','天线安装日期':'Antenna installation',
    'Site Log 修订日期':'Site Log revision','Site Log 数据源':'Site Log source','Site Log 链接':'Site Log URL',
    '已收录观测码':'Indexed observation codes','序列号':'Serial number','安装日期':'Installation date',
    '设备摘要取查询范围首个有记录日期；观测码为范围内已收录的并集，不表示每天都具备。':'Equipment is shown for the first indexed day. Observation codes are the union over the range, not a claim of daily support.',
    '正在读取日期覆盖…':'Loading date coverage…',
    "GNSS 观测数据与精密产品": "GNSS observations & precise products",
    "正在准备 GNSS 工作台": "Preparing your GNSS workspace",
    "观测数据工作台": "Observation workspace",
    "精密产品工作台": "Precise product workspace",
    "检索测站、筛选数据并建立下载计划。": "Find stations, filter data and build a download plan.",
    "按方案与规则筛选所需的精密产品。": "Select precise products using plans and filtering rules.",
    "当前时间范围 · UTC": "CURRENT DATE RANGE \u00b7 UTC",
    "运行记录": "Activity log",
    "关于 MARK": "About MARK",
    "关闭运行记录": "Close activity log",
    "关闭关于窗口": "Close about window",
    "查看查询、筛选与下载的实时状态。": "Follow query, filter and download activity.",
    "从元数据，到所需数据。": "From metadata to the data you need.",
    "依据元数据发现数据，以规则筛选测站与产品，将所需文件加入下载计划。": "Discover data through metadata, filter stations and products with rules, and add the files you need to a download plan.",
    "观测数据检索": "Observation discovery",
    "站网、空间范围与测站元数据": "Networks, spatial scope and station metadata",
    "精密产品筛选": "Precise product selection",
    "快速方案与自定义产品规则": "Quick plans and custom product rules",
    "规则驱动筛选": "Rule-driven filtering",
    "完整性、系统设备与空间天气": "Completeness, equipment and space weather",
    "统一下载计划": "Unified download plans",
    "选择、批量下载与运行记录": "Selection, batch downloads and activity",
    '观测数据':'Observations','精密产品':'Precise Products','设置':'Settings','语言':'Language','皮肤':'Appearance',
    '亮色':'Light','暗色':'Dark','本地索引已就绪':'Local index ready',
    '观测数据检索 · 空间筛选 · 批量下载':'Observation search · Spatial filter · Batch download',
    '数据源':'Data sources','时间范围':'Time range','测站筛选':'Station filter','空间范围与批量选择':'Spatial scope & batch pick',
    '默认全球范围':'Global by default','站号、站网、国家或设备信息':'Station ID, network, country or equipment',
    '起始日期':'Start date','终止日期':'End date','日期高级筛选（空间天气）':'Date filters (space weather)',
    '查询预设':'Query preset','高级筛选':'Advanced filter','查询测站':'Query stations','选择国家':'Country',
    '全部国家':'All countries','纬度':'Lat','经度':'Lon','最小':'Min','最大':'Max','目标数量':'Target count',
    '最小间距 km':'Min spacing km','最小间距（km）':'Minimum spacing (km)','最大邻距（km）':'Maximum neighbor distance (km)','应用经纬度':'Apply lat/lon','地图框选':'Box select','清除范围':'Clear scope',
    '均匀选站':'Even spread','随机选站':'Random pick','全球测站地图':'Global station map',
    '查询后显示满足条件的测站':'Stations matching the query appear here','全部测站':'All stations',
    '仅已选择':'Selected only','满足条件':'Matched','已选择':'Selected','最已选择':'Selected',
    '测站结果':'Station results','尚未查询':'Not queried','搜索全部结果':'Search all results','仅看已选':'Selected only',
    '视图':'View','全选当前列表':'Select all','清空选择':'Clear selection',
    '测站':'Station','站网':'Network','国家':'Country','格式':'Format','采样率':'Interval','接收机':'Receiver',
    '系统':'System','天线 / 罩':'Antenna / Radome','范围完整性':'Completeness','状态':'Status','操作':'Actions',
    '暂无测站结果':'No station results','查询后，结果会在这里显示且可独立滚动':'Results appear here after a query',
    '运行窗口':'Runtime','运行提示、筛选结果和下载状态':'Hints, filter results and download status',
    '下载任务':'Downloads','保存目录':'Save folder','浏览':'Browse','打开目录':'Open folder','暂停':'Pause','取消':'Cancel','开始下载':'Start download',
    '尚未选择测站':'No station selected','快速方案':'Quick preset','不使用快速方案':'No quick preset',
    '自定义方案':'Custom preset','应用方案并查询':'Apply & query',
    'PRIDE PPP-AR｜整数固定定位产品':'PRIDE PPP-AR | ambiguity-fixed positioning products',
    'IGS 最终 PPP｜标准精密定位产品':'IGS Final PPP | standard precise positioning products',
    'UPD PPP-AR｜相位偏差整数固定':'UPD PPP-AR | phase-bias ambiguity fixing',
    'OSB PPP-AR｜观测偏差整数固定':'OSB PPP-AR | observable-bias ambiguity fixing',
    'PPP-RTK｜网络高精度定位基础文件':'PPP-RTK | core files for network positioning',
    '通用 PPP 智能优选｜自动找完整产品':'Automatic PPP | find a complete product bundle',
    '产品类型':'Product type','产品等级':'Product level','生产机构 / 分析中心':'Analysis center',
    '日解':'Daily solution','周解':'Weekly solution','累计解':'Cumulative solution','多日解':'Multi-day solution','其他解':'Other solution',
    '周期':'Period','解算周期':'Solution period','全部解算周期':'All solution periods','发布等级':'Publication level','等级 / 周期':'Level / period',
    'VMF1 / VMF3 对流层格网':'VMF1 / VMF3 tropospheric grids',
    'VMF 对流层格网':'VMF Troposphere grid','VMF / 维也纳映射函数':'VMF / Vienna Mapping Function','格网':'grid','栅格':'grid',
    'TU Wien / 维也纳理工大学':'TU Wien / Vienna University of Technology',
    '本地产品清单':'Local product list','产品详情':'Product details','尚未选择产品':'No product selected',
    '产品 ID':'Product ID','分析中心':'Center','等级':'Level','星座':'Constellation','间隔':'Interval',
    '获取方式':'Access','可用范围':'Availability','查询产品':'Query products',
    '选择日期与筛选条件后查询':'Query after choosing dates and filters','全部等级':'All levels','不限制':'Any',
    '自然色':'Natural','舒适密度':'Comfortable','紧凑密度':'Compact','列设置':'Columns','站号 A–Z':'Station A–Z'
,'普通查询（按当前条件）':'Standard query (current filters)','IGS/ITRF 参考框架（按日期自动选择）':'IGS/ITRF reference frame (date-aware)',
    'GIM 电离层产品相关测站':'GIM ionosphere stations','GIM 评估（全球均匀·每日可用约束）':'GIM evaluation (even spread · daily availability)','多系统 POD（全球均匀 60 站）':'Multi-GNSS POD (60 global stations)',
    'Network RTK 候选网（区域组网）':'Network RTK candidates','例如 ZIM2、MGEX、CHN、POLARX5...':'e.g. ZIM2, MGEX, CHN, POLARX5…',
    '搜索国家缩写，例如 CHN、USA、DEU':'Search country code, e.g. CHN, USA, DEU',
    '搜索产品 ID、文件名或分析中心':'Search product ID, file name or center',
    '全部产品':'All Products','全部机构':'All Centers','全部分析中心/全部机构':'All Analysis Centers / All Centers','仅保留 >':'Keep >','排除 >':'Exclude >','至':'to',
    '清空选择':'Clear selection','取消':'Cancel','应用国家范围':'Apply country scope','新建方案':'New preset',
    '已保存方案':'Saved presets','方案名称':'Preset name','添加产品':'Add product','保存方案':'Save preset',
    '删除当前方案':'Delete preset','产品规则':'Product rules','应用并刷新结果':'Apply and refresh','清除全部条件':'Clear all',
    'MARK: Metadata-Guided Automation of Resource Workflows':'MARK: Metadata-Guided Automation of Resource Workflows','MARK':'MARK','GNSS 元数据系统':'GNSS metadata system','元数据 · 规则 · 发现':'METADATA · RULES · DISCOVERY',
    '全球范围':'Global scope','等待查询':'Waiting for query','等待查询测站':'Waiting for station query','等待任务':'Waiting for task',
    '设置条件后点击“查询测站”':'Set filters, then click “Query stations”','设备信息':'Device information',
    '选择测站':'Select station','测站详情':'Station details','查看测站详情':'View station details','复制测站信息':'Copy station information','在地图中定位':'Locate on map',
    '信息类型':'Information type','全部':'All','提示':'Notice','警告':'Warning','下载':'Download','当前筛选':'Active filters',
    '删除此条信息':'Delete this message','未设置额外筛选':'No extra filters','等待读取筛选条件…':'Reading active filters…',
    '全选所有站网':'Select all networks','取消全选所有站网':'Clear all networks','全选有知识库':'Select indexed networks','取消全选（有知识库）':'Clear indexed networks',
    '全部 321':'All 321','可多选':'Multiple selection','选择筛选项':'Choose filter items','没有匹配的候选项':'No matching candidates',
    'RINEX 版本':'RINEX version','RINEX 观测数':'RINEX observation count','RINEX 延迟':'RINEX latency',
    '接收机与卫星系统':'Receiver and satellite systems','可用性与观测格式':'Availability and observation format',
    '限制选定日期范围内的文件完整性和数据格式':'Limit file completeness and data format within the selected dates',
    '处理方式':'Processing mode','单站完整性':'Single-station completeness','每日覆盖条件':'Daily coverage',
    '每日测站数 ≥':'Daily station count ≥','每日测站比例 ≥':'Daily station ratio ≥','每日最低测站数':'Minimum daily stations','每日最低比例 (%)':'Minimum daily ratio (%)',
    '最小间距 km':'Minimum spacing (km)','最大邻距 km':'Maximum neighbor distance (km)','目标数量':'Target count',
    "六项指标均按日筛选；仅支持 IGS / MGEX 测站":"All six metrics are filtered daily. IGS / MGEX stations only.",
    "每日延迟 ≤":"Daily latency ≤",
    "最大每日延迟 (h)":"Maximum daily latency (h)",
    "每日平均绝对值 ≤":"Daily mean absolute ≤",
    "最大每日平均去趋势位移 (mm)":"Maximum daily mean detrended displacement (mm)",
    "最大每日平均绝对残差 (mm)":"Maximum daily mean absolute residual (mm)",
    "每日平均值 ≤":"Daily mean ≤",
    "最大每日平均多路径 (cm)":"Maximum daily mean multipath (cm)",
    "每日总数 ≤":"Daily total ≤",
    "最大每日周跳 (次/日)":"Maximum daily cycle slips (per day)",
    "当天所有已启用指标达标才计入合格日期，缺少记录的日期不计入；完整率按当天实际/计划观测数计算，位置和残差取当天分量绝对值的平均，多路径取当天有效值的平均，周跳取当天总数；零多路径值不参与平均。":"A date qualifies only when every enabled metric passes. Dates with missing records do not count. Completeness uses daily actual/planned counts. Position and residuals use daily means of absolute components, multipath uses the daily mean of valid values, and cycle slips use the daily total. Zero multipath values are excluded.",
    "合格日期覆盖率":"Qualified date coverage",
    "最低合格日期覆盖率 (%)":"Minimum qualified date coverage (%)",
    "合格日期比例 ≥":"Qualified date ratio ≥",
    "最低合格天数":"Minimum qualified days",
    "合格天数 ≥":"Qualified days ≥",
    "限制合格日期覆盖率和数据格式，合格日期同时满足文件、属性及已启用的质量条件":"Limit qualified date coverage and file format. Qualified dates meet file, attribute and enabled quality criteria.",
    '观测质量筛选':'Observation quality filters','每日观测完整率 ≥':'Daily completeness ≥','最低每日观测完整率 (%)':'Minimum daily completeness (%)',
    '平均绝对去趋势位移 ≤':'Mean absolute detrended displacement ≤','最大平均去趋势位移 (mm)':'Maximum mean detrended displacement (mm)',
    '平均绝对残差 ≤':'Mean absolute residual ≤','最大平均绝对残差 (mm)':'Maximum mean absolute residual (mm)',
    '平均每日总数 ≤':'Mean daily total ≤','最大平均每日周跳 (次/日)':'Maximum mean daily cycle slips (per day)',
    '最大平均多路径 (cm)':'Maximum mean multipath (cm)','最大平均延迟 (h)':'Maximum mean latency (h)',
    '期间平均值 ≤':'Period mean ≤','期间平均延迟 ≤':'Period mean latency ≤','最低完整率 (%)':'Minimum completeness (%)','最低可用天数':'Minimum available days','可用天数 ≥':'Available days ≥',
    '参考框架名册':'Reference frame catalogue','IGS / ITRF 参考框架':'IGS / ITRF reference frame',
    '仅保留 GIM 使用测站':'Keep GIM stations only','排除 GIM 使用测站':'Exclude GIM stations','GIM 建模测站':'GIM modelling stations','GIM 产品等级':'GIM product grades','EPN / 欧洲永久 GNSS 站网':'EPN / EUREF Permanent GNSS Network','MCC / 俄罗斯任务控制中心':'MCC / Russian Mission Control Center','GIA / 阿拉斯加大学费尔班克斯分校地球物理研究所':'GIA / Geophysical Institute, University of Alaska Fairbanks',"WCC / 武汉组合中心":"WCC / Wuhan Combination Center","ASI / 意大利航天局":"ASI / Italian Space Agency","AUS / 澳大利亚地球科学局":"AUS / Geoscience Australia","BEK / 巴伐利亚科学院大地测量与冰川学委员会":"BEK / Bavarian Academy of Sciences, Geodesy and Glaciology","BEV / 奥地利联邦计量与测绘局":"BEV / Austrian Federal Office of Metrology and Surveying","CSR / 德克萨斯大学奥斯汀分校空间研究中心":"CSR / Center for Space Research, University of Texas at Austin","DEO / 代尔夫特理工大学地球空间研究所":"DEO / Delft Institute for Earth-Oriented Space Research","EUR / EUREF / EPN 组合解":"EUR / EUREF / EPN combined solutions","GOP / 佩奇尼大地测量天文台":"GOP / Geodetic Observatory Pecny","GSI / 日本国土地理院":"GSI / Geospatial Information Authority of Japan","IGE / 西班牙国家地理院":"IGE / Instituto Geografico Nacional, Spain","LPT / 瑞士联邦地形局 swisstopo":"LPT / Federal Office of Topography swisstopo","MUT / 波兰军事技术大学":"MUT / Military University of Technology, Poland","NCL / 纽卡斯尔大学":"NCL / Newcastle University","NKG / 北欧大地测量委员会":"NKG / Nordic Geodetic Commission","OLG / 格拉茨空间研究所":"OLG / Institute for Space Research, Graz","PDR / GFZ / 德累斯顿理工大学联合重处理":"PDR / GFZ / TU Dresden joint reprocessing","PGC / 加拿大太平洋地学中心":"PGC / Pacific Geoscience Centre, Canada","RGA / 塞尔维亚共和国大地测量局":"RGA / Republic Geodetic Authority, Serbia","ROB / 比利时皇家天文台":"ROB / Royal Observatory of Belgium","SGO / 匈牙利卫星大地测量天文台":"SGO / Satellite Geodetic Observatory, Hungary","SUT / 斯洛伐克工业大学":"SUT / Slovak University of Technology","TUG / 格拉茨工业大学":"TUG / Graz University of Technology","TUM / 慕尼黑工业大学":"TUM / Technical University of Munich","ULR / 拉罗谢尔大学 / IGN 联合分析":"ULR / University of La Rochelle / IGN consortium","UPA / 帕多瓦大学":"UPA / University of Padova","WUT / 华沙理工大学":"WUT / Warsaw University of Technology",'先选择 GIM 机构，再选择各机构的产品等级':'Choose GIM providers, then choose their product grades','请先选择 GIM 机构':'Choose GIM providers first','未选择（全部等级）':'Not selected (all grades)','最终 (FIN)':'Final (FIN)','快速 (RAP)':'Rapid (RAP)','预报 (PRE)':'Predicted (PRE)','实时 (RT)':'Real-time (RT)','其他 / 未分类':'Other / Unclassified','未选择等级时使用所选机构的全部等级；选定后仅使用勾选的机构与等级组合。按整个查询日期范围取测站并集，不按每日观测日期匹配。只有数量、没有测站标识的产品不参与。':'If no grades are selected, all grades from the selected providers are used. Otherwise only checked provider and grade pairs are used. Station IDs are combined across the full query date range, not matched to each observation day. Count-only products without station IDs are excluded.','GIM 机构':'GIM providers',
    '按日期限制为对应 IGS/ITRF 官方核心测站名册；跨框架日期范围使用重叠框架的测站并集':'Use the official IGS/ITRF core-station list for each date; combine overlapping frames across ranges',
    '按当前 UTC 日期范围，使用所选机构每日 GIM 中记录的测站并集进行筛选':'Use the union of stations recorded in the selected providers’ daily GIMs for the UTC range',
    '按大洲浏览并可同时选择多个国家；每个大洲可一键全选；国旗旁显示 ISO 三字母简写':'Browse countries by continent; select multiple countries and use continent-level select-all; ISO-3 codes are shown beside flags',
    '完整保留原版的 42 个全球与区域站网，并标出本地索引状态':'Includes all 42 global and regional networks and shows local index status',
    '不可用的数据源会保留显示，但不会触发无效网络查询。':'Unavailable sources remain visible but are not queried.',
    '不选择任何国家时使用全球范围；经纬度条件仍可叠加。':'Leave countries empty to use the global scope; latitude/longitude filters can still be combined.',
    '选择日期与筛选条件后查询':'Query after selecting dates and filters','选择一行查看完整信息':'Select a row to view full details',
    '产品 ID、获取方式与可用范围会显示在这里':'Product ID, access method and availability appear here','暂无产品结果':'No product results',
    '本机保存，不修改知识库。':'Saved locally; the knowledge base is not modified.','应用后只读取本地知识库，无需联网。':'Only the local knowledge base is read after applying; no network is required.',
    '恢复默认':'Restore defaults','完成':'Done','保存目录':'Save folder','尚未选择内容':'Nothing selected','尚未选择产品':'No product selected','尚未选择测站':'No station selected',
    '拖动平移 · 滚轮缩放（最高 32×）· 双击恢复 · ':'Drag to pan · wheel to zoom (up to 32×) · double-click to reset · ',
    '单位：Dst = nT，F10.7 = sfu；数据来自本地空间天气索引。':'Units: Dst = nT, F10.7 = sfu; data come from the local space-weather index.',
    '先按 Kp、Dst、F10.7 保留日期，再计算测站范围完整性':'Filter dates by Kp, Dst and F10.7 before calculating station-range completeness',
    '观测完整率逐日检查，其他指标按期间汇总；仅支持 IGS / MGEX 测站':'Observation completeness is checked daily. Other metrics use period aggregates. IGS / MGEX stations only.',
    '每日观测完整率 = 当天实际观测数 / 当天计划观测数，每天均须达标，缺失日不通过；位置采用东、北、高三个去趋势分量绝对值的期间平均；残差采用东、北、高三个分量绝对值的期间平均；周跳为各卫星系统每日总数的期间平均。启用某项后，无该类记录的测站会被排除；零多路径值按“无数据”处理。':'Daily observation completeness = actual / planned observations for each day. Every query date must pass, and missing days fail. Position and residual metrics use period means of absolute E/N/U components; cycle slips use the period mean of daily totals per constellation. Stations without the enabled metric are excluded; zero multipath is treated as missing data.',
    '观测':'Observation','数据':'Data','测站':'Station','站网':'Network','国家':'Country','机构':'Provider','产品':'Product','方案':'Plan','范围':'Scope','日期':'Date','条件':'Condition','信息':'Information','操作':'Action',
    '包含':'Include','排除':'Exclude','不排除':'Do not exclude','未选择':'Not selected','等待':'Waiting','读取':'Reading','索引':'Index','知识库':'knowledge base','可用':'Available','未索引':'Not indexed','当前显示':'Showing','个数据源':'data sources','个国家':'countries','个测站':'stations','个产品':'products','项已选择':'selected','共':'Total ','项':'items',
    '起':'from','（':' (','）':')','单位':'Unit','下限':'Lower bound','上限':'Upper bound','例如':'e.g.','说明':'Note','用法':'Usage','提示':'Notice','警告':'Warning','平静':'Quiet','不稳定':'Unsettled','活跃':'Active','中等':'Moderate','强':'Strong','极强':'Extreme',
    '正在读取数据源…':'Reading data sources…','正在读取本地知识库':'Reading local knowledge base','等待本地索引…':'Waiting for local index…','运行信息将在这里显示':'Runtime information appears here','选择测站后开始下载':'Select stations to start downloading','尚未选择内容':'Nothing selected','尚未选择测站':'No station selected','尚未选择产品':'No product selected',
    '恢复全球视图':'Reset global view','缩小':'Zoom out','放大':'Zoom in','双击恢复':'Double-click to reset','最高':'Maximum','全屏地图':'Fullscreen map','地图框选':'Box select on map','查询后显示满足条件的测站':'Stations matching the query appear here',
    '按日期自动选择':'Date-aware automatic selection','日期范围':'Date range','起始日期':'Start date','终止日期':'End date','应用并刷新结果':'Apply and refresh results','清除全部条件':'Clear all conditions','正在按空间天气条件刷新':'Refreshing by space-weather conditions',
    '最终':'Final','快速':'Rapid','超快速':'Ultra-rapid','近实时':'Near real-time','实时流归档':'Real-time stream archive','预报':'Predicted','每日':'Daily','每周':'Weekly','静态':'Static','其他':'Other','其他机构':'Other providers',
    '观测码':'Observation code','频点':'Frequency','卫星系统':'Satellite system','接收机型号':'Receiver model','天线型号':'Antenna model','使用测站':'Stations used','参考框架':'Reference frame','全球范围':'Global scope','区域组网':'Regional network',
    '中国台湾':'Taiwan, China','武汉大学':'Wuhan University','中国科学院':'Chinese Academy of Sciences','上海天文台':'Shanghai Astronomical Observatory','日本宇宙航空研究开发机构':'Japan Aerospace Exploration Agency','斯克里普斯海洋研究所':'Scripps Institution of Oceanography',
    '数据来自本地空间天气索引':'Data from the local space-weather index','适合跨多个框架的查询':'Suitable for ranges spanning multiple frames','使用本地可用性索引':'Uses the local availability index','日质量知识库':'daily-quality knowledge base','使用所选机构每日':'Uses each selected provider’s daily','中记录的测站并集进行筛选':'recorded station union for filtering','日期范围仍会与该框架有效期进行提示':'the date range is checked against the frame validity period','按大洲浏览并可同时选择多个国家':'Browse by continent and select multiple countries','所有方案都会出现在上方':'All plans appear in the list above','启用某项后':'When enabled','暂时仅支持':'Currently supports only','再计算测站范围完整性':'then calculate station-range completeness',
    '搜寻':'Search','搜索':'Search','刷新':'Refresh','删除':'Delete','复制':'Copy','清空':'Clear','完成':'Done','返回':'Back','应用':'Apply','取消':'Cancel','打开':'Open','关闭':'Close','全选':'Select all','选择':'Select',
    '请选择测站后开始下载':'Please select stations to start downloading','综合产品':'combined product','最终产品组合':'final product combination','偏差产品组合':'bias-product combination','同机构组合':'same-center combination','通用产品':'general product','轨道':'Orbit','钟差':'Clock offset','偏差':'Bias','姿态':'Attitude','大气产品':'Atmospheric products','一站式下载':'one-stop download','广播星历':'Broadcast ephemeris','差分码偏差':'Differential code bias','卫星姿态':'Satellite attitude','电离层':'Ionosphere','对流层':'Troposphere','测站坐标':'Station coordinates','天线校准':'Antenna calibration','未校准相位延迟':'Uncalibrated phase delay','全球均匀':'Globally even','候选网':'Candidate network','快速方案':'Quick preset','本地产品清单':'Local product list','查询条件':'query conditions','按当前条件':'current conditions','读取本地知识库':'Read local knowledge base','无需联网':'no network required','索引已就绪':'Index ready','尚未收录':'Not indexed yet','没有匹配':'No match','请尝试其他':'Try another','空间天气':'space weather','日期筛选':'date filters','测站范围':'station range','文件完整性':'file completeness','数据格式':'data format','范围完整性':'range completeness','页面':'page','共':'Total ','条':' messages','运行信息':'runtime messages'
  };
  // Runtime strings and map metadata are produced outside the static HTML,
  // so keep their complete phrases here.  Long phrases are intentionally
  // listed before short terms to avoid mixed Chinese/English output.
  Object.assign(enDict, {
    '本地记录': 'Local inventory',
    '本地记录覆盖率': 'Local inventory coverage',
    '未配置 UPD 在线下载地址；覆盖率表示本地存档记录。': 'UPD download URLs are not configured. Coverage reflects the supplied local archive.',
    'UPD 只有本地记录，尚未配置在线下载地址。': 'UPD has local inventory records only; online download URLs are not configured.',
    'UPD 文件头声明的配套产品': 'Input products declared in the UPD header',
    '所选范围内最新记录': 'Latest record in the selected range',
    '未收录 UPD 配套文件声明': 'No UPD input-file declarations indexed'
  });
  Object.assign(enDict, {
    '中文':'Chinese','工作台切换':'Workbench mode','产品筛选':'Product filters','信息':'Information',
    '选择数据源和日期后，点击“查询测站”。':'Select data sources and dates, then click “Query stations”.',
    '选择数据源和日期后，点击“查询测站”':'Select data sources and dates, then click “Query stations”',
    '请运行 MARK.exe 以查询本地知识库。':'Open MARK.exe to query the local knowledge bases.',
    '本地索引已就绪。请选择测站后开始下载。':'Local index ready. Select stations to start downloading.',
    '本地产品索引已就绪。':'Local product index ready.',
    '当前分类暂无信息':'No messages in this category',
    '运行提示、筛选结果和下载状态':'Runtime hints, filter results and download status',
    '部分可用':'Partially available','不可用':'Unavailable','可用':'Available','已解析':'Parsed',
    '按日期解析':'Date-resolved','范围可用':'Range available','范围完整':'Range complete',
    '可下载':'Downloadable','部分日期可用':'Partially available','等待查询':'Waiting for query',
    '等待任务':'Waiting for task','正在处理任务':'Processing task','下载进行中':'Download in progress',
    '清空本洲':'Clear continent','全选本洲':'Select all in continent','其他地区':'Other regions',
    '亚洲':'Asia','欧洲':'Europe','非洲':'Africa','北美洲':'North America','南美洲':'South America','大洋洲':'Oceania','南极洲':'Antarctica',
    '已选择':'Selected','满足条件':'Matched','暂无坐标':'No coordinates','暂无':'None','正在读取…':'Reading…',
    '已从地图框选空间范围。':'Spatial bounds selected from the map.','在地图上拖动鼠标框选范围':'Drag on the map to select a rectangle',
    '已退出地图框选':'Map selection mode closed','该测站暂无可用坐标':'No usable coordinates for this station',
    '测站信息已复制':'Station information copied','当前环境无法访问剪贴板':'Clipboard unavailable in this environment',
    '已定位':'Located','测站详情':'Station details','取消选择':'Deselect','选择测站':'Select station',
    '已提交高级测站筛选，正在刷新本地结果。':'Advanced station filters submitted; refreshing local results.',
    '已提交日期高级筛选，正在按空间天气条件刷新。':'Advanced date filters submitted; refreshing by space-weather conditions.',
    '请至少添加一种产品':'Add at least one product','自定义方案已保存':'Custom preset saved','自定义方案已删除':'Custom preset deleted',
    '保存失败：无法写入本机方案存储':'Save failed: unable to write local preset storage','自定义方案不存在':'Custom preset not found',
    '暂无方案，点击“新建方案”开始添加。':'No presets yet. Click “New preset” to add one.','删除规则':'Delete rule',
    '例如：我的 IGS 最终产品':'e.g. My IGS final products','本机保存，不修改知识库。':'Saved locally; the knowledge base is unchanged.','预设：':'Preset: ',
    '同一方案中的不同产品可以来自不同机构；保存后可继续“新建方案”，所有方案都会出现在上方“快速方案”列表中。':'Products in one plan can come from different providers. After saving, click “New preset” to add another; all plans appear in the “Quick preset” list above.',
    '正在从本地索引读取候选项…':'Reading candidates from the local index…','没有匹配的候选项':'No matching candidates',
    '请输入 ISO 三字母国家简写':'Enter a three-letter ISO country code','没有匹配的国家':'No matching countries',
    '显示前':'Showing first','项，请搜索缩小范围':'items; search to narrow the list','项已选择':'selected;','共':'Total ',
    '高级筛选候选项读取提示：':'Advanced-filter candidate warning:','等待读取筛选条件…':'Waiting for active filters…',
    '上个月':'Previous month','下个月':'Next month','日期选择器':'Date picker','范围：1994 年至':'Range: 1994–','范围：1980 年至':'Range: 1980–','年':'year','月':'month','今天':'Today',
    '产品':'Product','数据源':'Data source','候选数据源':'Candidate sources','文件与下载源':'Files and download sources',
    '观测格式':'Observation format','数据间隔':'Data interval','采样间隔':'Sampling interval','不适用':'Not applicable','格式版本':'Format version','索引说明':'Index notes','首选文件名':'Preferred filename',
    '产品类型':'Product type','分析中心':'Analysis center','产品等级':'Product level','卫星系统':'Satellite systems',
    '天线 / 天线罩':'Antenna / radome','地理坐标':'Geographic coordinates','坐标':'Coordinates','天线/罩':'Antenna/radome',
    '已选择：':'Selected: ','未限定国家':'No country restriction','按日期读取':'Date-aware','高速索引':'Fast index',
    '空间天气日期筛选':'Space-weather date filter','高级':'Advanced','每日覆盖':'Daily coverage','ITRF/IGS 框架':'ITRF/IGS frame',
    'GIM 测站':'GIM stations','筛选项':'Filter items','可多选':'Multiple selection','不排除':'Do not exclude','不限制':'No restriction',
    '一般':'General','国家':'Country','站网':'Network','格式':'Format','采样率':'Interval','接收机':'Receiver','天线':'Antenna','状态':'Status','操作':'Actions',
    '设置条件后点击“查询测站”':'Set filters, then click “Query stations”',
    '拖动平移':'Drag to pan','滚轮缩放':'Wheel to zoom','双击恢复':'Double-click to reset','等经纬投影':'Equirectangular projection','全球范围':'Global scope',
    '离线参考底图':'Offline reference map',
    'Natural Earth 公有领域数据；等经纬投影。':'Natural Earth public-domain data; equirectangular projection.',
    '北极圈':'Arctic Circle','北回归线':'Tropic of Cancer','赤道':'Equator','南回归线':'Tropic of Capricorn','南极圈':'Antarctic Circle',
    '中国台湾':'Taiwan, China','中国':'China','美国':'United States','加拿大':'Canada','巴西':'Brazil','阿根廷':'Argentina','澳大利亚':'Australia','日本':'Japan','韩国':'South Korea','印度':'India','俄罗斯':'Russia','德国':'Germany','法国':'France','英国':'United Kingdom','意大利':'Italy','西班牙':'Spain','南非':'South Africa','墨西哥':'Mexico','土耳其':'Türkiye','埃及':'Egypt','沙特阿拉伯':'Saudi Arabia','伊朗':'Iran','印度尼西亚':'Indonesia','新西兰':'New Zealand'
    ,'本地知识库可用':'Local knowledge base available','本地知识库尚未收录':'Not indexed in the local knowledge base','没有匹配的数据源':'No matching data sources','请尝试其他站网名称':'Try another network name','本地可用':'locally available','个本地可用':'locally available','当前条件':'current conditions','当前结果':'current results','满足当前条件':'match the current conditions','数据源选择':'data-source selection','国家选择':'country selection','高级筛选':'Advanced filters','正在刷新':'Refreshing','刷新结果':'refresh results','候选项':'candidates','候选网':'candidate network','天':'days','年':'year','月':'month','范围':'range','条件':'conditions','完成':'Done','失败':'Failed','错误':'Error','未设置':'Not set','保存目录':'save folder','下载源':'download source','文件':'file','文件名':'filename','来源':'source','详情':'details','状态':'status','摘要':'summary','请选择':'Please select','请先':'Please first','暂无':'None','正在':'In progress','已提交':'Submitted','已应用':'Applied','已读取':'Read','已选择':'Selected','满足':'Matched','按日期':'Date-aware','快速':'Rapid','最终':'Final','预报':'Predicted','其他':'Other','机构':'Provider','等级':'Level','完整性':'Completeness','每日':'Daily','质量':'Quality','空间':'Spatial','日期':'Date','范围内':'within range','至少':'at least','最大':'maximum','最小':'minimum','成功':'Success','处理中':'Processing','已取消':'Cancelled','取消':'Cancel','继续':'Resume','暂停':'Pause','开始':'Start','下载':'Download','站号':'Station ID','站号 Z–A':'Station Z–A'
  });
  Object.assign(enDict, {
    '版本':'Version','国际':'International','服务':'Service','磁暴':'Geomagnetic storm','告警等级':'Alert level','强':'Strong','弱':'Weak','很低':'Very low','很高':'Very high','高':'High','低':'Low','日值':'Daily value','日峰值':'Daily peak','日最低':'Daily minimum','行星':'planetary','说明':'Note','用法':'Usage','口径':'Definition','产品计算':'Product computation','产品规则':'Product rules','搜索产品':'Search products','搜索候选项':'Search candidates','搜索站网名称':'Search network names','点击选择':'Click to select','选择观测数据源':'Select observation data sources','机构选择支持':'Provider selection supports','常用分级':'common classification','全部分析中心/全部机构':'All analysis centers / providers','生产机构':'Provider','联合分析':'combined analysis','多系统':'Multi-GNSS','精密轨道':'Precise orbit','精密钟差':'Precise clock','参考组':'Reference group','指数':'Index','总相位周跳':'Total cycle slips','坐标残差':'Coordinate residuals','位置时间序列':'Position time series','完整率':'Completeness rate','完整性比例':'Completeness ratio','观测数':'Observation count','下载任务':'Download task','批量下载':'Batch download','空间筛选':'Spatial filter','国际 GNSS 服务':'International GNSS Service','欧洲航天局':'European Space Agency','欧洲定轨中心':'European orbit determination center','德国地学研究中心':'German Research Centre for Geosciences','德国航空航天中心':'German Aerospace Center','法国国家地理院':'French National Geographic Institute','俄罗斯信息分析中心':'Russian Information and Analysis Center','加拿大自然资源部':'Natural Resources Canada','美国国家大地测量局':'U.S. National Geodetic Survey','美国海军天文台':'U.S. Naval Observatory','喷气推进实验室':'Jet Propulsion Laboratory','日本多 GNSS 联合分析':'Japanese multi-GNSS joint analysis','日本宇宙航空研究开发机构':'Japan Aerospace Exploration Agency','上海天文台':'Shanghai Astronomical Observatory','中国科学院':'Chinese Academy of Sciences','华中科技大学':'Huazhong University of Science and Technology','加泰罗尼亚理工大学':'Polytechnic University of Catalonia','斯克里普斯海洋研究所':'Scripps Institution of Oceanography','武汉大学':'Wuhan University','麻省理工学院':'Massachusetts Institute of Technology','德国联邦制图与大地测量局':'German Federal Agency for Cartography and Geodesy','国际地球自转与参考系服务':'International Earth Rotation and Reference Systems Service','观测数据源':'observation data sources','候选网（区域组网）':'candidate network (regional)','空间天气筛选':'space-weather filter','关闭空间天气筛选':'Close space-weather filter','关闭测站详情':'Close station details','关闭自定义方案':'Close custom preset','范围：':'Range:','计划观测数':'planned observations','实际观测数':'actual observations','磁暴强度':'Geomagnetic storm intensity','太阳射电流量':'solar radio flux','保留范围':'Keep range','仅保留':'Keep only','排除':'Exclude','级':'level','单位：':'Units:','数据来自':'Data from'
  });
  Object.assign(enDict, {
    '正在读取本地知识库…':'Reading the local knowledge base…','查询预设决定按什么策略挑选测站':'The query preset controls how stations are selected','广播星历通用方案':'Broadcast ephemeris preset','选择方案或筛选条件后，点击“查询产品”应用当前条件。':'Choose a preset or filters, then click “Query products” to apply the current conditions.','完整性阈值':'Completeness threshold','阈值':'Threshold','浏览…':'Browse…','简写':'abbreviation','搜索国家简写，例如 CHN、USA、DEU':'Search country code, e.g. CHN, USA, DEU','更多站网':'More networks',
    '候选筛选':'Candidate filters','先筛选，再选站':'Filter, then select','查询与选站':'Query and station selection','选站条件':'Selection constraints','每日可用条件检查固定集合的每一天，允许单站缺测。':'Daily availability checks the fixed set on every date; individual stations may be missing.',
    '选站操作':'Selection actions','固定集合的测站数':'Fixed set size','0 表示不限制':'0 = unlimited','站数 ≥':'Count ≥','比例 ≥':'Rate ≥','不限制每日可用站数':'No daily minimum',
    '自定义精密产品方案':'Custom precise product plans','可创建并管理多套方案，每套方案都会保存在本机':'Create and manage multiple plans; each plan is saved locally','我的':'My','同一方案中的不同产品可以来自不同机构':'Products in one plan can come from different providers','保存后可继续“新建方案”，所有方案都会出现在上方“快速方案”列表中。':'After saving, create another plan; all plans appear in the “Quick presets” list above.','使用本地可用性索引、Site Log 与 IGS 日质量知识库':'Uses the local availability index, Site Log and the IGS daily-quality knowledge base',
    '按日期自动选择（IGS14 起）':'Date-aware automatic selection (from IGS14)','这是参考框架核心测站名册筛选，不是接收机质量筛选。选择具体框架后，日期范围仍会与该框架有效期进行提示':'This filters the reference-frame core station catalogue, not receiver quality. After a frame is selected, the date range is checked against its validity period','按日期自动选择”适合跨多个框架的查询。':'Date-aware automatic selection” is suitable for queries spanning multiple frames.','包含项需满足，排除项命中即移除，可同时选择多项':'Included items must match; excluded items are removed; multiple items can be selected','仅保留/排除均按这些机构在所选日期范围内实际有测站记录的':'Keep/exclude rules use station records actually available from these providers in the selected date range','没有测站记录的产品不会虚构测站。':'Products without station records do not fabricate stations.',
    'Kp 日峰值':'Daily Kp peak','NOAA 行星 K 指数，0–9':'NOAA planetary K index, 0–9','Dst 日最低':'Daily Dst minimum','磁暴强度，数值越负越强':'Storm intensity; more negative values indicate stronger storms','F10.7 日值':'Daily F10.7 value','10.7 cm 太阳射电流量':'10.7 cm solar radio flux','分别为':'respectively'
  });
  Object.assign(enDict, {
    '添加下载计划':'Add to download plan',
    '打开下载计划':'Open download plan',
    '关闭下载计划':'Close download plan',
    '查看下载计划':'View download plan',
    '观测数据下载计划':'Observation download plan',
    '精密产品下载计划':'Precise product download plan',
    '查询时可继续向计划中添加内容':'Keep adding items while querying',
    '查询时可继续向计划中添加测站':'Keep adding stations while querying',
    '查询时可继续向计划中添加产品':'Keep adding products while querying',
    '等待添加内容':'Waiting for items',
    '移除和开始下载只作用于勾选项目。':'Remove and Start download apply only to checked items.',
    '从查询结果中选择内容，然后点击“添加下载计划”':'Select results, then add them to the download plan',
    '下载中仍可关闭此窗口，继续查询并添加内容。':'Close this window to keep querying and adding items during downloads.',
    '下载计划为空':'Download plan is empty',
    '显示更多':'Show more',
    '移除':'Remove',
    '待下载':'Pending',
    '下载中':'Downloading',
    '已完成':'Completed',
    '进行中':'Running',
    '后续':'Next',
    '测站完整名称':'Full station name', '国家':'Country', '已收录设备历史':'Indexed equipment history',
    '稳定区间':'Stable intervals', '设备与观测码':'Equipment and observation codes',
    '正在读取测站信息…':'Loading station information…',
    '观测码':'Observation codes', '设备历史使用完整收录时间线；观测码按知识库能力区间展示。':'Equipment history uses the full indexed timeline; observation codes follow indexed capability intervals.',
    '各项相同的相邻区间已合并；未收录的信息显示未收录。':'Adjacent intervals with identical values are merged; missing metadata is shown as not indexed.',
    '未收录':'Not indexed'
  });
  Object.assign(enDict,{"CLK 使用的 ANTEX": "ANTEX referenced by CLK", "正在读取 CLK 头信息…": "Loading CLK header information…", "CLK 头信息尚未接入": "CLK header information is not connected yet", "CLK 头信息知识库尚未接入": "CLK header database is not connected yet", "CLK 头信息知识库暂不可读": "CLK header database is temporarily unavailable", "CLK 头信息表读取失败": "Unable to read CLK header records", "CLK 头信息读取未完成": "CLK header read did not finish", "CLK 产品族标识无法解析": "Unrecognized CLK product family", "按 CLK 文件头和注释收录；仅声明模型时无法确认历史文件版本。当前别名的内容可能随更新改变。": "Indexed from CLK headers and comments. A model name alone does not identify a historical file version. Current aliases may change when updated.", "已声明文件": "Declared filename", "仅声明模型": "Model only", "完整头未声明 ANTEX": "Complete header without an ANTEX declaration", "头读取失败": "Header read failed", "头读取待重试": "Header read needs retry", "尚未扫描": "Not scanned yet", "文件名声明": "Filename declaration", "模型声明，版本未确认": "Model declaration; version unconfirmed", "日期区间": "date intervals", "SYS / PCVS APPLIED 声明": "SYS / PCVS APPLIED declaration", "CLK 注释中的引用": "Reference in CLK comments", "范围完整性按相关 CLK 引用日期统计，非 ANTEX 文件内部有效期。": "Range completeness counts dates referencing this file in CLK, rather than validity dates inside ANTEX."});
  // Translate complete UI phrases first; short fragments in the older dictionary
  // can otherwise produce mixed-language text and change the layout.
  Object.assign(enDict, {
    '轨道 · 钟差 · 偏差 · 姿态 · 大气产品一站式下载':'Orbit · Clock · Bias · Attitude · Atmospheric products',
    '产品 ID、获取方式与范围完整性会显示在这里':'Product ID, access and coverage appear here',
    '等待本地索引':'Waiting for local index',
    '按起止 UTC 日期读取本地知识库，并按日期、完整性与测站条件筛选。':'Read the local index for the UTC date range, then apply date, coverage and station filters.',
    '分析中心':'Center', '范围完整性':'Coverage',
    '选择国家/地区':'Choose area', '全部国家/地区':'All areas',
    '最小':'Min', '最大':'Max',
    '运行提示、筛选结果和下载状态':'Query and download activity',
    '运行信息将在这里显示':'Activity appears here',
    '高级测站筛选':'Advanced station filters',
    '最低完整率 (%)':'Minimum coverage (%)',
    '每日最低比例 (%)':'Minimum daily ratio (%)',
    '最低每日观测完整率 (%)':'Minimum daily completeness (%)',
    '最大平均延迟 (h)':'Maximum mean latency (h)',
    '最大平均去趋势位移 (mm)':'Maximum mean detrended displacement (mm)',
    '最大平均绝对残差 (mm)':'Maximum mean absolute residual (mm)',
    '5 · RMS 多路径':'5 · RMS multipath',
    '最大平均多路径 (cm)':'Maximum mean multipath (cm)',
    '最大平均每日周跳 (次/日)':'Maximum mean daily cycle slips',
    '请输入名称或 ISO 三字母地区代码':'Enter a name or three-letter ISO area code',
    'ERP / EOP / 地球自转参数':'ERP / EOP / Earth rotation',
    'OSB / 观测量偏差':'OSB / observation bias',
    '任意两个所选测站之间的最小距离；0 表示不限制':'Minimum distance between any two selected stations; 0 means unlimited.',
    '限制所选测站形成一个连通网络，适用于区域组网；全球 GIM 评估建议设为 0':'Connects the selected stations as a regional network; use 0 for global GIM evaluation.',
    '下载时按所选日期解析实际文件名与可用镜像':'The file name and available mirror are resolved for the selected date at download time.'
  });
  Object.assign(enDict,{"下载文件": "Downloaded files", "保存方式": "Folder layout", "按日期分文件夹（年／年积日）": "By date (year / day of year)", "统一放在保存目录": "All files in the save folder", "下载后询问是否解压与 RINEX 转换": "Ask to extract and convert RINEX after downloading", "仅处理观测数据：解压，将 d／crx 转为 o／rnx，成功后删除压缩包和中间文件。开始前检查磁盘空间。": "For observations: extract files and convert d/crx to o/rnx. Remove archives and intermediate files after success. Check disk space first.", "设置对后续开始的下载任务生效。": "Applies to downloads started after this change.", "下载计划为空": "Empty plan", "当前分类暂无信息": "No messages in this category", "信息已复制": "Messages copied", "当前环境无法访问剪贴板": "Clipboard access is unavailable", "正在处理任务": "Processing", "正在后处理": "Postprocessing", "下载进行中": "Downloading", "待下载": "Pending", "继续": "Resume", "已选": "Selected", "个产品": "products", "个测站": "stations", "下载已暂停。": "Download paused.", "已取消下载。": "Download cancelled.", "查询失败；请检查网络、日期或稍后重试。": "Query failed. Check the network and dates, then retry.", "已继续观测文件下载。下载线程正在恢复。…": "Observation download resumed. Workers are restarting…", "正在安全取消解压与 RINEX 转换…": "Stopping extraction and RINEX conversion safely…", "下载已结束，正在估算解压与 RINEX 转换所需空间…": "Download finished. Estimating space for extraction and RINEX conversion…", "已取消后处理，下载文件保留。": "Postprocessing cancelled; downloaded files kept.", "已跳过后处理，下载文件保留。": "Postprocessing skipped; downloaded files kept.", "本次文件已是标准 RINEX，无需解压或转换。": "Files are already standard RINEX; no extraction or conversion is needed.", "正在解压与 RINEX 转换…": "Extracting files and converting RINEX…", "请选择保存目录。": "Choose a save folder.", "请选择有效的起止日期。": "Choose valid start and end dates.", "PRIDE PPP-AR": "PRIDE PPP-AR", "整套完整": "Complete bundle", "清单显示各产品完整率，可下载已找到的文件。": "The list shows coverage; available files can be downloaded.", "详见清单完整率": "See coverage in the product list", "高速精密摘要读取失败": "Unable to read the precise search index", "状态": "Status", "无法生成": "Unable to generate", "原因": "Reason", "范围": "Range", "方案": "Preset", "缺口": "Gaps", "查询完成": "Query complete", "精密产品查询完成": "Precise product query complete", "读取下载路线": "Reading download routes", "正在读取下载路线": "Reading download routes", "生成下载清单": "Preparing download list", "下载清单提示": "Download list notice", "测站详细信息读取失败": "Unable to read station details", "无法生成已选测站下载清单": "Unable to prepare the download list for selected stations", "无法按每日设备履历复核下载清单": "Unable to verify the download list against daily equipment history", "下载任务已取消": "Download cancelled", "任务已完成": "Task completed", "下载已取消": "Download cancelled", "下载结束": "Download finished", "下载完成": "Download complete", "已存在": "Already exists", "下载失败": "Download failed", "下载成功": "Downloaded", "已完成": "Completed", "已取消": "Cancelled", "已跳过": "Skipped", "失败": "Failed", "成功": "Succeeded", "无法": "Unable to", "缺少": "Missing", "不完整": "Incomplete", "剩余": "Remaining", "已下载": "Downloaded", "待核实": "Unverified", "待处理": "Pending", "待重试": "Retry pending", "按日期解析": "Resolve by date", "未找到": "Not found", "已保存": "Saved", "仅跳过": "Only skipped", "本轮": "This pass", "当前": "Current", "暂停下载": "Pause download", "取消下载": "Cancel download", "正在取消…": "Cancelling…", "正在下载": "Downloading", "正在查询": "Querying", "正在生成": "Preparing", "仅完成已保存的文件": "Only saved files completed", "可继续查询其他测站或产品，并添加到下载计划。": "You can query more stations or products and add them to the plan.", "尚未完成": "Not complete", "尚未扫描": "Not scanned", "读取失败": "Read failed", "连接失败": "Connection failed", "文件": "File", "总计": "Total", "已有": "Existing", "耗时": "Elapsed", "已检查": "Checked", "下载清单": "Download list", "使用所选机构在整个查询日期范围内各款 GIM 产品的已收录测站并集": "Use indexed stations from all GIM products of the selected providers across the entire date range", "不区分产品等级或具体文件；期间任一天被使用即纳入，不逐日匹配观测日期。只有数量、没有测站标识的产品不贡献测站。": "No product level or individual file is selected. A station used on any date in the range is included; observation dates are not matched day by day. Count-only products do not add station identifiers."});

  Object.assign(enDict,{"任务进行中，结束后可更改设置。": "Settings can be changed after the current task finishes.", "观测文件下载已暂停；可点击“继续下载”恢复，也可点击“取消下载”结束本次任务。": "Observation download paused. Use Resume to continue or Cancel to end this task.", "正在取消下载…正在安全结束清单生成或传输任务。": "Cancelling safely: stopping list preparation or file transfer…", "所选测站在这个日期范围内没有符合条件的下载文件。": "No matching files for the selected stations in this date range.", "等待自动并发下载名额时已取消。": "Cancelled while waiting for a download slot.", "所选范围超过 10 万个日文件；请缩短范围或分批添加下载计划。": "More than 100,000 daily files. Shorten the range or add items in batches.", "内存不足，未能加入下载计划；请缩短范围或减少测站。": "Not enough memory to add these items. Shorten the range or select fewer stations.", "内存不足，未能加入下载计划；请缩短日期范围或减少产品。": "Not enough memory to add these items. Shorten the range or select fewer products.", "下载计划超过 25 万个产品日期；请缩短范围或分批添加。": "More than 250,000 product dates. Shorten the range or add items in batches.", "方案兼容性元数据待核对，不能直接加入下载计划。": "Preset compatibility needs verification before adding it to the download plan.", "下载清单已就绪，正在开始下载…": "Download list ready; starting downloads…", "正在读取已选日期的下载路线": "Reading download routes for the selected dates", "下载清单已就绪，自动并发最多 12 线程，按服务器动态调节": "Download list ready; up to 12 parallel transfers, adjusted for each server", "下载路线包含无效文件名或地址": "Download route contains an invalid filename or address", "等待下载任务。": "Waiting for a download task.", "下载任务已取消，等待重新开始。": "Download cancelled; ready to start again.", "正在取消精密产品下载…": "Cancelling precise product downloads…", "正在取消精密产品下载准备…": "Cancelling precise product download preparation…", "已继续精密产品下载。…": "Precise product download resumed…", "精密产品下载已暂停；点击“继续下载”恢复。…": "Precise product download paused. Use Resume to continue…", "快速方案请求状态已失效，请重新查询。": "Preset request is no longer valid. Query again.", "正在读取快速方案的实际下载路线…": "Reading download routes for the preset…", "已清除快速下载方案。请按需要查询本地知识库。": "Preset cleared. Query the local index with your preferred filters.", "快速方案未启用；可继续使用下方日期、类型、等级和机构筛选。": "No preset enabled. Use the date, type, level and provider filters below.", "ANTEX 索引中没有适用的 IGS GLOBAL 卫星/接收机标准校准文件。": "No applicable IGS GLOBAL satellite/receiver standard calibration file in the ANTEX index.", "查询范围完整": "Full coverage in the selected range", "部分日期有文件，可下载已找到的文件": "Files are available on some dates; available files can be downloaded", "清单显示各产品完整率；兼容性待核对，不能直接下载。": "The list shows coverage. Verify compatibility before downloading.", "清单显示各产品完整率；没有找到可下载的文件。": "The list shows coverage. No downloadable files were found.", "未找到对应的数据源。": "No matching data source found.", "已切换到普通观测查询方案。请按需选择站网和筛选条件。": "Standard observation query selected. Choose networks and filters.", "测站详细信息正在加载，完成后可生成或检查选站方案。": "Station details are loading. Station selection can be generated or checked afterward.", "正在后台生成选站方案，逐日可用性与空间约束将一起检查…": "Generating station selection; checking daily availability and spatial constraints…", "正在检查当前所选测站的逐日可用性…": "Checking daily availability for the selected stations…", "查询条件或所选测站已变化，本次选站结果已作废，请重新生成或检查。": "Filters or station selection changed. Generate or check the selection again.", "测站查询进行中；请先等待查询完成，再切换页面。": "Station query in progress. Wait for completion before switching pages.", "已应用选站方案": "Station selection applied", "当前选站检查通过": "Station selection check passed", "准备中": "Preparing", "所选范围没有可用下载路线": "No download routes in the selected range", "产品族路线读取失败": "Unable to read product-family routes", "下载路线读取失败": "Unable to read download routes", "产品族部分日期没有可用下载路线": "Some product-family dates have no download routes", "个已选日期没有可用下载路线": "selected dates have no download routes", "索引提示": "Index notice", "日期范围": "Date range", "已存在跳过": "Skipped existing", "已完成的文件保留在": "Completed files kept in", "统一保存到": "Saved in", "保存到": "Saved in", "下各年/DOY目录": "in year/day-of-year folders", "服务器未提供总大小，请继续等待": "Server did not provide total size; please wait", "后处理": "Postprocessing", "因空间不足暂停": "Paused: not enough disk space", "解压": "Extracted", "转换": "Converted", "跳过": "Skipped", "保留压缩文件": "Archives kept", "日文件": "daily files", "准备下载": "Preparing downloads", "条": "entries", "项": "items", "尚未设置保存目录": "Save folder not set", "没有测站记录的产品不会虚构测站。": "Products without station records do not add inferred stations. "});
  Object.assign(enDict,{"所选机构在查询期间的各款 GIM 产品合并计算；不区分最终、快速等等级。不按每日观测日期匹配。仅使用已收录的测站标识；只有数量的产品不参与。": "Combine indexed station lists from all GIM products of the selected providers in the query range, including final and rapid products. Stations are not matched to observation dates day by day. Products that provide only a station count are excluded."});
  // Translate complete runtime messages before display clipping.
  Object.assign(enDict, {
  "高级筛选未应用：请检查可用性与观测质量阈值。": "Advanced filters were not applied. Check the availability and observation-quality thresholds.",
  "比例应为0–100，其余六类质量上限不得为负数。": "Percentages must be 0–100; all six quality upper limits must be nonnegative.",
  "高级测站筛选已应用，正在按新条件刷新结果…": "Advanced station filters applied. Refreshing results…",
  "日期高级筛选未应用：请检查 Kp、Dst 和 F10.7 阈值。": "Date filters were not applied. Check the Kp, Dst and F10.7 thresholds.",
  "日期高级筛选已应用，正在按空间天气条件刷新结果…": "Date filters applied. Refreshing results using space-weather conditions…",
  "正在补充 Site Log 元数据；请稍候再切换页面。": "Loading Site Log metadata. Wait before switching pages.",
  "高级筛选已应用，正在重新读取本地日期索引并按新条件筛选…": "Advanced filters applied. Reading the local date index and refreshing results…",
  "高级筛选已保存；下一次查询会使用这些条件。": "Advanced filters saved. They will apply to the next query.",
  "日期高级筛选已应用，正在按空间天气指标重新计算保留日期和测站…": "Date filters applied. Recalculating retained dates and stations using space-weather indices…",
  "日期高级筛选已保存；下一次查询会按空间天气指标筛选日期。": "Date filters saved. The next query will filter dates using space-weather indices.",
  "已保留你手动调整的站网选择；本次全选/取消全选已跳过。": "Your manually adjusted network selection was retained; select-all/deselect-all was skipped.",
  "已应用 GIM/TEC 全球电离层建模预设：IGS + MGEX、全球范围、30S、GPS、单站完整性不限制、每日覆盖至少245个测站，目标 250 个测站；最小站间距和最大相邻距离不限制，RINEX、接收机、天线和频点不限制。点击“开始查询”后将自动按全球均匀规则选择最多250个测站。": "GIM/TEC global ionosphere modelling preset applied: IGS + MGEX, global scope, 30S, GPS, unrestricted station completeness, at least 245 available stations daily and a target of 250 stations. Spacing, neighbor distance, RINEX, receiver, antenna and frequencies are unrestricted. Click Query stations to select up to 250 stations with an even global distribution.",
  "已应用 GIM 评估预设：选择全部本地可用观测数据源，GPS、30S、RINEX 3/4，单站日期范围完整率至少90%，选站集合每日已确认可用站不少于35；排除八个机构在当前日期范围内用于 GIM 建模的测站，按至少50 km间距全球均匀选择固定50站。点击“高级测站筛选”可查看或调整机构范围。": "GIM evaluation preset applied: all locally available observation sources, GPS, 30S, RINEX 3/4, at least 90% station completeness across the date range and at least 35 confirmed available stations daily. Exclude stations used by the eight GIM providers during this date range and select a fixed global set of 50 stations with spacing of at least 50 km. Open Advanced station filters to inspect or change the providers.",
  "已应用多系统 POD 预设：IGS + MGEX、全球范围、30S、RINEX 3/4、GPS + GLONASS + Galileo + BDS、每日观测完整率至少95%，目标60站；查询完成后将自动按全球均匀规则选站。30S文件可在POD处理中降采样到300S。": "Multi-GNSS POD preset applied: IGS + MGEX, global scope, 30S, RINEX 3/4, GPS + GLONASS + Galileo + BDS, at least 95% observation completeness on every query date and a target of 60 stations. Stations will be selected with an even global distribution after the query. The 30S files can be downsampled to 300S during POD processing.",
  "已应用 Network RTK 候选网预设：全部本地数据源、30S、RINEX 3/4、GPS + GLONASS，最大邻站距离75 km。请先用国家、经纬度或地图框选限定服务区域，并填写目标数量；查询后点击“均匀选站”。本预设使用30秒历史观测文件筛选候选测站，不代表已验证NTRIP、RTCM、实时在线率、数据延迟或站址环境。": "Network RTK candidate preset applied: all local sources, 30S, RINEX 3/4, GPS + GLONASS and a maximum neighbor distance of 75 km. Specify the service area using countries, coordinates or map selection and enter a target count; click Even spread after querying. This preset selects candidates from historical 30-second observation files. NTRIP, RTCM, real-time availability, latency and site conditions have not been verified.",
  "已应用 IGS/ITRF 参考框架方案：按日期自动选择 IGS14、IGb14、IGS20、IGb20 或 IGc20，官方核心测站、30S、RINEX 3；点击“开始查询”查看可用观测日期。": "IGS/ITRF reference-frame preset applied: automatically choose IGS14, IGb14, IGS20, IGb20 or IGc20 by date; official core stations, 30S and RINEX 3. Click Query stations to view available observation dates.",
  "正在读取起止日期内的本地知识库，并按日期、完整性与测站条件计算结果。超出知识库的日期沿用最后已知信息，下载时在线确认。": "Reading the local index for the selected date range and applying date, completeness and station filters. Dates beyond the index use the last known information; files are verified online when downloaded.",
  "Site Log 正在加载；元数据筛选完成后才能全选当前列表。": "Site Log is loading. Select-all will be available after metadata filtering finishes.",
  "所选范围超过 10 万个日文件；请缩短范围或分批添加下载计划。": "The selection exceeds 100,000 daily files. Shorten the range or add items to the download plan in batches.",
  "内存不足，未能加入下载计划；请缩短范围或减少测站。": "Insufficient memory to add items to the download plan. Shorten the range or select fewer stations.",
  "正在取消下载…正在安全结束清单生成或传输任务。": "Cancelling download… Safely stopping list generation or transfers.",
  "正在检查文件和剩余磁盘空间…": "Checking files and available disk space…",
  "请选择日期和筛选条件，然后点击“查询产品”。": "Select dates and filters, then click Query products.",
  "精密产品查询已取消。": "Precise product query cancelled.",
  "快速方案后台查询未返回结果。": "The background preset query returned no results.",
  "IGS 每日 SINEX、CODE Final GIM、VMF1/VMF3 OP 四时次、IGS 每日 RINEX 3 BRDC；缺口会按日期区间显示在运行窗口。": "IGS daily SINEX, CODE Final GIM, VMF1/VMF3 OP at four epochs and IGS daily RINEX 3 BRDC; gaps are shown by date range in the activity log.",
  "已应用 PRIDE PPP-AR 方案；正在核对 WUM RAPID 与辅助产品…": "PRIDE PPP-AR preset applied. Checking WUM RAPID and supporting products…",
  "标准 PPP：IGS Final SP3 + CLK + ERP/EOP + ANTEX；适合浮点 PPP，Bias/ORBEX 等可按需另行选择。": "Standard PPP: IGS Final SP3 + CLK + ERP/EOP + ANTEX; suitable for float PPP. Select Bias/ORBEX and other products separately as needed.",
  "已应用标准 IGS Final PPP 方案；正在查询 IGS Final SP3 起始产品…": "Standard IGS Final PPP preset applied. Querying the initial IGS Final SP3 products…",
  "UPD PPP-AR 兼容模式：SP3 + CLK + 对应分析中心 UPD + ERP + ANTEX；UPD 不能与其他机构的钟差/偏差混用。": "UPD PPP-AR compatibility mode: SP3 + CLK + matching analysis-center UPD + ERP + ANTEX. UPD must not be mixed with clock or bias products from other providers.",
  "已应用 UPD PPP-AR 兼容方案；请按分析中心检查 UPD 与轨道/钟差是否成套。": "UPD PPP-AR compatibility preset applied. Check that UPD, orbit and clock products form a matching bundle from the same analysis center.",
  "SP3、CLK 与 OSB 必须属于同一分析中心和产品线，不跨机构拼接。": "SP3, CLK and OSB must belong to the same analysis center and product line; products from different providers must not be combined.",
  "已应用 OSB PPP-AR 同机构方案，正在核对产品族。": "Same-center OSB PPP-AR preset applied. Checking product families.",
  "PPP-RTK / 网络 RTK 通用精密产品：SP3 + CLK + OSB + ERP + ANTEX；区域大气改正和实时播发不属于本地精密文件库，需由用户的服务端另行提供。": "PPP-RTK / network RTK core precise products: SP3 + CLK + OSB + ERP + ANTEX. Regional atmospheric corrections and real-time broadcasting are outside the local precise-file index and must be supplied by your server.",
  "已应用 PPP-RTK / 网络 RTK 通用精密产品方案。": "PPP-RTK / network RTK core precise-product preset applied.",
  "通用 PPP 智能优选：自动选择一套完整的 SP3 轨道、CLK 钟差、ERP 和 ANTEX；轨道与钟差保持同机构同产品线，优先完整覆盖，再选 Final 和高采样率钟差。": "Automatic PPP: select a complete SP3 orbit, CLK clock, ERP and ANTEX bundle. Orbit and clock products stay within the same analysis center and product line. Prefer complete coverage, then Final products and high-rate clocks.",
  "已应用通用 PPP 智能优选方案，正在比较分析中心与产品族。": "Automatic PPP preset applied. Comparing analysis centers and product families.",
  "方案兼容性元数据待核对，不能直接加入下载计划。": "Preset compatibility metadata needs verification before items can be added to the download plan.",
  "下载计划超过 25 万个产品日期；请缩短范围或分批添加。": "The download plan exceeds 250,000 product dates. Shorten the range or add items in batches.",
  "内存不足，未能加入下载计划；请缩短日期范围或减少产品。": "Insufficient memory to add items to the download plan. Shorten the date range or select fewer products.",
  "CLK 头信息知识库尚未接入": "CLK header information is not connected.",
  "CLK 头信息知识库暂不可读": "CLK header information is temporarily unreadable.",
  "CLK 头信息表读取失败": "Unable to read the CLK header table.",
  "CLK 头信息读取未完成": "CLK header information could not be fully read.",
  "文件损坏。": "The file is corrupted.",
  "文件损坏": "The file is corrupted",
  "网络错误。": "Network error.",
  "网络错误": "Network error",
  "网络连接失败。": "Network connection failed.",
  "路径不存在。": "The path does not exist.",
  "没有可用的下载路线。": "No download routes are available.",
  "目标测站数量应为 1 至 100000 的整数。": "The target station count must be an integer from 1 to 100000.",
  "站间距离应为 0 至 20050 km；0 表示不限制。": "Station spacing must be 0–20050 km; 0 means unrestricted.",
  "最小站间距不能大于区域组网的最大连边距离。": "Minimum station spacing cannot exceed the regional network's maximum link distance.",
  "随机种子应为大于 0 的整数。": "The random seed must be a positive integer.",
  "请先查询候选测站。": "Query candidate stations first.",
  "时间范围或数据源已变化，请先重新查询候选测站。": "The date range or data sources have changed. Query candidate stations again.",
  "每日最低可用站数不能大于目标测站数量。": "The daily minimum available count cannot exceed the target station count.",
  "选站计算未完成，请缩短日期范围或减少候选数量后重试。": "Station selection could not finish. Shorten the date range or reduce the candidate count and try again.",
  "所选测站在这个日期范围内没有符合条件的下载文件。": "The selected stations have no matching files in this date range.",
  "等待自动并发下载名额时已取消。": "Cancelled while waiting for an automatic download slot.",
  "本批任务中的所有 IGS 镜像均已检测为不可用。": "All IGS mirrors tested for this batch are unavailable.",
  "失败文件保留原件。": "Originals of failed files were retained.",
  "已存在，跳过": "Already exists; skipped",
  "已下载": "Downloaded",
  "OSB 未混入此结果。": "OSB products are excluded from these results.",
  "DCB、DSB、ABS 均未混入。": "DCB, DSB and ABS products are excluded.",
  "Bias/TRO-SINEX 未混入。": "Bias/TRO-SINEX products are excluded.",
  "已继续精密产品下载。下载线程正在恢复。…": "Precise product downloads resumed. Restarting download workers…",
  "精密产品下载已暂停；可点击“继续下载”恢复，也可点击“取消下载”结束本次任务。": "Precise product downloads paused. Click Resume download to continue or Cancel download to end this task.",
  "精密产品下载已暂停；点击“继续下载”恢复。…": "Precise product downloads paused. Click Resume download to continue…"
});

  Object.assign(enDict, {
  "已继续精密产品下载。…": "Precise product downloads resumed…",
  "当前其他类别仍使用本地文件名规则生成候选；SP3、CLK、ERP/EOP、DCB、OSB、IONEX、TRO、SINEX 与广播星历只读各自的本地知识库。": "Other categories still generate candidates using local filename rules. SP3, CLK, ERP/EOP, DCB, OSB, IONEX, TRO, SINEX and broadcast ephemeris each read their own local indexes.",
  "（未扫描或待复扫）": "(not scanned or rescan pending)"
});
  Object.assign(enDict, {
  "Dst 分级参考 · GB/T 31160—2014《地磁暴强度等级》": "Dst classification reference · GB/T 31160—2014: Classification of geomagnetic storm intensity",
  "−50 < Dst ≤ −30 小 · −100 < Dst ≤ −50 中等 · −200 < Dst ≤ −100 大 · −300 < Dst ≤ −200 特大 · Dst ≤ −300 超大（nT）": "−50 < Dst ≤ −30 Small · −100 < Dst ≤ −50 Moderate · −200 < Dst ≤ −100 Large · −300 < Dst ≤ −200 Very large · Dst ≤ −300 Extreme (nT)",
  "标准按磁暴主相期间的最低值分级，此处以日最低值作为筛选参考；Dst > −30 不代表所有地磁活动均平静。": "The standard uses the minimum Dst during the storm main phase. The daily minimum is used here as a filtering reference. Dst > −30 alone does not establish quiet conditions for all geomagnetic activity.",
  "F10.7 分级参考 · QX/T 135—2011《太阳活动水平分级》": "F10.7 classification reference · QX/T 135—2011: Classification for solar activity level",
  "F10.7 < 80 很低 · 80 ≤ F10.7 < 100 低 · 100 ≤ F10.7 < 150 中 · 150 ≤ F10.7 < 200 高 · F10.7 ≥ 200 很高（sfu）": "F10.7 < 80 Very low · 80 ≤ F10.7 < 100 Low · 100 ≤ F10.7 < 150 Medium · 150 ≤ F10.7 < 200 High · F10.7 ≥ 200 Very high (sfu)",
  "标准使用订正到 1 AU 的值，本地筛选使用观测值；此表作为分级参考，具体阈值由研究要求确定。": "The standard uses values adjusted to 1 AU. Local filters use observed values. These bands are a classification reference. Choose thresholds according to research requirements.",
  "10.7 cm 太阳射电通量（观测值）": "10.7 cm solar radio flux (observed)"
});
  Object.assign(enDict, {
  "F10.7 数据口径": "F10.7 data definition",
  "1 AU 订正值（行业标准）": "Adjusted to 1 AU (industry standard)",
  "观测值（电离层与大气研究）": "Observed (ionosphere and atmosphere)",
  "10.7 cm 太阳射电通量（1 AU 订正值）": "10.7 cm solar radio flux (adjusted to 1 AU)",
  "标准采用 1 AU 订正值，与当前筛选口径一致；具体阈值由研究要求确定。": "The standard uses values adjusted to 1 AU, matching the selected data definition. Choose thresholds according to research requirements.",
  "当前为观测值，GFZ 建议用于电离层和大气研究；使用本标准分级时请切换为 1 AU 订正值。": "Observed values are selected. GFZ recommends observed flux for ionospheric and atmospheric studies. Select values adjusted to 1 AU when applying this classification."
});
  Object.assign(enDict, {
  "Dst（nT）：": "Dst (nT): ",
  "F10.7（sfu）：": "F10.7 (sfu): ",
  "−50 < Dst ≤ −30 小 · −100 < Dst ≤ −50 中等 · −200 < Dst ≤ −100 大 · −300 < Dst ≤ −200 特大 · Dst ≤ −300 超大": "−50 < Dst ≤ −30 Small · −100 < Dst ≤ −50 Moderate · −200 < Dst ≤ −100 Large · −300 < Dst ≤ −200 Very large · Dst ≤ −300 Extreme",
  "F10.7 < 80 很低 · 80 ≤ F10.7 < 100 低 · 100 ≤ F10.7 < 150 中 · 150 ≤ F10.7 < 200 高 · F10.7 ≥ 200 很高": "F10.7 < 80 Very low · 80 ≤ F10.7 < 100 Low · 100 ≤ F10.7 < 150 Medium · 150 ≤ F10.7 < 200 High · F10.7 ≥ 200 Very high",
  "F10.7 口径": "F10.7 definition",
  "1 AU 订正值": "Adjusted to 1 AU",
  "观测值": "Observed",
  "10.7 cm 太阳射电通量": "10.7 cm solar radio flux"
});
  Object.assign(enDict, {
  "精密产品索引缺失或为空，请重新安装官方知识库后重试。": "The precise-product index is missing or empty. Reinstall the official knowledge bases, then retry.",
  "精密产品索引与知识库不匹配，请重新安装官方知识库后重试。": "The precise-product index does not match the knowledge bases. Reinstall the official knowledge bases, then retry.",
  "观测数据索引不可用，请重新安装官方知识库后重试。": "The observation index is unavailable. Reinstall the official knowledge bases, then retry.",
  "请重新安装官方知识库后重试。": "Reinstall the official knowledge bases, then retry.",
  "请调整查询日期范围。": "Adjust the query date range.",
  "请调整所选站网。": "Adjust the selected networks.",
  "可尝试其他已收录的数据源。": "Try another indexed data source.",
  "当前查询继续使用上一次完整索引；请重新安装官方知识库后重试。": "This query uses the last complete index. Reinstall the official knowledge bases, then retry.",
  "中央高速观测索引结构不完整，": "The observation index structure is incomplete. ",
  "中央高速观测索引版本不兼容，": "The observation index version is incompatible. ",
  "高速索引测站编号不连续，": "Station identifiers in the observation index are not consecutive. ",
  "高速索引测站别名编号无效，": "A station alias identifier in the observation index is invalid. ",
  "高速索引档案编号无效，": "An archive identifier in the observation index is invalid. ",
  "高速索引位图损坏，": "The observation index bitmap is damaged. ",
  "完整日期位图损坏，": "The complete-date bitmap is damaged. ",
  "高速索引 Site Log 编号无效，": "A Site Log identifier in the observation index is invalid. ",
  "中央高速索引未覆盖所选站网掩码": "The observation index does not cover the selected network mask"
});
  Object.assign(enDict, {
  "IGS 本地知识库中的该日期是旧版或部分七源记录；界面已按本地记录显示，且没有访问网络。可尝试其他已收录的数据源。": "IGS coverage for this date is incomplete. Results use local records without network access. Try another indexed data source.",
  "IGS 本地知识库尚未收录该日期。界面查询不会自动联网；请调整查询日期范围。": "The IGS local index does not cover this date. This query did not access the network. Adjust the query date range.",
  "本地知识库尚未收录该日期；请调整查询日期范围。": "The local index does not cover this date. Adjust the query date range."
});
  function formatRuntimeEnglish(value) {
    const raw = String(value).trim().replace(/\\u2026/g, '…');
    const fileProgress = /^(\[\d+\/\d+\]\s*[^:：]+[:：]\s*)([\s\S]+)$/.exec(raw);
    if (fileProgress) return fileProgress[1] + localizeUiText(fileProgress[2]);
    let transfer = /^(\d{4}-\d{2}-\d{2})[：:]\s*正在从\s+(.+?)\s+下载$/.exec(raw);
    if (transfer) return `${transfer[1]}: downloading from ${transfer[2]}`;
    transfer = /^(\d{4}-\d{2}-\d{2})[：:]\s*尝试\s+(.+?)[：:]\s*(.*?)（自动并发\s*(\d+)\/(\d+)）$/.exec(raw);
    if (transfer) return `${transfer[1]}: trying ${transfer[2]}: ${transfer[3]} (${transfer[4]}/${transfer[5]} active transfers)`;
    transfer = /^(\d{4}-\d{2}-\d{2})[：:]\s*(处理完成|所有候选镜像均未成功)$/.exec(raw);
    if (transfer) return `${transfer[1]}: ${transfer[2] === '处理完成' ? 'completed' : 'all candidate mirrors failed'}`;
    const diagnostic = window.GnssRuntimeLog.translateDiagnostic(raw, localizeUiText);
    if (diagnostic !== null) return diagnostic;
    if (enDict[raw]) return enDict[raw];
    const family = name => name === '广播星历' ? 'Broadcast ephemeris' : name;
    const scan = name => name === '完整' ? 'fully indexed' : 'rescan pending';
    let m = /^(.*?)\s*(?:本地)?知识库(?:尚未收录 GPS 周 ([\d,]+)|尚未收录 ([\d,]+) 年|尚未收录这一天|尚未构建)。(?:请先运行 (\S+?)[；;])?本次查询没有联网。(?:数据库[:：](.*))?$/.exec(raw);
    if (m) return family(m[1].trim()) + ' local index ' + (m[2] ? 'does not cover GPS week ' + m[2] : m[3] ? 'does not cover year ' + m[3] : raw.includes('尚未构建') ? 'has not been built' : 'does not cover this date') + '. This query did not access the network. Adjust the query date range.' + (m[5] ? ' Database: ' + m[5] : '');
    m = /^(SP3|CLK|OSB) 查询来自本地知识库：GPS周 (\d+)，(\d+)\/(\d+) 个(?:数据中心|档案源)(完整|（待复扫）)，当天 (\d+) 个逻辑产品[。；;](.*)$/.exec(raw);
    if (m) return m[1] + ' local-index query: GPS week ' + m[2] + ', ' + m[3] + '/' + m[4] + ' centers ' + scan(m[5]) + '; ' + m[6] + ' products on this date. ' + localizeUiText(m[7]);
    m = /^ERP\/EOP 查询来自本地知识库：GPS周 (\d+) 的ERP档案 (\d+)\/(\d+)(完整|（未扫描或待复扫）)；当天 (\d+) 个ERP、(\d+) 个IERS EOP长序列。$/.exec(raw);
    if (m) return 'ERP/EOP local-index query: GPS week ' + m[1] + ', ' + m[2] + '/' + m[3] + ' ERP archives ' + scan(m[4]) + '; ' + m[5] + ' ERP products and ' + m[6] + ' IERS EOP long series on this date.';
    m = /^DCB 查询来自本地知识库：(\d+) 年，(\d+)\/(\d+) 个档案源(完整|（待复扫）)，当天 (\d+) 个逻辑产品；OSB 未混入此结果。$/.exec(raw);
    if (m) return 'DCB local-index query: year ' + m[1] + ', ' + m[2] + '/' + m[3] + ' archives ' + scan(m[4]) + '; ' + m[5] + ' products on this date. OSB products are excluded.';
    m = /^(IONEX|广播星历)查询来自本地知识库：(\d+)\/(\d+) 个档案(?:源|中心)(完整|（待复扫）)，当天 (\d+) 个逻辑(?: GIM\/ROTI )?产品。$/.exec(raw.replace('IONEX 查询', 'IONEX查询'));
    if (m) return family(m[1]) + ' local-index query: ' + m[2] + '/' + m[3] + ' archives ' + scan(m[4]) + '; ' + m[5] + (m[1] === 'IONEX' ? ' GIM/ROTI' : '') + ' products on this date.';
    m = /^SINEX 查询来自本地知识库：GPS周 (\d+)，(\d+)\/(\d+) 个档案源(完整|（待复扫）)，当天 (\d+) 个完整解、(\d+) 个坐标解；Bias\/TRO-SINEX 未混入。$/.exec(raw);
    if (m) return 'SINEX local-index query: GPS week ' + m[1] + ', ' + m[2] + '/' + m[3] + ' archives ' + scan(m[4]) + '; ' + m[5] + ' complete solutions and ' + m[6] + ' coordinate solutions on this date. Bias/TRO-SINEX products are excluded.';
    m = /^TRO 查询来自本地知识库：(\d+)\/(\d+) 个档案源(完整|（待复扫）)；当天 (\d+) 个IGS测站产品、(\d+) 个CODE\/EPN网络产品。$/.exec(raw);
    if (m) return 'TRO local-index query: ' + m[1] + '/' + m[2] + ' archives ' + scan(m[3]) + '; ' + m[4] + ' IGS station products and ' + m[5] + ' CODE/EPN network products on this date.';
    m = /^ANTEX 查询来自本地知识库：查询日期对应 GPS周 (\d+)，(\d+)\/(\d+) 个生产体系(完整|（待复扫）)，适用的天线模型 (\d+) 个。$/.exec(raw);
    if (m) return 'ANTEX local-index query: GPS week ' + m[1] + ', ' + m[2] + '/' + m[3] + ' production systems ' + scan(m[4]) + '; ' + m[5] + ' applicable antenna models.';
    m = /^正在后台读取精密产品知识库：(\S+) 至 (\S+)…$/.exec(raw);
    if (m) return 'Reading precise product indexes: ' + m[1] + ' to ' + m[2] + '…';
    m = /^自定义方案“(.+?)”：(\d+) 条产品\/等级\/机构\/采样间隔规则；查询结果只显示符合任一规则的产品。$/.exec(raw);
    if (m) return 'Custom preset "' + m[1] + '": ' + m[2] + ' product/level/provider/interval rules; results include products matching any rule.';
    m = /^(?:日期范围 (\S+) 至 (\S+) )?下载(已取消|结束|完成)：成功 (\d+)，已存在跳过 (\d+)，失败 (\d+)。(.*)$/.exec(raw);
    if (m) return 'Download ' + (m[3] === '已取消' ? 'cancelled' : 'finished') + (m[1] ? ': ' + m[1] + ' to ' + m[2] : '') + ': ' + m[4] + ' successful, ' + m[5] + ' existing files skipped, ' + m[6] + ' failed. ' + localizeUiText(m[7]);
    m = /^解压与 RINEX 转换(已取消|因空间不足暂停|完成)：成功 (\d+)，失败 (\d+)。(.*)$/.exec(raw);
    if (m) return 'Extraction and RINEX conversion ' + (m[1] === '已取消' ? 'cancelled' : m[1] === '因空间不足暂停' ? 'paused because disk space is insufficient' : 'finished') + ': ' + m[2] + ' successful, ' + m[3] + ' failed. ' + localizeUiText(m[4]);
    m = /^下载文件异常累计 (\d+) 条[；;]最近[:：](.*)$/.exec(raw);
    if (m) return 'File download errors: ' + m[1] + ' total; latest: ' + localizeUiText(m[2]);
    m = /^(已完成的文件保留在|统一保存到|保存到) (.*?)(?: 下各年\/DOY目录)?[。.]$/.exec(raw);
    if (m) return (m[1] === '已完成的文件保留在' ? 'Completed files were retained in ' : 'Saved to ') + m[2] + (raw.includes('下各年/DOY目录') ? ' in year/DOY folders' : '') + '.';
    m = /^(.*?)本地知识库读取失败[:：](.*)$/.exec(raw);
    if (m) return 'Unable to read the ' + family(m[1].trim()) + ' local index: ' + localizeUiText(m[2]);
    const errors = {'测站详细信息读取失败':'Unable to read station details', '无法生成已选测站下载清单':'Unable to prepare the selected-station download list', '无法按每日设备履历复核下载清单':'Unable to verify the list against daily equipment history', '下载清单提示':'Download-list notice', '无法打开知识库':'Unable to open the local index', '下载失败':'Download failed', '下载路线读取失败':'Unable to read download routes', '产品族路线读取失败':'Unable to read product-family routes'};
    m = /^([^：:]+)[:：]\s*([\s\S]*)$/.exec(raw);
    if (m && errors[m[1]]) return errors[m[1]] + ': ' + localizeUiText(m[2]);
    m = /^无法连接 (.+?)[。.]$/.exec(raw);
    if (m) return 'Unable to connect to ' + m[1] + '.';
    return null;
  }
  function formatNativeEnglish(raw) {
    let m = /^已选择\s+(.+?)\s+产品类型；点击“查询产品”读取本地知识库。$/.exec(raw);
    if (m) return `Product type selected: ${m[1] === 'all' ? 'All products' : preciseProductTypeLabel(m[1])}; click "Query products" to read the local index.`;
    m = /^全部精密产品查询范围\s+(\d{4}-\d{2}-\d{2})\s+至\s+(\d{4}-\d{2}-\d{2})：合并已安装的真实知识库，共\s+([\d,]+)\s+个逻辑产品；后台读取耗时\s+([\d.]+)\s+秒。$/.exec(raw);
    if (m) return `All precise products ${m[1]} to ${m[2]}: ${m[3]} products from installed local databases; loaded in ${m[4]} s.`;
    m = /^已自动选择\s*([\d,]+)\s*个具有本地知识库的站网；红色站网保持未选中。$/.exec(raw);
    if (m) return `${m[1]} indexed networks selected; unavailable networks left unchecked.`;
    m = /^已(选中|取消选择)\s*([\d,]+)\s*个具有可用本地知识库的站网；([\s\S]*)$/.exec(raw);
    if (m) return `${m[2]} indexed networks ${m[1] === '选中' ? 'selected' : 'deselected'}; unavailable networks left unchecked.`;
    m = /^查询完成：\s*(\d{4}-\d{2}-\d{2})\s+至\s+(\d{4}-\d{2}-\d{2})[；;]\s*([\d,]+)\s*个测站，地图显示\s*([\d,]+)\s*个[。.]?(?:\s*([\d,]+)\s*个暂缺坐标。)?$/.exec(raw);
    if (m) return `Query complete: ${m[1]} to ${m[2]}; ${m[3]} stations, ${m[4]} mapped${m[5] ? `; ${m[5]} without coordinates` : ''}.`;
    m = /^已将\s*([\d,]+)\s*个(测站|产品)添加到下载计划。$/.exec(raw);
    if (m) return `Added ${m[1]} ${m[2] === '测站' ? 'stations' : 'products'} to the download plan.`;
    m = /^已移除\s*([\d,]+)\s*(?:个|项)(?:测站|产品|下载计划)?[。.]?$/.exec(raw);
    if (m) return `Removed ${m[1]} items.`;
    m = /^已选\s*([\d,]+)\s*个(测站|产品)$/.exec(raw);
    if (m) return `${m[1]} ${m[2] === '测站' ? 'stations' : 'products'} selected`;
    m = /^待下载\s*([\d,]+)\s*(?:个|项)\s*(测站|产品)?$/.exec(raw);
    if (m) return `${m[1]} ${m[2] === '测站' ? 'stations' : m[2] === '产品' ? 'products' : 'items'} pending`;
    m = /^精密产品查询完成：\s*(\d{4}-\d{2}-\d{2})\s+至\s+(\d{4}-\d{2}-\d{2})；\s*([\d,]+)\s*个产品。$/.exec(raw);
    if (m) return `Precise product query complete: ${m[1]} to ${m[2]}; ${m[3]} products.`;

    m = /^已将\s*([\d,]+)\s*个精密产品添加到下载计划。$/.exec(raw);
    if (m) return `Added ${m[1]} precise products to the download plan.`;
    m = /^正在为 (\S+) 至 (\S+) 的 ([\d,]+) 个已选测站生成下载清单…$/.exec(raw);
    if (m) return `Preparing download list: ${m[1]} to ${m[2]}; ${m[3]} selected stations…`;
    m = /^准备下载 (?:(\S+) 至 (\S+) 的 )?([\d,]+) 个(日文件|文件)…$/.exec(raw);
    if (m) return `Preparing ${m[3]} ${m[4] === '日文件' ? 'daily files' : 'files'}${m[1] ? `: ${m[1]} to ${m[2]}` : ''}…`;
    m = /^生成下载清单：(.*?)\s+(\d+)\/(\d+) 个知识库，(\d+)\/(\d+) 个日期查询块，已找到 ([\d,]+) 个文件$/.exec(raw);
    if (m) return `Preparing list: ${m[1] === '准备中' ? 'starting' : m[1]}; ${m[2]}/${m[3]} indexes, ${m[4]}/${m[5]} date blocks, ${m[6]} files found`;
    m = /^生成下载清单：正在按日期复核 Site Log 设备履历\s+(\d+)\/(\d+)$/.exec(raw);
    if (m) return `Verifying Site Log equipment history: ${m[1]}/${m[2]}`;
    m = /^下载清单已生成，共 ([\d,]+) 个日文件；自动并发已启用（全局最多 (\d+)，按服务器独立调节）…$/.exec(raw);
    if (m) return `List ready: ${m[1]} daily files; up to ${m[2]} parallel transfers, adjusted for each server…`;
    m = /^正在生成下载清单：([\d,]+)\/([\d,]+) 个产品日期…$/.exec(raw);
    if (m) return `Preparing download list: ${m[1]}/${m[2]} product dates…`;
    m = /^正在读取下载路线（当前产品 (\d+)\/(\d+) 天）$/.exec(raw);
    if (m) return `Reading download routes: ${m[1]}/${m[2]} days for this product`;
    m = /^正在后台生成快速方案：(\S+) 至 (\S+)…$/.exec(raw);
    if (m) return `Preparing preset: ${m[1]} to ${m[2]}…`;
    m = /^正在下载 (.+?)(?: · 已选 (\d+) 项)?$/.exec(raw);
    if (m) return `Downloading ${m[1]}${m[2] ? ` · ${m[2]} selected` : ''}`;
    m = /^(已应用选站方案|当前选站检查通过)：([\d,]+) 个测站，([\d,]+) 个评估日期，每日最少 ([\d,]+) 个已确认可用站；最小站间距 ([\d.]+) km。$/.exec(raw);
    if (m) return `${m[1] === '已应用选站方案' ? 'Station selection applied' : 'Selection check passed'}: ${m[2]} stations, ${m[3]} dates; at least ${m[4]} available stations daily; minimum spacing ${m[5]} km.`;
    return null;
  }
  const uiPrefs = { language: 'en', theme: 'light' };
  function localizeUiText(value) {
    const raw = String(value ?? '');
    if (!raw || typeof enDict === 'undefined') return raw;
    const toEn = uiPrefs.language === 'en';
    const downloadLog = /^(.*?) (Download log: [\s\S]*|Download log could not be created in the save folder\.)$/.exec(raw);
    if (toEn && downloadLog) return `${localizeUiText(downloadLog[1])} ${downloadLog[2]}`;
    const selectedType = /^已选择\s+(.+?)\s+产品类型；点击“查询产品”读取本地知识库。$/.exec(raw);
    if (!toEn && selectedType) return `已选择 ${selectedType[1] === 'all' ? '全部产品' : preciseProductTypeLabel(selectedType[1])} 产品类型；点击“查询产品”读取本地知识库。`;
    // Native messages already contain their original Chinese. Preserve IDs,
    // tool names and user data instead of reverse-translating fragments.
    if (!toEn && /[\u4e00-\u9fff]/.test(raw)) return raw;
    if (toEn) {
      const completedProducts = /^精密产品处理完成：成功\s*(\d+)，已存在\s*(\d+)，失败\s*(\d+)。$/.exec(raw.trim());
      if (completedProducts) return `Precise product download finished: succeeded ${completedProducts[1]}, already present ${completedProducts[2]}, failed ${completedProducts[3]}.`;
      const vmfGrid = /^(VMF[13])\s+(\S+)\s+(\S+)\s+格网\s*[（(]([^）)]+)[）)]$/.exec(raw.trim());
      if (vmfGrid) return `${vmfGrid[1]} ${vmfGrid[2]} ${vmfGrid[3]} grid (${vmfGrid[4]})`;
      const runtimeEnglish = formatRuntimeEnglish(raw);
      if (runtimeEnglish !== null) return runtimeEnglish;
      const formatted = formatNativeEnglish(raw);
      if (formatted !== null) return formatted;
      let match = /^共\s*(\d+)\s*条运行信息$/.exec(raw);
      if (match) return `${match[1]} log${match[1] === '1' ? '' : 's'}`;
      match = /^(\d+)\s*个产品$/.exec(raw);
      if (match) return `${match[1]} products`;
      match = /^精密产品查询范围\s+(\d{4}-\d{2}-\d{2})\s+至\s+(\d{4}-\d{2}-\d{2})：\s*(\d+)\s*个逻辑产品，已检查\s*(\d+)\s*天；知识库有索引\s*(\d+)\s*天，待复扫\/不完整\s*(\d+)\s*天；读取耗时\s*([\d.]+)\s*秒。$/.exec(raw);
      if (match) return `Precise products ${match[1]}–${match[2]}: ${match[3]} products; ${match[4]} days checked (${match[5]} indexed, ${match[6]} incomplete); ${match[7]} s.`;
    }
    const dict = toEn ? enDict : zhDict;
    const exact = dict[raw.trim()] || (toEn ? enCanonical.get(canonicalUiKey(raw)) : undefined);
    if (exact) return raw.replace(raw.trim(), exact);
    const paths = [];
    let out = raw.replace(/(?:[A-Za-z]:[\\/]|(?:https?|ftp):\/\/)[^\s<>\"；。]+/g, path => {paths.push(path);return `\uE000${paths.length-1}\uE001`;});
    if (toEn) out = out.replace(/(\d+)\s*个(?![\u4e00-\u9fff])/g, '$1 items');
    const entries = toEn ? enEntries : zhEntries;
    for (const [from,to] of entries) if (out.includes(from)) out = out.split(from).join(to);
    if (toEn) out = out.replace(/ 缺 /g, ' missing ').replace(/另 (\d+) 段/g, '$1 more intervals').replace(/；\s*/g, '; ').replace(/：\s*/g, ': ').replace(/，\s*/g, ', ').replace(/。\s*/g, '. ');
    return out.replace(/\uE000(\d+)\uE001/g, (_, index) => paths[Number(index)]);
  }
  Object.assign(enDict, {'查询完成：':'Query complete: ', '个测站，地图显示':'stations; mapped', '个暂缺坐标。':'stations without coordinates.', '下载文件异常累计':'File download errors: ', '条；最近：':' total; latest: '});
  // The packaged HTML includes some full-width parentheses while newer UI
  // strings use ASCII parentheses.  Treat those as the same lookup key.
  const canonicalUiKey = value => String(value).trim().replace(/\s*[（(]\s*/g,'(').replace(/\s*[）)]\s*/g,')');
  Object.assign(enDict, {'条件':'Condition','完整率 ≥':'At least ≥','最低完整率 (%)':'Minimum (%)'});
  Object.assign(enDict, {'本发布版数据截至 2026-08-01（含当天）；各站网和产品的实际覆盖范围可能更早。':'Release data dates end on 2026-08-01 inclusive; some networks and products end earlier.'});
  Object.assign(enDict, {
    '单击查看测站详情':'Click to view station details',
    '展开查看':'Expand',
    '高速索引；下载时按日期读取下载源':'Download sources are resolved for the selected dates.',
    '高速 IONEX 索引；下载时按日期读取下载源':'Download sources are resolved for the selected dates.',
    '高速精密摘要；下载时按日期读取下载源':'Download sources are resolved for the selected dates.',
    'TU Wien 官方目录；下载时读取实有文件路线':'Files are resolved from the official TU Wien directory at download time.',
    '本地知识库：':'Local index: ',
    '当前别名':'Current alias',
    '个下载源':'download sources',
    '非实时服务':'not a live service',
    '地球旋转参数':'Earth rotation parameters',
    '地球定向参数':'Earth orientation parameters',
    '全球电离层图':'Global ionosphere map',
    'ROTI 扰动图':'ROTI disturbance map',
    '网络TRO-SINEX':'network TRO-SINEX',
    '网络对流层参数':'network troposphere parameters',
    'SINEX 完整解':'SINEX full solution',
    'SINEX 坐标解':'SINEX coordinate solution',
    '天线校准模型':'Antenna calibration model',
    '文件名与候选镜像已经解析':'Filename and candidate mirrors have been resolved'
  });
  const enCanonical = new Map(Object.entries(enDict).map(([key,value]) => [canonicalUiKey(key),value]));
  const zhDict = {}; for (const k in enDict) if (!(enDict[k] in zhDict)) zhDict[enDict[k]] = k;
  const sortedEntries = dict => Object.entries(dict).filter(([from,to]) => from && to).sort((a,b) => b[0].length - a[0].length);
  const enEntries = sortedEntries(enDict), zhEntries = sortedEntries(zhDict);
  function isDarkUi() { return uiPrefs.theme === 'dark'; }
  function loadUiPrefs() {
    try { const language = localStorage.getItem(uiLanguageKey);
      if (language === 'en' || language === 'zh') uiPrefs.language = language;
      const raw = JSON.parse(localStorage.getItem(uiPrefsKey) || '{}');
      if (raw.theme === 'dark' || raw.theme === 'light') uiPrefs.theme = raw.theme;
      // Keep appearance settings shared; each installation saves its language.
      if (Object.prototype.hasOwnProperty.call(raw, 'palette')) saveUiPrefs(); } catch (error) {}
  }
  function saveUiPrefs() { try { localStorage.setItem(uiPrefsKey, JSON.stringify(uiPrefs)); localStorage.setItem(uiLanguageKey, uiPrefs.language); } catch (error) {} }
  function applyTheme(theme) {
    uiPrefs.theme = theme;
    document.documentElement.setAttribute('data-theme', theme);
    document.querySelectorAll('#themeSwitch button').forEach(b => b.classList.toggle('active', b.dataset.themeValue === theme));
    if (typeof renderMapPoints === 'function') renderMapPoints();
  }
  function applyLanguage(lang) {
    uiPrefs.language = lang;
    post('ui-language', { language:lang });
    document.documentElement.setAttribute('lang', lang === 'en' ? 'en' : 'zh-CN');
    const mapSource = $('#mapSourceInfo');
    if (mapSource) {
      const originalTitle = mapSource.dataset.sourceTitle || (mapSource.dataset.sourceTitle = mapSource.title);
      mapSource.textContent = lang === 'en' ? enDict['离线参考底图'] : '离线参考底图';
      mapSource.title = lang === 'en' ? enDict[originalTitle] : originalTitle;
    }
    document.querySelectorAll('#languageSwitch button').forEach(b => b.classList.toggle('active', b.dataset.lang === lang));
    if (translateObserver) translateObserver.disconnect();
    renderQuickPresetOptions();
    renderCustomPresetLibrary();
    renderActiveFilters();
    translateDom(document.body);
    renderInfo();
    renderObservationRows();
    renderPreciseRows();
    if (state.latest.stations) {
      const count = state.rowsSelectedOnly
        ? Number(state.latest.rowTotal ?? state.latest.visible ?? 0)
        : Number(state.latest.visible || 0);
      setText('#resultSummary', lang === 'en'
        ? `${count} ${state.rowsSelectedOnly ? 'selected stations' : 'stations match'}`
        : `${count} ${state.rowsSelectedOnly ? '个已选测站' : '个测站满足条件'}`);
    }
    renderDownloadPlan();
    updateDownloadDock();
    if (translateObserver) translateObserver.observe(document.body, translateObserverOptions);
    // Country names and axis annotations are painted on a canvas rather than
    // represented by DOM text, so explicitly repaint them when the language
    // switch changes.
    scheduleMapDraw();
    const dateMaxYear=new Date().getFullYear()+1;
    window.MarkWeatherPreview.repaint();
    document.querySelectorAll('input[type="date"]').forEach(input=>{const minYear=input.min.slice(0,4);input.title=lang==='en'?`Range: ${minYear}–${dateMaxYear}`:`范围：${minYear} 年至 ${dateMaxYear} 年`;});
    if (!$('#countryPickerLayer')?.classList.contains('hidden')) renderCountryPicker();
    if(state.stationDetailData) {
      const row=findObservation(state.detailStation);
      if(row)$('#stationDetailBody').innerHTML=stationDetailMarkup(row);
      renderCoverage($('#stationDateCoverage'),state.stationDetailData.coverage);
    }
    if(state.productCoverage)renderClkAntex(state.productCoverage.antex);
    if(state.productCoverage) renderCoverage($('#productDateCoverage'),state.productCoverage.coverage);
    syncSortHeader('#sortStationCompleteness',state.sortColumn==='completeness'?(state.ascending?'asc':'desc'):null);
    syncSortHeader('#sortPreciseCompleteness',state.preciseSortDirection);
    translateDom(document.body);
  }
  const enAttr = { '设置':'Settings','放大':'Zoom in','缩小':'Zoom out','恢复全球视图':'Reset view','全屏地图':'Fullscreen map',
    '复制信息':'Copy','清空信息':'Clear','上一页':'Previous','下一页':'Next','测站操作':'Row actions','收起查询条件':'Collapse',
    '删除此条信息':'Delete this message','关闭数据源选择':'Close data-source picker','关闭国家选择':'Close country picker','关闭高级筛选':'Close advanced filters',
    '快速选择年份':'Jump to year','快速选择起始年份':'Jump to start year','快速选择终止年份':'Jump to end year' };

  const originalTextNodes = new WeakMap();
  const originalAttributes = new WeakMap();
  function translateDom(root) {
    const toEn = uiPrefs.language === 'en';
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    const nodes = []; while (walker.nextNode()) nodes.push(walker.currentNode);
    for (const node of nodes) {
      if (node.parentElement?.closest('script,style,[data-no-translate]')) continue;
      const raw = node.nodeValue; if (!raw.trim()) continue;
      let record = originalTextNodes.get(node);
      if (!record || record.last !== raw) record = {source:raw,last:raw};
      // Restore original Chinese rather than translating English fragments back.
      const translated = !toEn && /[\u4e00-\u9fff]/.test(record.source)
        ? record.source : localizeUiText(record.source);
      if (translated !== raw) node.nodeValue = translated;
      record.last = translated; originalTextNodes.set(node,record);
    }
    root.querySelectorAll('[title],[aria-label],[placeholder]').forEach(el => {
      if (el.closest('[data-no-translate]')) return;
      const records = originalAttributes.get(el) || {};
      for (const attr of ['title','aria-label','placeholder']) {
        const raw = el.getAttribute(attr); if (!raw) continue;
        let record = records[attr];
        if (!record || record.last !== raw) record = {source:raw,last:raw};
        let translated = !toEn && /[\u4e00-\u9fff]/.test(record.source)
          ? record.source : localizeUiText(record.source);
        if (toEn && translated === record.source && enAttr[record.source.trim()]) translated = enAttr[record.source.trim()];
        if (translated !== raw) el.setAttribute(attr,translated);
        record.last = translated; records[attr] = record;
      }
      originalAttributes.set(el,records);
    });
  }
  let translating = false, translatePending = false, translateObserver;
  const translateObserverOptions = { childList: true, subtree: true, characterData: true, attributes: true, attributeFilter: ['title', 'aria-label', 'placeholder'] };
  function scheduleTranslate() {
    if (translating || translatePending) return;
    translatePending = true;
    requestAnimationFrame(() => {
      translatePending = false;
      translating = true;
      translateObserver.disconnect();
      translateDom(document.body);
      translateObserver.observe(document.body, translateObserverOptions);
      translating = false;
    });
  }
  function setupUiPreferences() {
    loadUiPrefs();
    applyTheme(uiPrefs.theme);
    applyLanguage(uiPrefs.language);
    translateObserver = new MutationObserver(() => { if (!translating) scheduleTranslate(); });
    translateObserver.observe(document.body, translateObserverOptions);
    const trigger = $('#settingsTrigger'), panel = $('#appSettings');
    if (!trigger || !panel) return;
    const close = () => { panel.classList.add('hidden'); trigger.classList.remove('active'); trigger.setAttribute('aria-expanded','false'); };
    trigger.addEventListener('click', event => {
      event.stopPropagation();
      const open = panel.classList.contains('hidden');
      if (open) {
        const rect = trigger.getBoundingClientRect();
        panel.classList.remove('hidden'); trigger.classList.add('active'); trigger.setAttribute('aria-expanded','true');
        panel.style.top = Math.max(12, Math.min(rect.bottom + 8, window.innerHeight - panel.offsetHeight - 12)) + 'px';
        panel.style.right = Math.max(12, Math.min(window.innerWidth - rect.right, window.innerWidth - panel.offsetWidth - 12)) + 'px';
      } else close();
    });
    panel.addEventListener('click', event => event.stopPropagation());
    const saveDownloadSettings = () => post('download-settings', {
      layout: $('#downloadStorageLayout').value,
      postprocess: $('#askObservationPostprocess').checked ? 'ask' : 'off'
    });
    $('#downloadStorageLayout').addEventListener('change', saveDownloadSettings);
    $('#askObservationPostprocess').addEventListener('change', saveDownloadSettings);
    panel.querySelectorAll('#languageSwitch button').forEach(b => b.addEventListener('click', () => { applyLanguage(b.dataset.lang); saveUiPrefs(); }));
    panel.querySelectorAll('#themeSwitch button').forEach(b => b.addEventListener('click', () => { applyTheme(b.dataset.themeValue); saveUiPrefs(); }));
    document.addEventListener('click', () => { if (!panel.classList.contains('hidden')) close(); });
    document.addEventListener('keydown', event => { if (event.key === 'Escape') close(); });
    window.addEventListener('resize', () => { close(); closeAdvancedChoicePicker(); });
  }
  window.MarkWeatherPreview.install({post,payload:dateAdvancedPayload});
  loadCustomPresets(); buildColumnMenu(); bindEvents(); setupTableCellTooltips(); setupMapInteractions(); loadMap(); setupUiPreferences();
  if (location.hash === '#precise') setMode('precise', false);
  if (bridge) {
    bridge.addEventListener('message', event => { const message=event.data||{}; if(message.type==='state')handleState(message);else if(message.type==='space-weather-preview')window.MarkWeatherPreview.handleMessage(message);else if(message.type==='progress')handleProgress(message);else if(message.type==='advanced-choices')handleAdvancedChoices(message);else if(message.type==='map-hover-detail')handleMapHoverDetail(message);else if(message.type==='station-detail')handleStationDetail(message);else if(message.type==='product-coverage')handleProductCoverage(message);else window.MarkSelection.onMessage(message); });
    post('ready', { language:uiPrefs.language });
  } else showNativeBridgeRequired();
})();
