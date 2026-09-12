'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {pathToFileURL} = require('url');
const source = path.resolve(__dirname, '..');
const workspace = process.env.DXL_WORKSPACE || path.resolve(source, '../DXL-Workspace');
const {chromium} = require(path.join(workspace, 'tools/ui-qa/node_modules/playwright'));
const output = process.argv[2] || path.join(workspace, 'diagnostics/launcher-profile-isolation');

(async () => {
    fs.mkdirSync(output, {recursive:true});
    const browser = await chromium.launch({channel:'msedge', headless:true});
    try {
        for (const lang of ['zh','en']) {
            const page = await browser.newPage({viewport:{width:1440,height:1080}});
            const errors = [];
            page.on('pageerror', error => errors.push(error.message));
            await page.addInitScript(() => {
                window.testMessages=[];
                window.chrome={webview:{postMessage(text){testMessages.push(JSON.parse(text));},addEventListener(){}}};
            });
            await page.goto(pathToFileURL(path.join(source,'src/ui/web/index.html')).href);
            await page.evaluate(lang => {
                onHostMessage({type:'settings',payload:{lang,watchGames:false,scanned:true,profiles:[
                    {id:'a',name:'Game A',exe:'a.exe',settings:{nrUseRealMotion:true,nrIntensity:0.25}},
                    {id:'b',name:'Game B',exe:'b.exe',settings:{nrUseRealMotion:true,nrIntensity:0.8}}
                ]}});
                activeId='a'; lastStatus={target:'b.exe',currentPid:22}; renderAll(); showPage('profile');
                testMessages.length=0;
            }, lang);
            await page.locator('#nrUseRealMotion').uncheck({force:true});
            await page.evaluate(() => { activeId='b'; renderAll(); liveApplyNow(); });
            const route = await page.evaluate(() => ({a:draft.profiles[0].settings.nrUseRealMotion,
                b:draft.profiles[1].settings.nrUseRealMotion,
                files:testMessages.filter(m=>m.type==='applyProfile').map(m=>m.file)}));
            assert.equal(route.a,false); assert.equal(route.b,true);
            assert.ok(route.files.includes('a.exe.json'));
            await page.evaluate(() => { activeId='a'; renderAll(); persistAll(); testMessages.length=0; });
            await page.locator('#masterEnabled').check({force:true});
            assert.equal(await page.evaluate(() => testMessages.find(m=>m.type==='setMaster').file),'a.exe.json');
            await page.locator('#nrExternalEdit').check({force:true});
            await page.locator('#nrIntensity').fill('0.7');
            await page.evaluate(() => { flushLauncherNr(); persistAll(); });
            assert.equal(await page.evaluate(() => saved.profiles[0].settings.nrIntensity),0.25);
            await page.evaluate(() => {
                const request=testMessages.filter(m=>m.type==='editNrParameter').at(-1);
                onHostMessage({type:'nrParameterEdited',payload:{...request,ok:false}});
                persistAll();
            });
            assert.equal(await page.locator('#nrIntensity').inputValue(),'0.25');
            assert.equal(await page.evaluate(() => saved.profiles[0].settings.nrIntensity),0.25);
            await page.locator('#nrIntensity').fill('0.6');
            await page.evaluate(() => {
                flushLauncherNr();
                const request=testMessages.filter(m=>m.type==='editNrParameter').at(-1);
                onHostMessage({type:'nrParameterEdited',payload:{...request,ok:true}});
            });
            assert.equal(await page.evaluate(() => saved.profiles[0].settings.nrIntensity),0.6);
            await page.locator('#nrIntensity').scrollIntoViewIfNeeded();
            await page.screenshot({path:path.join(output,`nr-editor-${lang}.png`)});
            assert.deepEqual(errors,[]);
            console.log(`PASS ${lang}: actual DOM input/change routing, per-profile master, NR failure rollback/success persistence; no page errors`);
            await page.close();
        }
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode=1; });
