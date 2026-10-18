(() => {
  'use strict';
  const $=selector=>document.querySelector(selector);
  const esc=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const english=()=>document.documentElement.dataset.lang==='en'||document.documentElement.lang.startsWith('en');
  let api,latest={},report=null,openWanted=false,page=0,filter='all',activeDate='',focusReturn;
  let availableKeys=new Set(),unknownKeys=new Set(),dayView=null;
  const key=point=>`${point[0]}|${Number(point[1]).toFixed(6)}|${Number(point[2]).toFixed(6)}`;
  const tr=text=>api?.translate(text)??text;
  const set=(id,value)=>{const node=$(id);if(node&&node!==document.activeElement&&node.value!==String(value))node.value=String(value);};
  const label=(zh,en)=>english()?en:zh;
  const message=r=>{
    if(!english())return r.message||'';
    return ({matched:'The fixed station set satisfies all enabled constraints.',unmet:'The current station set does not satisfy all enabled constraints.',
      'candidate-shortage':'Too few candidates meet the filters and coordinate requirements. The existing selection was retained.',
      'coverage-shortage':'The candidate pool cannot meet the daily minimum on at least one date. The existing selection was retained.',
      'unverified-coverage':'Some dates lack enough confirmed records; unverified records cannot guarantee coverage.',
      'not-found':'No fully satisfying set was found within the bounded search. This does not prove infeasibility. The existing selection was retained.',
      'no-dates':'No dates remain after the explicit date filters.','outside-scope':'Some selected stations are outside the current candidate filters.',
      invalid:'Invalid selection parameters. Check the target, daily threshold, dates and distances.',cancelled:'Selection calculation cancelled.',error:'Selection calculation did not complete.'})[r.status]||r.message||'';
  };
  function inputs() {
    const mode=$('#selectionDailyMode').value;
    $('#selectionDailyStations').disabled=mode!=='stations';
    $('#selectionDailyPercent').disabled=mode!=='percent';
    $('#selectionDailyStationsField').classList.toggle('hidden',mode!=='stations');
    $('#selectionDailyPercentField').classList.toggle('hidden',mode!=='percent');
    $('#selectionDailyControls').classList.toggle('unlimited',mode==='ignore');
    $('#selectionDailyControls').title=mode==='ignore'?label('不限制每日可用站数','No daily minimum'):label('允许单站缺测','Individual gaps allowed');
    const unit=$('#selectionDailyStationsField > span'),unitLabel=label('站','stn');
    if(unit.textContent!==unitLabel)unit.textContent=unitLabel;
  }
  function parameters() {
    const nodes=['#selectionCount','#minimumDistance','#maximumNeighbor'];
    const mode=$('#selectionDailyMode').value;
    if(mode==='stations')nodes.push('#selectionDailyStations');
    if(mode==='percent')nodes.push('#selectionDailyPercent');
    for(const id of nodes)if(!$(id).value.trim()||!$(id).checkValidity()){$(id).reportValidity();return null;}
    if(mode==='stations'&&Number($('#selectionDailyStations').value)>Number($('#selectionCount').value)) {
      api.toast(label('每日最低可用站数不能大于目标测站数量。','The daily minimum cannot exceed the target count.'));return null;
    }
    return {count:$('#selectionCount').value,minDistance:$('#minimumDistance').value,maxNeighbor:$('#maximumNeighbor').value,
      dailyMode:mode,dailyStations:$('#selectionDailyStations').value,dailyPercent:$('#selectionDailyPercent').value};
  }
  function clearDay() {
    dayView=null;availableKeys.clear();unknownKeys.clear();$('#stationDayBanner')?.remove();api?.redraw();
  }
  function close() {
    $('#stationSetLayer').classList.add('hidden');focusReturn?.focus();
  }
  function show() {
    if(!report){openWanted=true;api.post('station-selection-report');return;}
    focusReturn=document.activeElement;renderReport();$('#stationSetLayer').classList.remove('hidden');
    $('#stationSetLayer [data-station-set-close]').focus();
  }
  function chooseDate(date) {
    if(report?.stale){api.toast(tr('条件或选站已变化，请重新检查'));return;}
    const day=report?.days.find(d=>d[0]===date&&d[4]);if(!day)return;
    activeDate=date;api.post('station-selection-day',{date});close();
  }
  function chart() {
    const days=report.days,r=report.summary;
    if(!days.length)return '';
    const max=Math.max(1,r.requested,r.selected,...days.map(d=>d[1]+d[2]));
    const x=i=>44+(days.length===1?.5:i/(days.length-1))*838;
    const y=n=>145-n/max*126;
    const points=days.map((d,i)=>d[4]?`${x(i).toFixed(2)},${y(d[1]).toFixed(2)}`:'').filter(Boolean).join(' ');
    const unknown=days.some(d=>d[2]>0)?`<polyline class="unknown-line" points="${days.map((d,i)=>d[4]?`${x(i).toFixed(2)},${y(d[1]+d[2]).toFixed(2)}`:'').filter(Boolean).join(' ')}"/>`:'';
    let grid='';for(let i=0;i<=4;i++){const n=Math.round(max*i/4);grid+=`<line class="grid-line" x1="44" x2="882" y1="${y(n)}" y2="${y(n)}"/><text x="35" y="${y(n)+3}" text-anchor="end">${n}</text>`;}
    const threshold=r.threshold>0?`<line class="threshold-line" x1="44" x2="882" y1="${y(r.threshold)}" y2="${y(r.threshold)}"/><text x="880" y="${Math.max(11,y(r.threshold)-5)}" text-anchor="end">≥ ${r.threshold}</text>`:'';
    return `<svg id="stationSetChart" class="station-set-chart" viewBox="0 0 900 180" preserveAspectRatio="none" role="img" aria-label="${esc(label('逐日可用站数；点击日期查看地图','Daily available station count; inspect dates on the map'))}">${grid}${threshold}${unknown}<polyline class="coverage-line" points="${points}"/><text x="44" y="168">${days[0][0]}</text><text x="882" y="168" text-anchor="end">${days.at(-1)[0]}</text></svg>
      <div class="station-set-chart-note"><span><i></i>${esc(tr('已确认可用'))}　<i class="unknown"></i>${esc(tr('未知记录'))}　<i class="threshold"></i>≥ ${r.threshold}</span><span>${esc(tr('不足日期不会自动排除'))}</span></div>`;
  }
  function filteredDays() {
    if(!report)return [];
    return report.days.filter(d=>filter==='shortage'?d[4]&&d[1]<report.summary.threshold:filter==='unknown'?d[4]&&d[2]>0:filter==='excluded'?!d[4]:true);
  }
  function table() {
    const rows=filteredDays(),size=60,total=Math.max(1,Math.ceil(rows.length/size));page=Math.min(page,total-1);
    $('#stationSetDateCount').textContent=label(`${rows.length} 个日期`,`${rows.length} dates`);
    $('#stationSetRows').innerHTML=rows.slice(page*size,(page+1)*size).map(d=>`<tr class="${!d[4]?'excluded':d[1]<report.summary.threshold?'shortage':''} ${activeDate===d[0]?'active':''}"><td><button type="button" data-coverage-date="${d[0]}" ${!d[4]||report.stale?'disabled':''} title="${esc(tr('查看当天地图'))}">${d[0]}</button></td><td>${d[4]?d[1]:'—'}</td><td>${d[4]?d[2]:'—'}</td><td>${d[4]?d[3]:'—'}</td><td>${d[4]?d[5]:'—'}</td></tr>`).join('')||`<tr><td colspan="5">${esc(tr('当前页无匹配日期'))}</td></tr>`;
    $('#stationSetPageLabel').textContent=`${page+1} / ${total}`;
    $('#stationSetPrevious').disabled=page===0;$('#stationSetNext').disabled=page>=total-1;
  }
  function renderReport() {
    if(!report)return;
    const r=report.summary,stale=report.stale;
    $('#stationSetBody').innerHTML=`<div class="station-set-notice ${stale?'stale':r.success?'':'unmet'}"><b>${esc(stale?tr('条件或选站已变化，请重新检查'):message(r))}</b>${r.generated&&!r.applied?`<p>${esc(tr('候选方案尚未采用'))}</p>`:''}<p>${esc(tr('同站跨数据源只计一次；按当前采样率、RINEX 与元数据条件匹配的记录统计。'))}</p></div>
      <div class="station-set-stats"><div><span>${esc(tr('所选测站'))}</span><b>${r.selected} / ${r.requested}</b></div><div><span>${esc(tr('每日最少可用'))}</span><b>${r.minimumAvailable} <small>${r.threshold>0?esc(label(`要求 ≥ ${r.threshold}`,`required ≥ ${r.threshold}`)):esc(label('不限制','unlimited'))}</small></b></div><div><span>${esc(tr('不足日期'))}</span><b>${r.failingDays} / ${r.activeDays}</b></div><div><span>${esc(tr('实际最小间距'))}</span><b>${Number(r.minimumDistanceKm).toFixed(1)} <small>km</small></b></div></div>
      ${chart()}<div class="station-set-chart-note"><span>${esc(tr('平均每日可用'))} ${Number(r.averageAvailable).toFixed(1)} · ${esc(tr('已排除日期'))} ${r.excludedDays} · ${esc(tr('待核实日期'))} ${r.unknownDays}</span></div>
      <p class="coverage-note">${esc(tr('每天的可用子集仍可能存在空间偏差，可点击日期检查地图。'))}</p>
      <div class="station-set-toolbar"><select id="stationSetDateFilter" aria-label="${esc(tr('日期筛选'))}">${[['all','全部日期'],['shortage','不足日期'],['unknown','待核实日期'],['excluded','已排除日期']].map(([value,text])=>`<option value="${value}" ${value===filter?'selected':''}>${esc(tr(text))}</option>`).join('')}</select><input type="date" id="stationSetJumpDate" aria-label="${esc(tr('查看当天地图'))}"><span id="stationSetDateCount"></span></div>
      <table class="station-set-table"><thead><tr>${['日期','所选可用','所选待核实','候选可用','候选待核实'].map(t=>`<th>${esc(tr(t))}</th>`).join('')}</tr></thead><tbody id="stationSetRows"></tbody></table><div class="station-set-pagination"><button class="btn soft" id="stationSetPrevious">${esc(tr('上一页'))}</button><span id="stationSetPageLabel"></span><button class="btn soft" id="stationSetNext">${esc(tr('下一页'))}</button></div>`;
    $('#stationSetDateFilter').addEventListener('change',e=>{filter=e.target.value;page=0;table();});
    $('#stationSetJumpDate').addEventListener('change',e=>chooseDate(e.target.value));
    $('#stationSetPrevious').addEventListener('click',()=>{page--;table();});$('#stationSetNext').addEventListener('click',()=>{page++;table();});
    $('#stationSetChart')?.addEventListener('click',e=>{
      const rect=e.currentTarget.getBoundingClientRect(),pos=(e.clientX-rect.left)/rect.width*900;
      const index=Math.max(0,Math.min(report.days.length-1,Math.round((pos-44)/838*(report.days.length-1))));chooseDate(report.days[index][0]);
    });
    table();
  }
  function exportReport() {
    if(!report)return;
    const r=report.summary,quote=v=>`"${String(v??'').replace(/"/g,'""')}"`;
    const metadata=[['MARK station-set coverage'],['status',r.status],['stale',report.stale],['applied',r.applied],['target',r.requested],['selected',r.selected],['daily_minimum',r.threshold],['random_seed',r.randomSeed],['minimum_spacing_km',r.minimumDistanceKm],['confirmed_records_only',true],['station_ids',...(report.stationIds||[])],[],['date','eligible','selected_available','selected_unverified','candidate_available','candidate_unverified']];
    const csv='\ufeff'+[...metadata,...report.days.map(d=>[d[0],d[4],d[1],d[2],d[3],d[5]])].map(row=>row.map(quote).join(',')).join('\r\n');
    const url=URL.createObjectURL(new Blob([csv],{type:'text/csv;charset=utf-8'}));
    const link=document.createElement('a');link.href=url;link.download=`MARK-station-selection-${report.days[0]?.[0]||'report'}.csv`;link.click();setTimeout(()=>URL.revokeObjectURL(url),30000);
  }
  function onState(current) {
    latest=current;
    const s=current.stationSelection||{busy:false,stale:true,summary:{}};
    if(s.dailyMode!==undefined){set('#selectionDailyMode',s.dailyMode);set('#selectionDailyStations',s.dailyStations);set('#selectionDailyPercent',s.dailyPercent);inputs();}
    const busy=Boolean(s.busy),r=s.summary||{};
    $('#uniformSelect').disabled=busy||s.metadataLoading||current.querying||!current.visible;
    $('#randomSelect').disabled=busy||s.metadataLoading||current.querying||!current.visible;
    $('#checkStationSelection').disabled=busy||s.metadataLoading||current.querying||!current.selected;
    $('#cancelStationSelection').classList.toggle('hidden',!busy);
    const text=busy?tr('正在后台计算选站方案…'):s.metadataLoading?label('测站详细信息正在加载，完成后可选站','Loading station details; selection will be available when complete'):!r.status?tr('尚未检查选站覆盖'):s.stale?tr('条件或选站已变化，请重新检查'):r.success?label(`已通过 · 每日最少 ${r.minimumAvailable} 站 · ${r.activeDays} 天`,`Passed · daily minimum ${r.minimumAvailable} · ${r.activeDays} days`):message(r);
    const waiting=busy||s.metadataLoading?text:current.querying?label('正在查询候选测站…','Querying candidate stations…'):!current.visible?label('请先查询测站','Query stations first'):'';
    $('#uniformSelect').title=waiting||label('按上方四项条件生成均匀分布的测站集合','Generate a distributed station set using the four constraints above');
    $('#randomSelect').title=waiting||label('按上方四项条件重新生成随机测站集合','Generate a new random station set using the four constraints above');
    $('#checkStationSelection').title=waiting||(!current.selected?label('请先选择测站','Select stations first'):text);
    $('.selection-actions').setAttribute('aria-busy',String(busy));
    if(report&&report.stale!==Boolean(s.stale)){report.stale=Boolean(s.stale);if(!$('#stationSetLayer').classList.contains('hidden'))renderReport();}
    if(s.stale&&dayView)clearDay();
  }
  function onMessage(value) {
    if(value.type==='station-selection-report') {
      report=value;page=0;
      if(openWanted||!$('#stationSetLayer').classList.contains('hidden')) {openWanted=false;show();}
      else if(!value.summary.success&&value.summary.status!=='cancelled') show();
    } else if(value.type==='station-selection-day') {
      dayView=value;availableKeys=new Set(value.points.map(key));unknownKeys=new Set(value.unknownPoints.map(key));
      $('#stationDayBanner')?.remove();const banner=document.createElement('div');banner.id='stationDayBanner';banner.className='station-day-banner';
      banner.innerHTML=`<span>${esc(value.date)} · ${esc(label(`${value.available} 个已确认可用站，${value.unknown} 个待核实站`,`${value.available} confirmed available, ${value.unknown} unverified`))}${value.candidatePlan?` · ${esc(tr('候选方案尚未采用'))}`:''}</span><button type="button">${esc(tr('返回完整地图'))}</button>`;
      banner.querySelector('button').addEventListener('click',clearDay);$('#mapViewport').appendChild(banner);api.redraw();
    }
  }
  function install(adapter) {
    api=adapter;inputs();
    const layer=document.createElement('div');layer.id='stationSetLayer';layer.className='settings-layer station-set-layer hidden';
    layer.innerHTML=`<button class="settings-scrim" type="button" data-station-set-close aria-label="关闭"></button><section class="settings-dialog station-set-dialog" role="dialog" aria-modal="true" aria-labelledby="stationSetTitle"><header><div class="settings-mark teal"><svg class="ico" aria-hidden="true"><use href="#i-calendar"></use></svg></div><div><p>FIXED STATION SET</p><h1 id="stationSetTitle">选站覆盖检查</h1><span>待核实不计入可用站数</span></div><button class="icon-btn" type="button" data-station-set-close aria-label="关闭">×</button></header><div class="settings-scroll station-set-body" id="stationSetBody"></div><footer><span>不足日期不会自动排除</span><button class="btn soft" id="stationSetExport">导出检查结果</button><button class="btn primary" type="button" data-station-set-close>关闭</button></footer></section>`;
    document.body.appendChild(layer);
    layer.addEventListener('click',e=>{if(e.target.closest('[data-station-set-close]'))close();const day=e.target.closest('[data-coverage-date]');if(day)chooseDate(day.dataset.coverageDate);});
    document.addEventListener('keydown',e=>{
      if(layer.classList.contains('hidden'))return;
      if(e.key==='Escape'){e.preventDefault();close();}
      if(e.key==='Tab'){const nodes=[...layer.querySelectorAll('button,input,select')].filter(n=>!n.disabled&&n.getClientRects().length),first=nodes[0],last=nodes.at(-1);if(e.shiftKey&&document.activeElement===first){e.preventDefault();last.focus();}else if(!e.shiftKey&&document.activeElement===last){e.preventDefault();first.focus();}}
    });
    $('#stationSetExport').addEventListener('click',exportReport);
    for(const id of ['#selectionCount','#minimumDistance','#maximumNeighbor','#selectionDailyMode','#selectionDailyStations','#selectionDailyPercent'])$(id).addEventListener('change',()=>{inputs();const payload=parameters();if(payload)api.post('selection-settings',payload);});
    for(const [id,type] of [['#uniformSelect','uniform-select'],['#randomSelect','random-select'],['#checkStationSelection','check-station-selection']])$(id).addEventListener('click',()=>{
      const payload=parameters();if(!payload)return;
      if(type==='random-select') {
        const words=crypto.getRandomValues(new Uint32Array(2));
        payload.randomSeed=(((BigInt(words[0])<<32n)|BigInt(words[1]))||1n).toString();
      }
      openWanted=type==='check-station-selection';api.post(type,payload);
    });
    $('#cancelStationSelection').addEventListener('click',()=>api.post('cancel-station-selection'));
    new MutationObserver(()=>{onState(latest);if(report&&!layer.classList.contains('hidden'))renderReport();}).observe(document.documentElement,{attributes:true,attributeFilter:['lang']});
  }
  window.MarkSelection={install,onState,onMessage,get dayView(){return dayView;},isAvailable:point=>availableKeys.has(key(point)),isUnknown:point=>unknownKeys.has(key(point))};
})();
