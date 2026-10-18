(() => {
  'use strict';
  const $ = selector => document.querySelector(selector);
  const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const text = (en, zh) => document.documentElement.lang === 'en' ? en : zh;
  const metrics = [
    {name:'Kp', mode:'dateKpMode', lower:'dateKpValue', upper:'dateKpUpper', unit:'', title:()=>text('Daily maximum Kp','Kp 日峰值')},
    {name:'Dst', mode:'dateDstMode', lower:'dateDstValue', upper:'dateDstUpper', unit:'nT', title:()=>text('Daily minimum Dst','Dst 日最低值')},
    {name:'F10.7', mode:'dateF107Mode', lower:'dateF107Value', upper:'dateF107Upper', unit:'sfu', title:()=>$('#dateF107Basis').value==='adjusted'?text('Daily F10.7 (1 AU)','F10.7 日值（1 AU）'):text('Daily F10.7 (observed)','F10.7 日值（观测值）')}
  ];
  let hooks, panel, summary, tooltip, data=null, timer=0, serial=0, request='', busy=false, error='', selected=-1, chartBounds, drawFrame=0;
  const visible=[true,true,true];
  let viewStart=0, viewEnd=0, rangeKey='', drag=null, suppressClick=false;
  const opened = () => !$('#dateAdvancedLayer').classList.contains('hidden');
  const palette = () => document.documentElement.dataset.theme === 'dark'
    ? {ink:'#dae4ef', muted:'#a7b6c7', grid:'#35445a', background:'#182331', keep:'#55c8bd', missing:'#8392a3', series:['#84B4FF','#B49AFF','#F7BD65']}
    : {ink:'#273e54', muted:'#607489', grid:'#e4eaf1', background:'#FFFFFF', keep:'#078f89', missing:'#8e9aaa', series:['#347BEA','#8957D7','#D8891C']};
  const number = value => value === null ? '—' : Number(value).toFixed(2).replace(/\.?0+$/, '');
  function rule(index) {
    const m=metrics[index], mode=$('#'+m.mode).value, low=Number($('#'+m.lower).value), high=Number($('#'+m.upper).value);
    if(mode==='ignore') return text('No restriction','不限制');
    const unit=m.unit?' '+m.unit:'';
    if(mode==='range') return `${number(low)} ≤ ${m.name} ≤ ${number(high)}${unit}`;
    const operator=index===1 ? (mode==='keep'?'≤':'>') : (mode==='keep'?'>':'≤');
    return `${m.name} ${operator} ${number(low)}${unit}`;
  }
  function reasons(row) {
    const messages=[];
    metrics.forEach((m,i)=>{
      if(row[6] & (1<<i)) messages.push(text(`${m.name} unavailable`,`${m.name} 缺失`));
      if(row[5] & (1<<i)) messages.push(text(`${m.name} fails ${rule(i)}`,`${m.name} 不满足 ${rule(i)}`));
    });
    return messages.join(' · ') || text('All enabled conditions pass','满足所有启用条件');
  }
  function status(row) {return row[4] ? text('Retained','保留') : row[6] ? text('Excluded: missing index','剔除：指标缺失') : text('Excluded','剔除');}
  function reasonChips(row) {
    const colors=palette().series;
    return metrics.map((m,i)=>{
      const bit=1<<i, missing=!!(row[6]&bit);
      if(!((row[5]|row[6])&bit))return '';
      return `<i class="weather-reason-chip${missing?' missing':''}" data-reason-metric="${i}" style="--index-color:${colors[i]}">${m.name}${missing?' '+text('(missing)','（缺失）'):''}</i>`;
    }).join('');
  }
  function content() {
    const p=palette(), n=data?.requestedDays||0, kept=data?.retainedDays||0, missing=data?.missingDays||0;
    const errorText=error==='invalid-conditions' ? text('Check the thresholds and ensure the lower limit does not exceed the upper limit.','请检查阈值，并确保下限不大于上限。')
      : error==='invalid-dates' ? text('Select a valid start and end date.','请选择有效的起止日期。')
      : error ? text('Unable to read the space-weather series. Check the local knowledge base.','无法读取空间天气时序，请检查本地知识库。') : '';
    summary.innerHTML=`<div class="weather-preview-heading"><strong>${text('Date selection preview','日期筛选预览')}</strong><span>${escape($('#startDate').value)} — ${escape($('#endDate').value)} · UTC</span></div>
      <div class="weather-preview-counts"><span>${text('Before','筛选前')}<b>${data&&!busy&&!error?n:'—'}</b></span><span class="kept">${text('Retained','保留')}<b>${data&&!busy&&!error?kept:'—'}</b></span><span class="excluded">${text('Excluded','剔除')}<b>${data&&!busy&&!error?n-kept:'—'}</b></span><span>${text('Missing enabled indices','启用指标缺失')}<b>${data&&!busy&&!error?missing:'—'}</b></span></div>
      <p class="weather-preview-status" role="status"${!busy&&!errorText?' hidden':''}>${busy?text('Updating preview…','正在更新预览…'):errorText}</p>`;
    $('#weatherMetricRules').innerHTML=metrics.map((m,i)=>{
      const active=$('#'+m.mode).value!=='ignore';
      const count=data?.rows.filter(row=>!(row[5]&(1<<i))&&!(row[6]&(1<<i))).length||0;
      $('#'+m.mode).closest('.weather-row').style.setProperty('--weather-index-color',p.series[i]);
      return `<button type="button" data-weather-metric="${i}" aria-pressed="${visible[i]}" title="${escape(visible[i]?text(`Hide ${m.name} curve and axis`,`隐藏 ${m.name} 曲线与纵轴`):text(`Show ${m.name} curve and axis`,`显示 ${m.name} 曲线与纵轴`))}" style="--index-color:${p.series[i]}"><b>${m.name}</b><span>${escape(rule(i))}</span>${active&&data&&!busy&&!error?`<small>${count}/${n} ${text('pass','满足')}</small>`:''}<em>${visible[i]?text('Shown','显示中'):text('Hidden','已隐藏')}</em></button>`;
    }).join('');
    $('#weatherChartLabel').textContent=text('Kp, Dst and F10.7 daily series','Kp、Dst、F10.7 逐日时序');
    for(const id of ['weatherSeriesChart','weatherDateStrip'])$('#'+id).setAttribute('aria-label',text('Space-weather date selection. Wheel to zoom, drag to pan, arrow keys to inspect, plus or minus to zoom.','空间天气日期筛选。滚轮缩放，拖动平移，方向键查看日期，加减键缩放。'));
    $('#weatherLegend').innerHTML=metrics.map((m,i)=>`<span><i style="background:${p.series[i]}"></i>${m.name}</span>`).join('')+`<span><i style="background:${p.keep}"></i>${text('Retained','保留')}</span><span><i class="weather-missing-key"></i>${text('Missing: hatched','缺失：斜线')}</span>`;
    $('#weatherPreviewNote').textContent=text('Buttons control curves and axes. Date strips show every filtering cause, including hidden indices. Stacked colors indicate multiple causes, hatching indicates missing indices. Wheel to zoom and drag to pan.','按钮控制曲线与纵轴，日期条显示所有筛选原因，包括已隐藏的指数。同日多个原因上下叠色，斜线表示指标缺失。滚轮缩放，拖动平移。');
    panel.classList.toggle('weather-preview-busy',busy||!!error);
    updateDay();draw();
  }
  function updateDay() {
    const row=!busy&&!error?data?.rows[selected]:null;
    $('#weatherSelectedDay').textContent=row ? `${row[0]} · Kp ${number(row[1])} · Dst ${number(row[2])} nT · F10.7 ${number(row[3])} sfu · ${status(row)} · ${reasons(row)}`
      : text('Hover or click a date to inspect the values. Use arrow keys on the chart to move between dates.','悬停或点击日期可查看数值。聚焦图表后可用方向键逐日查看。');
  }
  function windowControls() {
    const total=data&&!busy&&!error?data.rows.length:0, span=viewEnd-viewStart, available=total>0, minimum=Math.min(7,total);
    $('#weatherZoomIn').textContent=text('+ Zoom in','＋ 放大');$('#weatherZoomOut').textContent=text('− Zoom out','− 缩小');$('#weatherZoomReset').textContent=text('Full range','全部日期');
    $('#weatherPanLeft').textContent=text('← Earlier','← 前移');$('#weatherPanRight').textContent=text('Later →','后移 →');
    $('#weatherZoomIn').disabled=!available||span<=minimum;$('#weatherZoomOut').disabled=!available||span>=total;$('#weatherZoomReset').disabled=!available||span>=total;
    $('#weatherPanLeft').disabled=!available||viewStart<=0;$('#weatherPanRight').disabled=!available||viewEnd>=total;
    $('#weatherViewRange').textContent=available?`${data.rows[viewStart][0]} — ${data.rows[viewEnd-1][0]} · ${span}/${total} ${text('days','天')}`:text('No dates to display','暂无可显示日期');
    const slider=$('#weatherPanRange');slider.max=String(Math.max(0,total-span));slider.value=String(viewStart);slider.disabled=!available||span>=total;slider.setAttribute('aria-label',text('Move the visible date range','移动图中日期范围'));
    $('#weatherPanLabel').textContent=text('Move dates','移动日期');
    $('#weatherPanHint').textContent=available&&span<total?text('Drag the slider or chart to move','拖动滑块或图表移动'):text('Zoom in to inspect daily values','放大后可查看具体日期');
  }
  function changeView(start,span) {
    if(!data||busy||error||!data.rows.length)return;
    const total=data.rows.length;span=Math.max(Math.min(7,total),Math.min(total,Math.round(span)));
    viewStart=Math.max(0,Math.min(total-span,Math.round(start)));viewEnd=viewStart+span;
    if(selected<viewStart||selected>=viewEnd)selected=-1;
    tooltip.hidden=true;updateDay();scheduleDraw();
  }
  function zoom(direction,ratio=.5,factor=.5) {
    if(!data||busy||error)return;
    const span=viewEnd-viewStart, next=Math.max(Math.min(7,data.rows.length),Math.min(data.rows.length,Math.round(span*(direction>0?factor:1/factor))));
    const anchor=viewStart+Math.max(0,Math.min(1,ratio))*span;
    changeView(anchor-ratio*next,next);
  }
  function pan(direction) {const span=viewEnd-viewStart;changeView(viewStart+direction*Math.max(1,Math.round(span/2)),span);}
  function scheduleDraw() {cancelAnimationFrame(drawFrame);drawFrame=requestAnimationFrame(()=>{drawFrame=0;draw();});}
  function surface(id,height) {
    const element=$('#'+id), width=element.getBoundingClientRect().width;
    if(!width)return null;
    const ratio=window.devicePixelRatio||1;element.width=Math.round(width*ratio);element.height=Math.round(height*ratio);
    const ctx=element.getContext('2d');ctx.scale(ratio,ratio);ctx.font='11px "Segoe UI", sans-serif';ctx.lineWidth=1;
    return {ctx,width,height};
  }
  function integerScale(low,high) {
    const desired=Math.max(1,(high-low)/4), magnitude=10**Math.floor(Math.log10(desired));
    const factor=[1,2,2.5,5,10].find(value=>value>=desired/magnitude);
    const step=Math.max(1,Math.ceil(factor*magnitude));
    const first=Math.floor(low/step), last=Math.ceil(high/step);
    const ticks=Array.from({length:last-first+1},(_,i)=>(last-i)*step);
    return {low:first*step,high:last*step,ticks};
  }
  function draw() {
    if(!opened())return;
    const chart=surface('weatherSeriesChart',212), strip=surface('weatherDateStrip',66);
    if(!chart||!strip)return;
    windowControls();
    const p=palette(), rows=data&&!busy&&!error?data.rows.slice(viewStart,viewEnd):[], shown=metrics.map((m,i)=>i).filter(i=>visible[i]);
    const {ctx,width,height}=chart, left=48, right=shown.length===3?174:shown.length===2?84:14, top=26, bottom=32, w=width-left-right, h=height-top-bottom;
    chartBounds={left,w,count:rows.length,start:viewStart,end:viewEnd};
    if(!rows.length){ctx.fillStyle=p.muted;ctx.fillText(busy?text('Loading…','正在读取…'):text('No series available','暂无时序数据'),left,80);return;}
    const x=index=>left+(index+.5)/rows.length*w;
    const scales=metrics.map((m,index)=>{
      const mode=$('#'+m.mode).value;
      const thresholds=mode==='ignore'?[]:mode==='range'?[Number($('#'+m.lower).value),Number($('#'+m.upper).value)]:[Number($('#'+m.lower).value)];
      let low=Infinity,high=-Infinity;
      for(const row of rows){const value=row[index+1];if(value!==null){low=Math.min(low,value);high=Math.max(high,value);}}
      for(const value of thresholds){low=Math.min(low,value);high=Math.max(high,value);}
      if(index===0){low=0;high=9;}
      else if(!Number.isFinite(low)){low=index===1?-100:50;high=index===1?0:150;}
      else{if(index===1){low=Math.min(0,low);high=Math.max(0,high);}const padding=Math.max((high-low)*.12,1);low-=padding;high+=padding;}
      if(index===2)low=Math.max(0,low);
      const scale=index===0?{low,high,ticks:[9,6,3,0]}:integerScale(low,high);
      return {...scale,mode,thresholds,y:value=>top+(scale.high-value)/(scale.high-scale.low)*h};
    });
    const gridScale=scales[shown[0]??0];
    for(const value of gridScale.ticks){
      const py=gridScale.y(value);
      ctx.strokeStyle=p.grid;ctx.beginPath();ctx.moveTo(left,py);ctx.lineTo(left+w,py);ctx.stroke();
    }
    shown.forEach((index,position)=>{
      const m=metrics[index], axis=position===0?left:position===1?left+w:left+w+90, scale=scales[index], color=p.series[index];
      ctx.fillStyle=color;ctx.textAlign='left';
      ctx.fillText(index===0?'Kp':`${m.name} (${m.unit})`,position===0?axis-10:axis+7,12);
      ctx.textAlign=position===0?'right':'left';
      ctx.strokeStyle=color;ctx.globalAlpha=.3;ctx.beginPath();ctx.moveTo(axis,top);ctx.lineTo(axis,top+h);ctx.stroke();ctx.globalAlpha=1;
      for(const value of scale.ticks){
        ctx.fillText(String(value),position===0?axis-7:axis+7,scale.y(value)+4);
      }
    });
    ctx.save();ctx.beginPath();ctx.rect(left-3,top-4,w+6,h+8);ctx.clip();
    shown.forEach(index=>{
      const scale=scales[index], color=p.series[index];
      ctx.strokeStyle=color;ctx.globalAlpha=1;ctx.lineWidth=1;ctx.setLineDash([4,3]);
      scale.thresholds.forEach(value=>{ctx.beginPath();ctx.moveTo(left,scale.y(value));ctx.lineTo(left+w,scale.y(value));ctx.stroke();});
      ctx.setLineDash([]);ctx.globalAlpha=scale.mode==='ignore'?.65:1;ctx.lineWidth=1.25;
      ctx.beginPath();let connected=false;
      rows.forEach((row,day)=>{const value=row[index+1];if(value===null){connected=false;return;}if(connected)ctx.lineTo(x(day),scale.y(value));else ctx.moveTo(x(day),scale.y(value));connected=true;});ctx.stroke();
      rows.forEach((row,day)=>{
        const value=row[index+1];if(value===null)return;
        const failed=!!(row[5]&(1<<index));
        ctx.fillStyle=failed?p.background:color;ctx.strokeStyle=color;ctx.lineWidth=1.5;
        ctx.beginPath();ctx.arc(x(day),scale.y(value),rows.length>600?1:failed?3.1:2.1,0,Math.PI*2);ctx.fill();if(failed)ctx.stroke();
      });
      ctx.globalAlpha=1;ctx.lineWidth=1;
    });
    if(selected>=viewStart&&selected<viewEnd){ctx.strokeStyle=p.ink;ctx.setLineDash([2,3]);ctx.beginPath();ctx.moveTo(x(selected-viewStart),top);ctx.lineTo(x(selected-viewStart),top+h);ctx.stroke();ctx.setLineDash([]);}
    ctx.restore();
    if(!shown.length){ctx.fillStyle=p.muted;ctx.textAlign='center';ctx.fillText(text('Select an index button to show its curve','点击上方指数按钮显示曲线'),left+w/2,top+h/2);}
    const ticks=Math.min(rows.length,width<620?3:5);
    for(let i=0;i<ticks;i++){
      const index=ticks===1?0:Math.round(i*(rows.length-1)/(ticks-1));
      ctx.fillStyle=p.muted;ctx.textAlign=i===0?'left':i===ticks-1?'right':'center';ctx.fillText(rows[index][0],x(index),height-10);
    }
    ctx.textAlign='left';
    const s=strip.ctx, sw=strip.width-left-right, cell=sw/rows.length, afterY=3, afterHeight=27;
    s.fillStyle=p.muted;s.fillText(text('Result','筛选结果'),0,20);
    s.fillStyle=p.grid;s.fillRect(left,afterY,sw,afterHeight);
    // At long ranges, union the reason bits within each visible pixel. Never
    // overwrite an earlier cause with the color of a later date.
    const grouped=rows.length>sw, count=grouped?Math.max(1,Math.floor(sw)):rows.length, bins=[];
    rows.forEach((row,index)=>{
      const slot=grouped?Math.min(count-1,Math.floor(index/rows.length*count)):index;
      const bin=bins[slot]||(bins[slot]={retained:true,failed:0,missing:0});
      bin.retained=bin.retained&&row[4];bin.failed|=row[5];bin.missing|=row[6];
    });
    bins.forEach((bin,index)=>{
      const bx=left+index/count*sw, bw=sw/count;
      if(bin.retained){s.fillStyle=p.keep;s.fillRect(bx,afterY,bw,afterHeight);return;}
      // Divide the whole date cell equally among its actual exclusion causes.
      const causes=metrics.map((m,i)=>i).filter(i=>(bin.failed|bin.missing)&(1<<i));
      if(!causes.length){s.fillStyle=p.missing;s.fillRect(bx,afterY,bw,afterHeight);return;}
      s.fillStyle=p.series[causes[0]];s.fillRect(bx,afterY,bw,afterHeight);
      const bandHeight=afterHeight/causes.length;
      causes.forEach((i,position)=>{
        const bit=1<<i;
        const by=afterY+position*bandHeight;s.fillStyle=p.series[i];s.fillRect(bx,by,bw,bandHeight);
        if(bin.missing&bit){
          s.save();s.beginPath();s.rect(bx,by,bw,bandHeight);s.clip();s.strokeStyle='#FFFFFF';s.globalAlpha=.65;s.lineWidth=1;
          for(let dx=-bandHeight;dx<bw;dx+=5){s.beginPath();s.moveTo(bx+dx,by+bandHeight);s.lineTo(bx+dx+bandHeight,by);s.stroke();}s.restore();
        }
      });
    });
    if(selected>=viewStart&&selected<viewEnd){s.strokeStyle=p.ink;s.strokeRect(left+(selected-viewStart)*cell,1,Math.max(cell,2),31);}
    s.fillStyle=p.muted;s.textAlign='left';s.fillText(rows[0][0],left,44);s.textAlign='right';s.fillText(rows[rows.length-1][0],left+sw,44);
    if(grouped){s.textAlign='left';s.fillText(text('Dates grouped by screen pixel. Zoom in to inspect individual dates.','当前按屏幕像素合并显示，可放大查看具体日期。'),0,61);}
  }
  function refresh(immediate=false) {
    clearTimeout(timer);
    if(!opened())return;
    request=String(++serial);busy=true;error='';tooltip.hidden=true;content();
    const id=request;
    timer=setTimeout(()=>{
      if(!opened()||request!==id)return;
      const payload=hooks.payload();
      hooks.post('space-weather-preview',{...payload,start:$('#startDate').value,end:$('#endDate').value,request:id});
    },immediate?0:220);
  }
  function handleMessage(message) {
    if(message.request!==request||!opened())return;
    busy=false;error=message.error||'';data=error?null:message;selected=-1;
    if(data){
      const key=`${message.start}|${message.end}`,total=data.rows.length;
      if(key!==rangeKey||viewEnd<=viewStart){viewStart=0;viewEnd=total;rangeKey=key;}
      else{const span=Math.min(total,viewEnd-viewStart);viewStart=Math.max(0,Math.min(total-span,viewStart));viewEnd=viewStart+span;}
    }
    content();
  }
  function inspect(event,canvas) {
    if(!data||busy||error||!data.rows.length)return;
    const box=canvas.getBoundingClientRect(), bounds=chartBounds;
    const index=bounds.start+Math.max(0,Math.min(bounds.count-1,Math.floor((event.clientX-box.left-bounds.left)/bounds.w*bounds.count)));
    selected=index;updateDay();scheduleDraw();
    const row=data.rows[index];tooltip.hidden=false;
    tooltip.innerHTML=`<strong>${row[0]} · ${status(row)}</strong><div class="weather-tooltip-metrics">${metrics.map((m,i)=>`<span style="color:${palette().series[i]}">${m.name} <b>${number(row[i+1])}</b>${m.unit?' '+m.unit:''}</span>`).join('')}</div><div class="weather-reason-chips">${reasonChips(row)}</div><p>${escape(reasons(row))}</p>`;
    const tipBox=tooltip.getBoundingClientRect();
    tooltip.style.left=Math.max(8,Math.min(window.innerWidth-tipBox.width-8,event.clientX+12))+'px';
    tooltip.style.top=Math.max(8,Math.min(window.innerHeight-tipBox.height-8,event.clientY+14))+'px';
  }
  function install(options) {
    hooks=options;
    const scroll=$('#dateAdvancedForm .settings-scroll');
    summary=document.createElement('section');summary.className='weather-preview-summary';summary.setAttribute('data-no-translate','');summary.id='weatherPreviewSummary';scroll.prepend(summary);
    panel=document.createElement('section');panel.className='weather-preview-panel';panel.id='weatherPreview';panel.setAttribute('data-no-translate','');
    panel.innerHTML=`<div class="weather-chart-heading"><p id="weatherChartLabel"></p><div id="weatherLegend"></div></div><div id="weatherMetricRules"></div><div class="weather-chart-tools"><div class="weather-zoom-buttons"><button type="button" id="weatherZoomIn"></button><button type="button" id="weatherZoomOut"></button><button type="button" id="weatherZoomReset"></button><button type="button" id="weatherPanLeft"></button><button type="button" id="weatherPanRight"></button></div><span id="weatherViewRange" aria-live="polite"></span></div><canvas id="weatherSeriesChart" tabindex="0"></canvas><canvas id="weatherDateStrip" tabindex="0"></canvas><div class="weather-range-navigation"><label for="weatherPanRange" id="weatherPanLabel"></label><input type="range" id="weatherPanRange" min="0" max="0" step="1" value="0"><span id="weatherPanHint"></span></div><p id="weatherSelectedDay" aria-live="polite"></p><p id="weatherPreviewNote"></p>`;
    scroll.append(panel);tooltip=document.createElement('div');tooltip.className='weather-preview-tooltip';tooltip.setAttribute('data-no-translate','');tooltip.hidden=true;document.body.append(tooltip);
    metrics.forEach(m=>{
      $('#'+m.mode).addEventListener('change',()=>refresh());
      [m.lower,m.upper].forEach(id=>$('#'+id).addEventListener('input',()=>refresh()));
    });
    $('#dateF107Basis').addEventListener('change',()=>refresh());
    $('#resetDateAdvanced').addEventListener('click',()=>setTimeout(()=>refresh(true),0));
    $('#weatherMetricRules').addEventListener('click',event=>{
      const button=event.target.closest('[data-weather-metric]');if(!button)return;
      const index=Number(button.dataset.weatherMetric);visible[index]=!visible[index];tooltip.hidden=true;content();$('#weatherMetricRules [data-weather-metric="'+index+'"]').focus({preventScroll:true});
    });
    $('#weatherZoomIn').addEventListener('click',()=>zoom(1));$('#weatherZoomOut').addEventListener('click',()=>zoom(-1));
    $('#weatherZoomReset').addEventListener('click',()=>changeView(0,data?.rows.length||0));
    $('#weatherPanLeft').addEventListener('click',()=>pan(-1));$('#weatherPanRight').addEventListener('click',()=>pan(1));
    $('#weatherPanRange').addEventListener('input',event=>changeView(Number(event.target.value),viewEnd-viewStart));
    for(const id of ['weatherSeriesChart','weatherDateStrip']){
      const canvas=$('#'+id);
      canvas.addEventListener('wheel',event=>{
        if(!data||busy||error||!data.rows.length)return;
        event.preventDefault();tooltip.hidden=true;
        if(event.shiftKey||Math.abs(event.deltaX)>Math.abs(event.deltaY)){changeView(viewStart+(event.deltaX||event.deltaY)/chartBounds.w*(viewEnd-viewStart),viewEnd-viewStart);return;}
        if(!event.deltaY)return;
        const box=canvas.getBoundingClientRect(),ratio=Math.max(0,Math.min(1,(event.clientX-box.left-chartBounds.left)/chartBounds.w));
        zoom(event.deltaY<0?1:-1,ratio,.8);
      },{passive:false});
      canvas.addEventListener('pointerdown',event=>{
        if(event.button!==0||!data||busy||error||!data.rows.length)return;
        drag={id:event.pointerId,x:event.clientX,start:viewStart,span:viewEnd-viewStart,moved:false};suppressClick=false;canvas.setPointerCapture(event.pointerId);
      });
      canvas.addEventListener('pointermove',event=>{
        if(drag&&event.pointerId===drag.id){
          const distance=event.clientX-drag.x;
          if(Math.abs(distance)>3)drag.moved=true;
          if(drag.moved){canvas.classList.add('weather-panning');changeView(drag.start-distance/chartBounds.w*drag.span,drag.span);}
          return;
        }
        if(event.pointerType!=='touch')inspect(event,canvas);
      });
      const finishDrag=()=>{if(drag){suppressClick=drag.moved;drag=null;}canvas.classList.remove('weather-panning');};
      canvas.addEventListener('pointerup',finishDrag);canvas.addEventListener('pointercancel',finishDrag);canvas.addEventListener('lostpointercapture',finishDrag);
      canvas.addEventListener('click',event=>{if(suppressClick){suppressClick=false;return;}inspect(event,canvas);canvas.focus({preventScroll:true});});
      canvas.addEventListener('pointerleave',()=>{tooltip.hidden=true;});
      canvas.addEventListener('keydown',event=>{
        if(!['ArrowLeft','ArrowRight','Home','End','PageUp','PageDown','+','=','-','0'].includes(event.key)||!data||busy||error||!data.rows.length)return;
        event.preventDefault();tooltip.hidden=true;
        if(event.key==='+'||event.key==='='){zoom(1);return;}if(event.key==='-'){zoom(-1);return;}if(event.key==='0'){changeView(0,data.rows.length);return;}
        if(event.key==='PageUp'||event.key==='PageDown'){pan(event.key==='PageUp'?-1:1);return;}
        const next=event.key==='Home'?0:event.key==='End'?data.rows.length-1:Math.max(0,Math.min(data.rows.length-1,(selected<0?viewStart:selected)+(event.key==='ArrowLeft'?-1:1)));
        const span=viewEnd-viewStart;if(next<viewStart)changeView(next,span);else if(next>=viewEnd)changeView(next-span+1,span);
        selected=next;updateDay();draw();
      });
    }
    new ResizeObserver(()=>draw()).observe(panel);
    new MutationObserver(()=>{if(opened())content();}).observe(document.documentElement,{attributes:true,attributeFilter:['data-theme']});
    window.addEventListener('resize',()=>{tooltip.hidden=true;draw();});
    scroll.addEventListener('scroll',()=>{tooltip.hidden=true;});
    content();
  }
  window.MarkWeatherPreview={install,open:()=>refresh(true),close:()=>{clearTimeout(timer);cancelAnimationFrame(drawFrame);drag=null;request=String(++serial);if(tooltip)tooltip.hidden=true;},handleMessage,repaint:()=>{if(panel)content();}};
})();
