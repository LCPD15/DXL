'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {pathToFileURL} = require('url');
const source = path.resolve(__dirname, '..');
const workspace = process.env.DXL_WORKSPACE || path.resolve(source, '../DXL-Workspace');
const {chromium} = require(path.join(workspace, 'tools/ui-qa/node_modules/playwright'));
const output = path.join(workspace, 'diagnostics/game-compatibility-0.1');
(async()=>{
    fs.mkdirSync(output,{recursive:true});
    const browser = await chromium.launch({channel:'msedge',headless:true});
    try {
        for(const lang of ['zh','en']) {
            const page = await browser.newPage({viewport:{width:1560,height:1050}});
            const errors=[];page.on('pageerror',e=>errors.push(e.message));
            await page.addInitScript(()=>{
                window.testMessages=[];
                window.chrome={webview:{postMessage(s){testMessages.push(JSON.parse(s));},addEventListener(){}}};
            });
            await page.goto(pathToFileURL(path.join(source,'src/ui/web/index.html')).href+'?systemLang='+lang);
            await page.evaluate(lang=>{
                onHostMessage(JSON.stringify({type:'settings',payload:{lang,scanned:true,profiles:[
                    {id:'default',name:'Default',settings:{}},
                    {id:'yysls',name:'燕云十六声 / Where Winds Meet',exe:'yysls.exe',settings:{}},
                    {id:'re',name:'Onimusha: Way of the Sword',exePath:'X:/Games/ONIMUSHAWOTS.EXE',settings:{}},
                    {id:'other',name:'Resident Evil 4 (unrelated executable)',exe:'other.exe',settings:{}}
                ]}}));
                activeId='yysls';renderAll();showPage('profile');
            },lang);
            assert.equal(await page.locator('#fgBufferCountThreshold').isVisible(),false,'advanced remains collapsed');
            assert.match(await page.locator('#fgThresholdSummary').innerText(),lang==='zh'?/特调默认 8/:/tuned default: 8/,'tuned default is visible while collapsed');
            await page.locator('#fgThresholdDetails > summary').click();
            assert.equal(await page.locator('#fgBufferCountThreshold').inputValue(),'8','existing missing override uses tuned default');
            assert.match(await page.locator('#fgThresholdScope').innerText(),lang==='zh'?/特调默认 8/:/tuned default: 8/);
            await page.locator('#fgBufferCountThreshold').fill('4');
            await page.locator('#fgBufferCountThreshold').dispatchEvent('input');
            await page.evaluate(()=>{persistAll();const model=testMessages.filter(m=>m.type==='applySettings').at(-1).payload;
                onHostMessage(JSON.stringify({type:'settings',payload:model}));activeId='yysls';renderAll();});
            assert.equal(await page.locator('#fgBufferCountThreshold').inputValue(),'4','explicit old-default value survives reload');
            assert.match(await page.locator('#fgThresholdScope').innerText(),lang==='zh'?/特调默认 8/:/tuned default: 8/,'default label does not misreport selected custom value');
            await page.screenshot({path:path.join(output,`yysls-custom-${lang}.png`)});
            await page.evaluate(()=>{activeId='other';renderAll();});
            assert.equal(await page.locator('#fgBufferCountThreshold').inputValue(),'4','other games remain four');
            assert.equal(await page.locator('#reFrameworkSetup').isVisible(),false,'display game name alone cannot identify engine');
            await page.evaluate(()=>{activeId='re';renderAll();});
            const note=page.locator('#reFrameworkSetup');
            assert.equal(await note.isVisible(),true);
            assert.match(await note.innerText(),lang==='zh'?/RE引擎游戏需要先安装RE框架和ReShade才能正常使用。/:/RE Engine games require REFramework and ReShade/);
            assert.equal(await page.locator('#reFrameworkDownload').getAttribute('href'),'https://github.com/praydog/REFramework-nightly/releases');
            assert.equal(await page.locator('#reShadeDownload').getAttribute('href'),'https://www.reshade.me/#download');
            for(const width of [1560,1000,860]) {
                await page.setViewportSize({width,height:1050});
                await page.locator('#deleteProfile').scrollIntoViewIfNeeded();
                const layout=await page.locator('.profile-title-row').evaluate(el=>{
                    const rect=x=>{const r=x.getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height,right:r.right,bottom:r.bottom};};
                    return {row:rect(el),info:rect(el.querySelector('.profile-title-info')),note:rect(document.querySelector('#reFrameworkSetup')),
                        remove:rect(document.querySelector('#deleteProfile')),width:el.clientWidth,scroll:el.scrollWidth,
                        links:[...el.querySelectorAll('a')].map(x=>({rect:rect(x),color:getComputedStyle(x).color}))};
                });
                assert.ok(layout.remove.width>=160&&layout.remove.height>=48,'large delete button retained');
                assert.ok(layout.scroll<=layout.width+1,'header has no horizontal overflow');
                assert.ok(layout.info.right<=layout.remove.x-8,'notice does not overlap delete');
                assert.ok(layout.note.right<=layout.info.right+1,'notice stays inside title group');
                for(const link of layout.links) {
                    assert.ok(link.rect.right<=layout.note.right+1,'download link stays inside note');
                    assert.equal(link.color,'rgb(106, 183, 255)','download links are blue');
                }
                await page.screenshot({path:path.join(output,`re-setup-${lang}-${width}.png`)});
            }
            const original=page.url();
            await page.locator('#reFrameworkDownload').click();
            await page.locator('#reShadeDownload').click();
            assert.equal(page.url(),original,'download links leave launcher page intact');
            assert.deepEqual(await page.evaluate(()=>testMessages.filter(m=>/^openRe/.test(m.type)).map(m=>m.type)),['openReFrameworkPage','openReShadePage']);
            // Editing the actual executable path updates advice without changing
            // the stored display name or moving focus out of the path field.
            await page.locator('#profileExePath').fill('X:/Games/re4_tool.exe');
            assert.equal(await note.isVisible(),false,'similarly named executable is excluded');
            assert.equal(await page.locator('#profileExePath').evaluate(el=>document.activeElement===el),true);
            await page.locator('#profileExePath').fill('X:/Games/RE4.EXE');
            assert.equal(await note.isVisible(),true);
            await page.locator('#deleteProfile').click();
            assert.equal(await page.locator('#deleteProfileDialog').evaluate(el=>el.open),true);
            await page.keyboard.press('Escape');
            assert.equal(await page.evaluate(()=>draft.profiles.some(p=>p.id==='re')),true,'delete cancel preserves profile');
            assert.deepEqual(errors,[]);
            await page.close();
        }
        console.log('PASS bilingual game compatibility: yysls default/custom reload, exact executable advice, official download routing, 1560/1000/860 layout, large delete target and cancellation');
    } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
