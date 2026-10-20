/* Check the rendered application before creating a distribution. */
'use strict';
const fs=require('node:fs'),path=require('node:path'),http=require('node:http'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const root=path.resolve(__dirname,'..');
const ui=process.argv[2]?path.resolve(process.argv[2]):path.join(root,'resources/ui');
const resources=path.dirname(ui);
let chromium;
try { ({chromium}=require('playwright')); }
catch { throw new Error('Release validation requires Node.js and Playwright. Install Playwright before packaging.'); }
const names=['state','uiPrefs','applyLanguage','translateDom','localizeUiText','setMode','handleState','renderCountries','updateCountryTrigger','renderCountryPicker','renderSourcePicker','renderActiveFilters','updateChoiceTrigger','openAdvancedChoicePicker','renderDownloadPlan','updateDownloadDock','showStationDetail','closeStationDetail','closeAdvancedChoicePicker','hideMapStationTooltip','showProductDetail','renderMapStationTooltip','addInfo','renderInfo','renderClkAntex','renderUpdInputs','setCustomPresetEditor','renderCustomPresetLibrary','renderQuickPresetOptions','normalizedObservation','equipmentPeriodMarkup','setHtml'];
const source=fs.readFileSync(path.join(ui,'ui.js'),'utf8');
assert(!source.includes('out.split(from).join(to)'),'Fragment translation must not be reintroduced.');
const instrumented=source.replace('  window.MarkWeatherPreview.install(',`  window.__localeCheck={${names.join(',')}};\n  window.MarkWeatherPreview.install(`);
assert.notEqual(instrumented,source);
const mapData=JSON.parse(fs.readFileSync(path.join(resources,'ne_50m_admin_0_countries.geojson'),'utf8'));
const expectedMap={land:0,coastline:0,boundary:0};
for(const feature of mapData.features)if(feature.properties?.MARK_KIND in expectedMap)expectedMap[feature.properties.MARK_KIND]++;
async function checkMap(page){
 await page.waitForFunction(()=>document.querySelectorAll('#countryLayer .map-reference-border').length>0);
 const result=await page.evaluate(()=>{
  const groups={land:[...document.querySelectorAll('#landLayer .map-reference-land')],coastline:[...document.querySelectorAll('#countryLayer .map-reference-coast')],boundary:[...document.querySelectorAll('#countryLayer .map-reference-border')]};
  const grid=[...document.querySelectorAll('#graticuleLayer path')];
  const paths=[...Object.values(groups).flat(),...grid];
  return {counts:Object.fromEntries(Object.entries(groups).map(([key,items])=>[key,items.length])),grid:grid.length,
   invalid:paths.filter(node=>node.namespaceURI!=='http://www.w3.org/2000/svg'||!(node instanceof SVGPathElement)||!node.getAttribute('d')||!Number.isFinite(node.getTotalLength())||node.getTotalLength()<=0).length,
   invisible:[...groups.coastline,...groups.boundary,...grid].filter(node=>{const style=getComputedStyle(node);return style.stroke==='none'||Number(style.strokeWidth.replace('px',''))<=0||Number(style.opacity)===0;}).length};
 });
 assert.deepEqual(result.counts,expectedMap,'Map layers must retain every source feature.');
 assert(result.grid>100,'The graticule must be rendered.');
 assert.equal(result.invalid,0,'Map paths must be drawable SVG elements.');
 assert.equal(result.invisible,0,'Map lines must have visible strokes.');
 return result;
}
function nativeMessages(){
 const file=path.join(root,'src/MainWindow.cpp'),code=fs.readFileSync(file,'utf8'),rows=[];
 const literals=text=>[...text.matchAll(/(?:L?"(?:\\.|[^"\\])*"\s*)+/g)].map(match=>[...match[0].matchAll(/L?"((?:\\.|[^"\\])*)"/g)].map(value=>value[1]).join(''));
 for(const match of code.matchAll(/\b(?:SetOperationStatusText|SetQuerySummaryText)\s*\(/g)){
  let at=match.index+match[0].length,start=at,depth=1;
  while(depth&&at<code.length){
   if(code[at]==='"'){at++;while(at<code.length){if(code[at]==='\\'){at+=2;continue;}if(code[at++]==='"')break;}continue;}
   if(code[at]==='(')depth++;if(code[at]===')')depth--;at++;
  }
  rows.push(...literals(code.slice(start,at-1)));
 }
 for(const match of code.matchAll(/(?:std::wstring\s+(?:status|summary|resultSummary|preparationStatus|message)|status\s*\+=|summary\s*\+=|progress->message\s*=)[\s\S]{0,650}/g))rows.push(...literals(match[0].split(';')[0]));
 for(const match of code.matchAll(/\berror\s*=\s*L"((?:\\.|[^"\\])*)"/g))rows.push(match[1]);
 for(const name of ['StationSelection.cpp','StationSelectionCoverage.cpp'])rows.push(...literals(fs.readFileSync(path.join(root,'src',name),'utf8')));
 return [...new Set(rows.filter(text=>/[\u3400-\u9fff]/.test(text)))].map(template=>{
  let values=[];
  if(/^(?:日期筛选：在|\{\} 至|查询完成：|日期范围 )/.test(template))values=['2024-02-01','2024-02-10'];
  if(template.startsWith('{}：{} 个测站'))values=['已应用选站方案'];
  if(template.startsWith('生成下载清单：{}'))values=['IGS'];
  if(template.startsWith('解压与 RINEX 转换{}'))values=['完成'];
  let index=0;return template.replace(/\{[^}]*\}/g,()=>values[index++]??'12');
 });
}
const server=http.createServer((request,response)=>{
 const name=decodeURIComponent(new URL(request.url,'http://localhost').pathname);
 const file=path.resolve(resources,'.'+name);
 if(!file.startsWith(resources+path.sep)){response.writeHead(403);response.end();return;}
 if(!fs.existsSync(file)||!fs.statSync(file).isFile()){response.writeHead(404);response.end();return;}
 const type=file.endsWith('.js')?'text/javascript':file.endsWith('.html')?'text/html':file.endsWith('.css')?'text/css':file.endsWith('.json')||file.endsWith('.geojson')?'application/json':'application/octet-stream';
 response.setHeader('Content-Type',type+'; charset=utf-8');
 response.end(file===path.join(ui,'ui.js')?instrumented:fs.readFileSync(file));
});
async function scan(page,label){
 const failures=await page.evaluate(()=>{
  window.__localeCheck.translateDom(document.body);
  const ignored=node=>node.closest('script,style,[data-no-translate],#languageSwitch [data-lang="zh"]');
  const rows=[],walker=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);
  while(walker.nextNode()){
   const node=walker.currentNode;if(!node.parentElement||ignored(node.parentElement))continue;
   if(/[\u3400-\u9fff]/.test(node.nodeValue))rows.push({id:node.parentElement.closest('[id]')?.id,text:node.nodeValue.trim().slice(0,220)});
  }
  for(const node of document.querySelectorAll('[title],[aria-label],[placeholder]')){
   if(ignored(node))continue;
   for(const attr of ['title','aria-label','placeholder'])if(/[\u3400-\u9fff]/.test(node.getAttribute(attr)||''))rows.push({id:node.id||node.closest('[id]')?.id,attr,text:node.getAttribute(attr)});
  }
  return rows;
 });
 return failures.map(failure=>({state:label,...failure}));
}
async function main(){
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1920,height:1080}});
  const errors=[];page.on('pageerror',error=>errors.push(error.message));
  await page.addInitScript(()=>{window.__messages=[];window.chrome={webview:{postMessage(value){window.__messages.push(JSON.parse(value));},addEventListener(){}}};});
  await page.goto(`http://127.0.0.1:${server.address().port}/ui/index.html`,{waitUntil:'networkidle'});
  await page.waitForFunction(()=>window.__localeCheck);
  let snapshots=0;const failures=[];
  const check=async label=>{failures.push(...await scan(page,label));snapshots++;assert.deepEqual(errors,[],'Application errors');};
  await check('Initial interface');
  const mapChecks=[await checkMap(page)];
  const messages=nativeMessages();
  const untranslated=await page.evaluate(messages=>messages.map(source=>({source,english:window.__localeCheck.localizeUiText(source)})).filter(row=>/[\u3400-\u9fff]/.test(row.english)),messages);
  assert.deepEqual(untranslated,[],'Native message catalog');
  await page.evaluate(()=>{const node=document.createElement('div');node.id='locale-check-sentinel';node.textContent='Selected 4 个 countries';document.body.append(node);});
  assert((await scan(page,'Checker self-test')).some(row=>row.id==='locale-check-sentinel'),'The checker must reject mixed-language output.');
  await page.evaluate(()=>document.querySelector('#locale-check-sentinel').remove());
  await page.evaluate(()=>{
   const api=window.__localeCheck;
   const countries=['CHN','AUS','USA','FRA','DEU','JPN','CAN','ITA','NZL','GBR','MYT','ATF','IOT','ATA'];
   const networks=['IGS','MGEX','NGS','EPN','HK','GA-GDC','RGNA','GEONET','CACS','RING'].map((name,index)=>({name,available:index!==9,checked:index<4}));
   const row=['TEST','Test station','AUS','TRIMBLE NETR9','TRM59800.00','IGS',100,'Available',true,-30,140,true,'IGS','RINEX 3/4','30S','','','100.0% (10/10)','','GPS+GLO',100,'AUS','NONE'];
   window.__localeFixture={countries,networks,rows:[row],mapPoints:[['TEST',-30,140,true]],sources:4,availableSources:9,stations:1,visible:1,selected:1,
    startDate:'2024-02-01',endDate:'2024-02-10',preciseStartDate:'2024-02-01',preciseEndDate:'2024-02-10',
    folder:'C:/MARK/Downloads',preciseFolder:'C:/MARK/Products',country:'',advanced:{},dateAdvanced:{},observationQueue:[],preciseQueue:[],
    preciseProducts:[],status:'候选范围：1 / 1 个测站（1 个有坐标）。',preciseStatus:'等待任务'};
   api.handleState(window.__localeFixture);
  });
  await check('Query results');
  for(const count of [0,1,2,3,4,7,14]){
   await page.evaluate(count=>{
    const api=window.__localeCheck,fixture=window.__localeFixture;
    fixture.country=fixture.countries.slice(0,count).join(',');api.handleState(fixture);
    api.state.pendingCountries=api.state.selectedCountries.slice();api.state.countryNeedle='';api.renderCountryPicker();
    if(count>3){
     if(document.querySelector('#countrySelectionLabel').textContent!==`${count} selected`)throw new Error('Country button is not a complete English message');
     if(document.querySelector('#mapScope').textContent!==`${count} countries / regions`)throw new Error('Map scope is not a complete English message');
    }
    api.applyLanguage('zh');
    if(count>3&&document.querySelector('#countrySelectionLabel').textContent!==`已选择 ${count} 个国家/地区`)throw new Error('Chinese country count was not restored');
    api.applyLanguage('en');
   },count);
   await check('Country counts '+count);
  }
  for(const bounds of [false,true])for(const selectedOnly of [false,true]){
   await page.evaluate(({bounds,selectedOnly})=>{
    const api=window.__localeCheck,fixture=window.__localeFixture;
    Object.assign(fixture,{latMin:bounds?'-40':'',latMax:bounds?'20':'',lonMin:bounds?'100':'',lonMax:bounds?'180':'',rowsSelectedOnly:selectedOnly,
     filter:'TEST',advanced:{completenessMode:'ratio',rinexVersion:3,systemsInclude:'GPS',gimStationMode:'exclude',gimProviders:'IGS'},dateAdvanced:{kpMode:'max'}});
    api.handleState(fixture);
   },{bounds,selectedOnly});await check('Bounds and selected results');
  }
  for(const needle of ['','IGS','not-a-network']){
   await page.evaluate(needle=>{const api=window.__localeCheck;api.state.sourceNeedle=needle;api.renderSourcePicker(window.__localeFixture.networks,true);},needle);
   await check('Source search '+needle);
  }
  for(const needle of ['','MYT','no-matching-area']){
   await page.evaluate(needle=>{const api=window.__localeCheck;api.state.countryNeedle=needle;api.renderCountryPicker();},needle);await check('Country search '+needle);
  }
  for(const kind of ['systems','receivers','antennas','signals','gimProviders','gimProductGrades'])for(const count of [0,1,4]){
   await page.evaluate(({kind,count})=>{
    const api=window.__localeCheck,trigger=document.querySelector(`[data-choice-kind="${kind}"]`);
    if(!trigger)throw new Error('Missing choice kind '+kind);
    const input=document.getElementById(trigger.dataset.choiceInput);
    input.value=Array.from({length:count},(_,i)=>kind==='signals'?`GPS:C${i+1}C`:kind==='systems'?['GPS','GLO','GAL','BDS'][i]:`ITEM${i}`).join(',');
    if(kind==='gimProductGrades')document.querySelector('#advancedGimProviders').value='IGS';
    api.updateChoiceTrigger(input.id);api.openAdvancedChoicePicker(trigger);
   },{kind,count});await check('Advanced choices '+kind+' '+count);
  }
  for(const mode of ['observation','precise'])for(const status of ['pending','running','done','failed','cancelled']){
   await page.evaluate(({mode,status})=>{
    const api=window.__localeCheck,fixture=window.__localeFixture;
    fixture.observationQueue=[];fixture.preciseQueue=[];
    fixture[mode==='precise'?'preciseQueue':'observationQueue']=Array.from({length:4},(_,i)=>({id:String(i+1),label:'TEST'+i,start:'2024-02-01',end:'2024-02-10',status,selected:true,percent:50}));
    Object.assign(fixture,{downloading:mode==='observation'&&status==='running',preciseDownloading:mode==='precise'&&status==='running',paused:false,precisePaused:false});
    api.setMode(mode,false);document.querySelector('#downloadPlanLayer').classList.remove('hidden');api.state.planSignature=null;api.handleState(fixture);
   },{mode,status});await check('Download plan '+mode+' '+status);
  }
  await page.evaluate(()=>{
   const api=window.__localeCheck;api.setMode('observation',false);api.showStationDetail(api.normalizedObservation(window.__localeFixture.rows[0]));
   api.state.mapHoverStation='TEST';api.state.mapHoverDetail={network:'IGS',country:'AUS',interval:'30S',receiver:'TRIMBLE NETR9',system:'GPS'};api.state.mapHoverPosition={x:300,y:200};api.renderMapStationTooltip();
  });await check('Station details and map tooltip');
  for(const family of ['SP3','CLK','SINEX','ANTEX','UPD','VMF1']){
   await page.evaluate(family=>{
    const api=window.__localeCheck;
    api.showProductDetail({id:'TEST_'+family,family,center:'IGS',latency:'Final',interval:'30S',constellation:'GPS',percent:100,availability:'100.0% (10/10)',availableDays:10,preferredFileName:'TEST.sp3.gz',sources:'https://example.org/TEST.sp3.gz',status:'Available',localInventoryOnly:family==='UPD'});
   },family);await check('Product details '+family);
  }
  for(const percent of [0,60,100]){
   await page.evaluate(percent=>{
    const api=window.__localeCheck,fixture=window.__localeFixture;
    fixture.preciseProducts=['SP3','CLK','SINEX','ANTEX','UPD'].map((family,i)=>({id:'TEST_'+family,family,center:'IGS',latency:'Final',interval:'30S',constellation:'GPS',percent,availability:`${percent}% (6/10)`,selected:true,availableDays:6,preferredFileName:'TEST.sp3.gz',sources:'https://example.org/TEST.sp3.gz',status:'Available',localInventoryOnly:family==='UPD'}));
    api.setMode('precise',false);api.handleState(fixture);
   },percent);await check('Product results '+percent);
  }
  for(const count of [0,1,4]){
   await page.evaluate(count=>{
    const api=window.__localeCheck;
    api.state.customPresets=Array.from({length:count},(_,i)=>({id:String(i),name:i?'Preset '+i:'科研方案',rules:[{family:'sp3',latency:'final',center:'CODE'}]}));
    api.renderQuickPresetOptions();api.renderCustomPresetLibrary();api.setCustomPresetEditor(api.state.customPresets[0]);
    if(count){document.querySelector('#quickPreset').value='custom:0';api.renderActiveFilters();}
   },count);await check('Custom presets '+count);
  }
  for(const success of [false,true]){
   await page.evaluate(success=>{
    window.MarkSelection.onMessage({type:'station-selection-report',stale:false,summary:{status:success?'ok':'not-found',success,selected:4,requested:4,minimumAvailable:3,threshold:3,failingDays:success?0:1,activeDays:2,minimumDistanceKm:50,averageAvailable:3.5,excludedDays:0,unknownDays:0,message:success?'当前测站集合满足数量、距离与每日可用约束。':'当前测站集合尚未满足全部选站约束。'},days:[['2024-02-01',3,0,10,true,0],['2024-02-02',4,0,10,true,0]]});
   },success);await check('Station-set report '+success);
  }
  await page.evaluate(()=>{
   const api=window.__localeCheck;api.setMode('observation',false);api.showStationDetail(api.normalizedObservation(window.__localeFixture.rows[0]));
   api.setHtml(document.querySelector('#stationDetailBody'),api.equipmentPeriodMarkup({start:'2024-02-01',end:'2024-02-10',fields:[['接收机','TRIMBLE NETR9'],['天线','TRM59800.00']],codes:['GPS:C1C','GPS:L1C']}));
   window.GnssCoverage.mount(document.querySelector('#stationDetailBody'),{start:'2024-02-01',end:'2024-02-10',ranges:[['2024-02-01','2024-02-05','available'],['2024-02-06','2024-02-07','unknown'],['2024-02-08','2024-02-10','missing']]},true);
  });await check('Equipment and daily coverage');
  for(const error of ['', 'invalid-dates','invalid-conditions','index-error']){
   await page.evaluate(error=>{
    document.querySelector('#dateAdvancedLayer').classList.remove('hidden');window.MarkWeatherPreview.open();
    const request=window.__messages.filter(message=>message.request&&message.type.includes('weather')).at(-1)?.request;
    window.MarkWeatherPreview.handleMessage({request,error,start:'2024-02-01',end:'2024-02-10',requestedDays:3,retainedDays:1,missingDays:1,rows:[['2024-02-01',3,-20,100,true,0,0],['2024-02-02',6,-70,150,false,1,0],['2024-02-03',null,-10,120,false,0,1]]});
   },error);await check('Weather preview '+error);
  }
  for(const language of ['zh','en','zh','en']){
   await page.evaluate(language=>window.__localeCheck.applyLanguage(language),language);
   mapChecks.push(await checkMap(page));
   if(language==='en')await check('Language switch');
  }
  assert.deepEqual(errors,[],'Application errors');
  const unique=[...new Map(failures.map(row=>[row.id+'|'+row.attr+'|'+row.text,row])).values()];
  if(unique.length)throw new Error('Untranslated application text:\n'+JSON.stringify(unique,null,2));
  const digest=name=>crypto.createHash('sha256').update(fs.readFileSync(path.join(ui,name))).digest('hex');
  const report={passed:true,rendered_states:snapshots,native_messages:messages.length,labels_titles_placeholders:true,hidden_dialogs:true,fragment_translation_disabled:true,mixed_language_rejected:true,map_svg_checks:mapChecks.length,map_counts:expectedMap,map_paths_drawable:true,ui_source_sha256:digest('ui.js'),runtime_source_sha256:digest('runtime-log.js')};
  if(process.env.MARK_UI_CHECK_REPORT)fs.writeFileSync(process.env.MARK_UI_CHECK_REPORT,JSON.stringify(report,null,2));
  if(process.env.MARK_UI_CHECK_SCREENSHOT){
   await page.evaluate(()=>{
    const api=window.__localeCheck,fixture=window.__localeFixture;
    api.applyLanguage('en');api.setMode('observation',false);fixture.country='CHN,AUS,USA,FRA';fixture.latMin='';fixture.latMax='';fixture.lonMin='';fixture.lonMax='';api.handleState(fixture);
    document.querySelectorAll('.settings-layer').forEach(layer=>layer.classList.add('hidden'));
    api.closeStationDetail();api.closeAdvancedChoicePicker();api.hideMapStationTooltip();
   });
   await page.screenshot({path:process.env.MARK_UI_CHECK_SCREENSHOT});
  }
  console.log(JSON.stringify(report));
 }finally{await browser.close();}
}
main().catch(error=>{console.error(error.message);process.exitCode=1;}).finally(()=>server.close());
