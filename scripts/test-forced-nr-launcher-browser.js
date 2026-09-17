'use strict';
const assert=require('assert/strict');
const fs=require('fs');
const path=require('path');
const {pathToFileURL}=require('url');
const source=path.resolve(__dirname,'..');
const workspace=process.env.DXL_WORKSPACE || path.resolve(source,'../DXL-Workspace');
const {chromium}=require(path.join(workspace,'tools/ui-qa/node_modules/playwright'));
const output=process.argv[2] || path.join(workspace,'diagnostics/forced-nr-launcher');
(async()=>{
 fs.mkdirSync(output,{recursive:true});
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try {
  for(const lang of ['zh','en']) {
   const page=await browser.newPage({viewport:{width:1440,height:1080}});
   const errors=[];page.on('pageerror',e=>errors.push(e.message));
   await page.addInitScript(()=>{
    window.testMessages=[];
    window.chrome={webview:{postMessage(s){testMessages.push(JSON.parse(s));},addEventListener(){}}};
   });
   await page.goto(pathToFileURL(path.join(source,'src/ui/web/index.html')).href);
   await page.evaluate(lang=>{
    onHostMessage({type:'settings',payload:{lang,watchGames:false,scanned:true,profiles:[
     {id:'a',name:'Game A',exe:'a.exe',settings:{nrForceCompatibility:false,masterEnabled:false}},
     {id:'b',name:'Game B',exe:'b.exe',settings:{nrForceCompatibility:false}}
    ]},forcedNrPreferences:{'a.exe.json':true}});
    activeId='a';renderAll();showPage('profile');testMessages.length=0;
   },lang);
   const force=page.locator('#nrForceCompatibility');
   assert.equal(await force.isChecked(),true,'saved in-game preference overrides stale launcher model');
   assert.equal(await page.locator('#nrExternalEdit').isChecked(),false);
   await force.uncheck({force:true});
   await page.evaluate(()=>{
    flushLauncherNr();
    // Re-focusing or stale core status cannot overwrite an unacknowledged click.
    onHostMessage({type:'forcedNrPreferences',payload:{'a.exe.json':true}});
    const request=testMessages.filter(m=>m.type==='editNrParameter').at(-1);
    if(request.key!=='nrForceCompatibility'||request.file!=='a.exe.json'||request.value!==0)throw Error('wrong offline edit');
    onHostMessage({type:'nrParameterEdited',payload:{...request,ok:true}});
   });
   assert.equal(await force.isChecked(),false);
   assert.equal(await page.evaluate(()=>saved.profiles[0].settings.nrForceCompatibility),false);
   // Enabling must work without the emergency parameter-editor opt-in.
   await force.check({force:true});
   await page.evaluate(()=>{
    flushLauncherNr();activeId='b';renderAll();
    const request=testMessages.filter(m=>m.type==='editNrParameter').at(-1);
    onHostMessage({type:'nrParameterEdited',payload:{...request,ok:true}});
   });
   const saved=await page.evaluate(()=>structuredClone(saved));
   assert.equal(saved.profiles[0].settings.nrForceCompatibility,true);
   assert.equal(saved.profiles[0].settings.masterEnabled,true);
   assert.equal(saved.profiles[0].settings.dlss5Enable,true);
   assert.equal(saved.profiles[0].settings.nrProcessingEnabled,true);
   assert.equal(saved.profiles[1].settings.nrForceCompatibility,false,'target switch cannot change another game');
   await page.evaluate(()=>{
    activeId='a';renderAll();testMessages.length=0;
    lastStatus={target:'a.exe',currentPid:11};
    syncNrParamsFromCore({target:'a.exe',currentPid:11,nrParamVersion:12,nrParamForceCompatibility:false});
   });
   assert.equal(await force.isChecked(),false,'in-game off appears in launcher');
   assert.equal(await page.evaluate(()=>saved.profiles[0].settings.nrForceCompatibility),false);
   assert.equal(await page.evaluate(()=>testMessages.filter(m=>m.type==='applyProfile').every(m=>m.noReload===1)),true,'core changes never echo a reload');
   await force.check({force:true});
   await page.evaluate(()=>{
    flushLauncherNr();
    const request=testMessages.filter(m=>m.type==='editNrParameter').at(-1);
    onHostMessage({type:'nrParameterEdited',payload:{...request,ok:false}});
   });
   assert.equal(await force.isChecked(),false,'failed save rolls back');
   await page.evaluate(()=>{
    const model=structuredClone(saved);
    onHostMessage({type:'settings',payload:model,forcedNrPreferences:{'a.exe.json':false}});
    activeId='a';renderAll();showPage('profile');
   });
   assert.equal(await force.isChecked(),false,'recovered off survives restart');
   await page.locator('#launchSettingsCard').scrollIntoViewIfNeeded();
   const card=page.locator('#launchSettingsCard');
   if(lang==='en')assert.equal(/[\u3400-\u9fff]/.test(await card.innerText()),false,'English launcher card has no Chinese');
   assert.equal((await page.locator('#forcedNrHint').innerText()).includes('待测试'),false);
   await card.screenshot({path:path.join(output,`forced-launcher-${lang}.png`)});
   assert.deepEqual(errors,[]);
   console.log(`PASS ${lang}: saved-overlay startup, offline off recovery, activation, per-game isolation, live reverse sync, pending guard, failure rollback and reload`);
   await page.close();
  }
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
