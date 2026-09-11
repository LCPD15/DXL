'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {pathToFileURL} = require('url');
const source = path.resolve(__dirname, '..');
const workspace = process.env.DXL_WORKSPACE || path.resolve(source, '../DXL-Workspace');
const {chromium} = require(path.join(workspace, 'tools/ui-qa/node_modules/playwright'));
const output = path.join(workspace, 'diagnostics/fg-controls-0.1');
(async()=>{
    fs.mkdirSync(output, {recursive:true});
    const browser = await chromium.launch({channel:'msedge',headless:true});
    try {
        for (const lang of ['zh','en']) {
            const page = await browser.newPage({viewport:{width:1560,height:1050}});
            const errors=[]; page.on('pageerror',e=>errors.push(e.message));
            await page.addInitScript(()=>{
                window.testMessages=[];
                window.chrome={webview:{postMessage(s){testMessages.push(JSON.parse(s));},addEventListener(){}}};
            });
            await page.goto(pathToFileURL(path.join(source,'src/ui/web/index.html')).href+'?systemLang='+lang);
            await page.evaluate(lang=>{
                onHostMessage(JSON.stringify({type:'settings',payload:{lang,scanned:true,watchGames:true,profiles:[
                    {id:'default',name:'Default',settings:{}},
                    {id:'mafia',name:'Mafia: Definitive Edition',exe:'mafiadefinitiveedition.exe',settings:{injectTiming:'auto'}},
                    {id:'running',name:'Other game',exe:'running.exe',settings:{injectTiming:'early'}}
                ]}}));
                activeId='mafia'; renderAll(); showPage('profile');
                onHostMessage(JSON.stringify({type:'status',payload:{attached:true,pid:123,target:'running.exe',api:'D3D12'}}));
                activeId='mafia'; renderAll();
            },lang);
            const checked=()=>page.locator('input[name="injectTiming"]:checked').count();
            assert.equal(await checked(),1);
            assert.equal(await page.locator('#timingAuto').isChecked(),true);
            for(const [id,value] of [['timingLate','late'],['timingEarly','early'],['timingAuto','auto']]) {
                await page.locator('#'+id).locator('..').click();
                assert.equal(await checked(),1);
                assert.equal(await page.locator('#'+id).isChecked(),true);
                const result=await page.evaluate(()=>({selected:activeProfile().settings.injectTiming,
                    other:draft.profiles.find(p=>p.id==='running').settings.injectTiming,
                    watch:testMessages.filter(m=>m.type==='watchList').at(-1)}));
                assert.equal(result.selected,value);
                assert.equal(result.other,'early','editing selected timing must not alter the running profile');
                assert.equal(result.watch.lateNames,value==='early'?'':'mafiadefinitiveedition.exe');
                await page.evaluate(()=>renderAll());
                assert.equal(await page.locator('#'+id).isChecked(),true,'render preserves enum value');
            }
            await page.locator('#timingAuto').focus();
            await page.keyboard.press('ArrowRight');
            assert.equal(await page.locator('#timingEarly').isChecked(),true,'native radio keyboard navigation');
            await page.evaluate(()=>{persistAll();const saved=testMessages.filter(m=>m.type==='applySettings').at(-1).payload;
                onHostMessage(JSON.stringify({type:'settings',payload:saved}));activeId='mafia';renderAll();});
            assert.equal(await page.locator('#timingEarly').isChecked(),true,'saved timing survives reload');
            for(const width of [1560,1000]) {
                await page.setViewportSize({width,height:1050});
                await page.locator('#deleteProfile').scrollIntoViewIfNeeded();
                const size=await page.locator('#deleteProfile').boundingBox();
                assert.ok(size.width>=160&&size.height>=48,'large delete target');
                await page.screenshot({path:path.join(output,`profile-${lang}-${width}.png`)});
                await page.locator('#injectTimingWarning').scrollIntoViewIfNeeded();
                const layout=await page.locator('.timing-options').evaluate(el=>({width:el.clientWidth,scroll:el.scrollWidth,
                    warning:getComputedStyle(document.querySelector('#injectTimingWarning')).color,
                    buttons:[...el.querySelectorAll('.timing-choice')].map(x=>{const r=x.getBoundingClientRect();return {y:r.y,width:r.width};})}));
                assert.ok(layout.scroll<=layout.width+1,'no timing overflow');
                assert.equal(new Set(layout.buttons.map(x=>Math.round(x.y))).size,1,'three buttons share a row');
                assert.equal(layout.warning,'rgb(239, 107, 107)');
                await page.screenshot({path:path.join(output,`timing-${lang}-${width}.png`)});
            }
            const advanced=page.locator('#fgThresholdDetails > summary');
            assert.equal(await page.locator('#fgBufferCountThreshold').isVisible(),false,'advanced threshold starts collapsed');
            await advanced.scrollIntoViewIfNeeded();
            await page.screenshot({path:path.join(output,`threshold-collapsed-${lang}.png`)});
            await advanced.focus();
            await page.keyboard.press('Enter');
            assert.equal(await page.locator('#fgBufferCountThreshold').isVisible(),true,'keyboard expands advanced threshold');
            await page.keyboard.press('Space');
            assert.equal(await page.locator('#fgBufferCountThreshold').isVisible(),false,'keyboard collapses advanced threshold');
            await page.keyboard.press('Enter');
            await page.locator('#fgBufferCountThreshold').fill('8');
            await page.locator('#fgBufferCountThreshold').dispatchEvent('input');
            assert.equal(await page.locator('#fgBufferCountThresholdOut').innerText(),'8');
            const threshold=await page.evaluate(()=>({selected:activeProfile().settings.fgBufferCountThreshold,
                other:resolvedSettings(draft.profiles.find(p=>p.id==='running')).fgBufferCountThreshold}));
            assert.equal(threshold.selected,8);assert.equal(threshold.other,4);
            await page.evaluate(()=>{persistAll();const model=testMessages.filter(m=>m.type==='applySettings').at(-1).payload;
                onHostMessage(JSON.stringify({type:'settings',payload:model}));activeId='mafia';renderAll();});
            assert.equal(await page.locator('#fgBufferCountThreshold').inputValue(),'8');
            await advanced.click();
            await advanced.click();
            assert.equal(await page.locator('#fgBufferCountThreshold').inputValue(),'8','collapsing advanced controls preserves the profile value');
            await page.locator('#fgBufferCountThreshold').scrollIntoViewIfNeeded();
            await page.screenshot({path:path.join(output,`threshold-${lang}.png`)});
            await page.locator('#deleteProfile').click();
            assert.equal(await page.locator('#deleteProfileDialog').evaluate(el=>el.open),true);
            await page.keyboard.press('Escape');
            assert.equal(await page.evaluate(()=>draft.profiles.some(p=>p.id==='mafia')),true);
            assert.deepEqual(errors,[]);
            await page.close();
        }
        console.log('PASS timing buttons: mutual exclusion, keyboard, reload, selected/running profile isolation, immediate watch policy, bilingual layout and enlarged confirmed delete');
    } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
