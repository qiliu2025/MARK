/* UTC dates only; at most 240 timeline bins and 31 calendar cells per view. */
(() => {
  'use strict';
  const DAY = 86400000;
  const kinds = ['available', 'missing', 'unknown', 'excluded', 'historical'];
  const colors = {available:'#229e98', missing:'#e78178', unknown:'#eeb153', excluded:'#cbd5e3', historical:'#a7b5cb'};
  const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const ordinal = text => {
    if (!/^\d{4}-\d{2}-\d{2}$/.test(String(text))) return NaN;
    const time = Date.parse(`${text}T00:00:00Z`);
    return Number.isFinite(time) && new Date(time).toISOString().slice(0,10) === text ? time / DAY : NaN;
  };
  const dateText = day => new Date(day * DAY).toISOString().slice(0,10);
  function prepare(value) {
    const first = ordinal(value?.start), last = ordinal(value?.end);
    if (!Number.isInteger(first) || !Number.isInteger(last) || last < first || last-first > 150000) return null;
    const ranges = (value.ranges || []).map(row => ({first:ordinal(row[0]), last:ordinal(row[1]), kind:row[2]}));
    // Reject a broken payload rather than silently paint a misleading green chart.
    let next = first;
    for (const run of ranges) {
      if (run.first !== next || !Number.isInteger(run.last) || run.last < run.first || !kinds.includes(run.kind)) return null;
      next = run.last + 1;
    }
    if (next !== last + 1) return null;
    const counts = Object.fromEntries(kinds.map(k => [k,0]));
    for (const run of ranges) counts[run.kind] += run.last-run.first+1;
    return {first,last,ranges,counts};
  }
  function statusAt(data, day) {
    if (day < data.first || day > data.last) return 'outside';
    let lo=0, hi=data.ranges.length-1;
    while (lo <= hi) {
      const mid=(lo+hi)>>1, run=data.ranges[mid];
      if (day < run.first) hi=mid-1;
      else if (day > run.last) lo=mid+1;
      else return run.kind;
    }
    return 'unknown';
  }
  function intersectRuns(data, first, last) {
    return data.ranges.filter(run => run.first <= last && run.last >= first)
      .map(run => ({first:Math.max(first,run.first), last:Math.min(last,run.last), kind:run.kind}));
  }
  function mount(root, value, english=false, expanded=false, initialDay=null) {
    if (!root) return;
    const data=prepare(value);
    const text=english ? {
      title:'Date coverage', available:'Indexed', missing:'No matching record', unknown:'Unverified', excluded:'Excluded by date filters', historical:'Outside historical scope',
      empty:'Daily coverage is not recorded for this result.', previous:'Previous month', next:'Next month', days:'days',
      month:'Month', year:'Year', ranges:'Date ranges in this month', tip:'Colours describe local index records under the query filters, not file contents or processing quality.',
      timeline:'Full query range; click to jump to a month', weekdays:['Mon','Tue','Wed','Thu','Fri','Sat','Sun'],
      pick:'Select a day to see its status.', outside:'Outside query range'
    } : {
      title:'日期覆盖', available:'有记录', missing:'无匹配记录', unknown:'待核实', excluded:'日期筛选排除', historical:'历史范围外',
      empty:'该结果尚未记录逐日覆盖信息。', previous:'上个月', next:'下个月', days:'天',
      month:'月份', year:'年份', ranges:'本月日期区间', tip:'颜色表示当前查询条件下本地索引的记录情况，不代表文件内容质量或处理质量。',
      timeline:'整个查询范围；点击可跳转月份', weekdays:['一','二','三','四','五','六','日'],
      pick:'点击日期查看当天的记录状态。', outside:'查询范围以外'
    };
    if (!data) { root.innerHTML=`<p class="coverage-note">${text.empty}</p>`; return; }
    const historyNote=value.historyStart ? (english
      ? `Retained archive scan scope starts on ${value.historyStart}. Earlier dates are outside that scope; this does not establish that no older product exists.`
      : `历史扫描收录起点：${value.historyStart}。更早日期属于历史范围外，未列为漏扫；该边界不能单独证明更早没有产品。`) : '';
    const firstDate=new Date(data.first*DAY), lastDate=new Date(data.last*DAY);
    const initial=new Date((initialDay ?? data.first)*DAY);
    let year=initial.getUTCFullYear(), month=initial.getUTCMonth(), selected=null;
    const yearMin=firstDate.getUTCFullYear(), yearMax=lastDate.getUTCFullYear();
    const monthOrdinal=(y,m) => Date.UTC(y,m,1)/DAY;
    const setMonth=day => { const d=new Date(day*DAY);year=d.getUTCFullYear();month=d.getUTCMonth();selected=null;render(); };
    const timeline=[];
    const total=data.last-data.first+1, width=Math.max(1,Math.ceil(total/240));
    for(let begin=data.first; begin<=data.last; begin+=width) {
      const end=Math.min(data.last,begin+width-1), counts=Object.fromEntries(kinds.map(k=>[k,0]));
      const runs=intersectRuns(data,begin,end);
      for(const run of runs) counts[run.kind]+=run.last-run.first+1;
      let percent=0;
      const stops=kinds.filter(k=>counts[k]).map(k=>{
        const start=percent; percent+=100*counts[k]/(end-begin+1);
        return `${colors[k]} ${start.toFixed(3)}% ${percent.toFixed(3)}%`;
      });
      const title=`${dateText(begin)} — ${dateText(end)} · ${kinds.filter(k=>counts[k]).map(k=>`${text[k]} ${counts[k]} ${text.days}`).join(' · ')}`;
      // A mixed bin jumps to its first missing/unverified day, so small gaps remain discoverable.
      const target=runs.find(run=>run.kind==='missing'||run.kind==='unknown')?.first ?? begin;
      timeline.push(`<button type="button" data-coverage-jump="${target}" style="flex:${end-begin+1};background:linear-gradient(90deg,${stops.join(',')})" title="${escape(title)}" aria-label="${escape(title)}"></button>`);
    }
    const legend=kinds.filter(k=>data.counts[k] || k==='available' || k==='missing').map(k=>
      `<span><i class="coverage-dot ${k}"></i>${text[k]} <b>${data.counts[k]}</b> ${text.days}</span>`).join('');
    root.innerHTML=`<section class="coverage-view"><header><b>${text.title}</b>${expanded?'':`<button type="button" class="coverage-expand" data-coverage-expand>${english?'Expand':'展开查看'}</button>`}<small>${value.start} — ${value.end}</small></header><div class="coverage-legend">${legend}</div><div class="coverage-timeline" aria-label="${text.timeline}">${timeline.join('')}</div><div class="coverage-timeline-caption"><span>${value.start}</span><span>${value.end}</span></div><div class="coverage-calendar"></div><p class="coverage-note">${text.tip}</p>${historyNote?`<p class="coverage-note coverage-history-note">${escape(historyNote)}</p>`:""}</section>`;
    const calendar=root.querySelector('.coverage-calendar');
    function render() {
      let begin=monthOrdinal(year,month), end=monthOrdinal(year,month+1)-1;
      if(end < data.first) {year=firstDate.getUTCFullYear();month=firstDate.getUTCMonth();begin=monthOrdinal(year,month);end=monthOrdinal(year,month+1)-1;}
      if(begin > data.last) {year=lastDate.getUTCFullYear();month=lastDate.getUTCMonth();begin=monthOrdinal(year,month);end=monthOrdinal(year,month+1)-1;}
      const offset=(new Date(begin*DAY).getUTCDay()+6)%7;
      const cells=Array.from({length:offset},()=>'<span class="coverage-calendar-spacer"></span>');
      for(let day=begin;day<=end;day++) {
        const kind=statusAt(data,day), title=`${dateText(day)} · ${text[kind]}`;
        cells.push(`<button type="button" class="coverage-day ${kind}${selected===day?' selected':''}" data-coverage-day="${day}" ${kind==='outside'?'disabled':''} title="${escape(title)}" aria-label="${escape(title)}" aria-pressed="${selected===day}">${day-begin+1}</button>`);
      }
      const runs=intersectRuns(data,begin,end);
      calendar.innerHTML=`<div class="coverage-navigation"><button type="button" data-coverage-nav="-1" title="${text.previous}" aria-label="${text.previous}" ${begin<=data.first?'disabled':''}>‹</button><select data-coverage-year aria-label="${text.year}">${Array.from({length:yearMax-yearMin+1},(_,i)=>`<option value="${yearMin+i}" ${yearMin+i===year?'selected':''}>${yearMin+i}</option>`).join('')}</select><select data-coverage-month aria-label="${text.month}">${Array.from({length:12},(_,i)=>`<option value="${i}" ${i===month?'selected':''}>${english?new Date(Date.UTC(2020,i,1)).toLocaleString('en',{month:'short',timeZone:'UTC'}):`${i+1} 月`}</option>`).join('')}</select><button type="button" data-coverage-nav="1" title="${text.next}" aria-label="${text.next}" ${end>=data.last?'disabled':''}>›</button></div><div class="coverage-calendar-grid">${text.weekdays.map(d=>`<span class="coverage-weekday">${d}</span>`).join('')}${cells.join('')}</div><div class="coverage-day-description" aria-live="polite">${selected===null?text.pick:`${dateText(selected)} · ${text[statusAt(data,selected)]}`}</div><details class="coverage-ranges"><summary>${text.ranges} · ${runs.length}</summary>${runs.map(run=>`<div><i class="coverage-dot ${run.kind}"></i><span>${dateText(run.first)}${run.last===run.first?'':` — ${dateText(run.last)}`}</span><b>${text[run.kind]}</b></div>`).join('')}</details>`;
    }
    root.onclick=event=>{
      if(event.target.closest('[data-coverage-expand]')) {
        openExpanded(value,english,monthOrdinal(year,month));
        return;
      }
      const jump=event.target.closest('[data-coverage-jump]');
      if(jump) {setMonth(Number(jump.dataset.coverageJump));return;}
      const nav=event.target.closest('[data-coverage-nav]');
      if(nav) {setMonth(monthOrdinal(year,month+Number(nav.dataset.coverageNav)));return;}
      const day=event.target.closest('[data-coverage-day]');
      if(day) {selected=Number(day.dataset.coverageDay);render();}
    };
    root.onchange=event=>{
      if(event.target.matches('[data-coverage-year]')) {year=Number(event.target.value);selected=null;render();}
      if(event.target.matches('[data-coverage-month]')) {month=Number(event.target.value);selected=null;render();}
    };
    render();
  }
  function openExpanded(value, english=false, initialDay=null) {
        const previous=document.activeElement, layer=document.createElement('div');
        layer.className='settings-layer coverage-expanded-layer';
        layer.innerHTML=`<button class="settings-scrim" type="button" data-coverage-close aria-label="${english?'Close coverage':'关闭日期覆盖'}"></button><section class="settings-dialog coverage-expanded-dialog" role="dialog" aria-modal="true" aria-labelledby="coverageExpandedTitle"><header><div class="settings-mark teal"><svg class="ico" aria-hidden="true"><use href="#i-calendar"></use></svg></div><div><p>DATE COVERAGE</p><h1 id="coverageExpandedTitle">${english?'Date coverage':'日期覆盖'}</h1></div><button class="icon-btn" type="button" data-coverage-close title="${english?'Close':'关闭'}" aria-label="${english?'Close':'关闭'}">×</button></header><div class="settings-scroll coverage-expanded-body"></div></section>`;
        const close=()=>{layer.remove();previous?.focus();};
        layer.addEventListener('click',e=>{if(e.target.closest('[data-coverage-close]'))close();});
        layer.addEventListener('keydown',e=>{
          if(e.key==='Escape'){e.preventDefault();e.stopPropagation();close();}
          if(e.key==='Tab') {
            const nodes=[...layer.querySelectorAll('.coverage-expanded-dialog button:not(:disabled),select,summary')].filter(n=>n.offsetParent);
            const first=nodes[0],last=nodes.at(-1);
            if(e.shiftKey&&document.activeElement===first){e.preventDefault();last?.focus();}
            else if(!e.shiftKey&&document.activeElement===last){e.preventDefault();first?.focus();}
          }
        });
        document.body.appendChild(layer);
        mount(layer.querySelector('.coverage-expanded-body'),value,english,true,initialDay);
        layer.querySelector('.coverage-expanded-dialog [data-coverage-close]').focus();
  }
  function mountCompact(root, value, english=false) {
    if(!root)return;
    root.onclick=null;root.onchange=null;
    const data=prepare(value);
    if(!data) {root.innerHTML=`<p class="coverage-note">${english?'Daily coverage is not recorded for this result.':'该结果尚未记录逐日覆盖信息。'}</p>`;return;}
    const labels=english?{available:'Indexed',missing:'No matching record',unknown:'Unverified',excluded:'Excluded',historical:'Outside historical scope'}:
      {available:'有记录',missing:'无匹配记录',unknown:'待核实',excluded:'日期筛选排除',historical:'历史范围外'};
    const legend=kinds.filter(kind=>data.counts[kind]).map(kind=>`<span><i class="coverage-dot ${kind}"></i>${labels[kind]} ${data.counts[kind]} ${english?'days':'天'}</span>`).join('');
    root.innerHTML=`<div class="coverage-compact"><div class="coverage-compact-actions"><small data-no-translate>${escape(value.start)} — ${escape(value.end)}</small><button type="button" class="coverage-expand" data-coverage-expand>${english?'Expand':'展开查看'}</button></div><div class="coverage-legend">${legend}</div></div>`;
    root.onclick=event=>{if(event.target.closest('[data-coverage-expand]'))openExpanded(value,english,data.first);};
  }
  const api={prepare,statusAt,intersectRuns,mount,mountCompact,ordinal,dateText};
  if(typeof module!=='undefined') module.exports=api;
  if(typeof window!=='undefined') window.GnssCoverage=api;
})();
