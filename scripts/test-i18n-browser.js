'use strict';
// Runs against Edge with an isolated mock host; never reads or writes player settings.
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const {pathToFileURL} = require('url');
const source = path.resolve(__dirname, '..');
const workspace = process.env.DXL_WORKSPACE || path.resolve(source, '../DXL-Workspace');
const {chromium} = require(path.join(workspace, 'tools/ui-qa/node_modules/playwright'));
const output = path.join(workspace, 'diagnostics/0.1-release');
const html = process.argv[2] || path.join(source, 'src/ui/web/index.html');
(async()=>{
    fs.mkdirSync(output,{recursive:true});
    const browser=await chromium.launch({channel:'msedge',headless:true});
    try {
        for(const [native,saved,expected,locale] of [['en','auto','en','zh-CN'],['zh','auto','zh','en-US'],['zh','en','en','zh-CN'],['en','zh','zh','en-US']]) {
            const page=await browser.newPage({locale,viewport:{width:1560,height:1000}});
            const errors=[];page.on('pageerror',e=>errors.push(e.message));
            await page.addInitScript(()=>{
                window.testMessages=[];
                window.chrome={webview:{postMessage(s){testMessages.push(JSON.parse(s));},addEventListener(){}}};
            });
            await page.goto(pathToFileURL(html).href+'?systemLang='+native);
            await page.evaluate(lang=>onHostMessage(JSON.stringify({type:'settings',payload:{lang,watchGames:false,scanned:true}})),saved);
            assert.equal(await page.evaluate(()=>I18N.lang()),expected);
            const persisted=await page.evaluate(()=>testMessages.filter(m=>m.type==='applySettings'));
            if(saved==='auto') assert.ok(persisted.some(m=>m.payload?.lang===expected), 'first resolved language is saved');
            await page.evaluate(()=>{
                onHostMessage(JSON.stringify({type:'hotkeysRegistered',payload:{toggleInject:'Ctrl + F10'}}));
                showPage('profile');
            });
            assert.match(await page.locator('#adviceText').innerText(),/Ctrl \+ F10/);
            assert.match(await page.locator('#hintInjectKey').innerText(),/Ctrl \+ F10/);
            await page.evaluate(()=>{
                draft.hotkeys.toggleInject='F6';renderAll();
            });
            assert.match(await page.locator('#adviceText').innerText(),/Ctrl \+ F10/,'unregistered draft must not appear as registered');
            await page.evaluate(()=>onHostMessage(JSON.stringify({type:'hotkeysRegistered',payload:{toggleInject:'F6'}})));
            assert.match(await page.locator('#adviceText').innerText(),/F6/);
            await page.evaluate(()=>{
                draft.hotkeys.toggleAll='F7';draft.hotkeys.toggleOverlay='F8';renderAll();
            });
            assert.match(await page.locator('#masterHotkeyHint').textContent(),/F7/);
            assert.match(await page.locator('#panelHotkeyHint').textContent(),/F8/);
            assert.match(await page.locator('#adviceText').innerText(),/F7.*F8.*F6/);
            assert.doesNotMatch(await page.locator('#adviceText').innerText(),/Alt \+ 9/);
            await page.locator('#projectLink').click();
            assert.ok((await page.evaluate(()=>testMessages)).some(m=>m.type==='openProjectPage'));
            assert.match(page.url(),/index\.html/,'project link stays outside the app webview');
            await page.evaluate(()=>{onHostMessage(JSON.stringify({type:'hotkeysRegistered',payload:{toggleInject:''}}));});
            assert.doesNotMatch(await page.locator('#adviceText').innerText(),/F6|Alt \+ 9/);
            if(expected==='en') {
                const missing=await page.evaluate(()=>{
                    const found=new Set();
                    function inspect(){
                        I18N.apply();
                        const walker=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);
                        for(let n=walker.nextNode();n;n=walker.nextNode())
                            if(!n.parentElement?.closest('script,style,[data-i18n-skip]')&&/\p{Script=Han}/u.test(n.nodeValue)) found.add(n.nodeValue.trim());
                        for(const el of document.querySelectorAll('[title],[placeholder],[aria-label]'))
                            for(const attr of ['title','placeholder','aria-label'])
                                if(/\p{Script=Han}/u.test(el.getAttribute(attr)||'')) found.add(el.getAttribute(attr));
                    }
                    for(const api of ['D3D12','D3D11'])for(const route of [0,1,2])for(const motion of [0,1,2])for(const block of [0,1,2,3,4,5,6,7,8,9]) {
                        onHostMessage(JSON.stringify({type:'status',payload:{attached:true,target:'fixture.exe',pid:123,currentPid:123,api,hooked:true,masterEnabled:false,srState:'active',nrState:'failed',injectedEarly:false,proxyActive:false,depthCandidates:2,stallCount:3,srSkippedFrames:0,nrSkippedFrames:2,eavesdropModules:2,eavesdropLookups:7,eavesdropFrames:14,eavesdropCapturedFrames:12,eavesdropCapturedWidth:1280,eavesdropCapturedHeight:720,nrAtEvaluateWanted:true,nrAtEvaluateFrames:77,nrAtEvaluateBlocked:block,nrRoute:route,nrMotionSource:motion,nrOpticalFlowMs:0.25,nrUsingRealMotion:motion===1,evaluateCount:50,nrEvaluateCount:20,evaluateFailures:1,nrEvaluateFailures:2,nrDll:true,frameMs:16.3}}));inspect();
                    }
                    for(const text of ['设置已保存','设置已应用','全局监控已关。','已保存：下次启动 DXL 时请求管理员权限。','已保存：下次启动 DXL 时使用普通权限。','正在扫描 Steam / Epic / GOG 的已安装游戏…']) appendLog(text);
                    inspect();
                    for(let i=0;i<3;i++){I18N.set('zh');I18N.set('en');inspect();}
                    return [...found];
                });
                assert.deepEqual(missing,[],'English text and attributes across route/status changes');
            }
            assert.deepEqual(errors,[],'browser errors');
            await page.close();
        }
        const page=await browser.newPage({viewport:{width:1440,height:960}});
        await page.addInitScript(()=>{window.chrome={webview:{postMessage(){},addEventListener(){}}};});
        await page.goto(pathToFileURL(html).href+'?systemLang=zh');
        for(const lang of ['zh','en']) {
            await page.evaluate(lang=>{onHostMessage(JSON.stringify({type:'settings',payload:{lang,scanned:true,watchGames:false}}));showPage('library');},lang);
            await page.screenshot({path:path.join(output,'library-'+lang+'.png')});
            await page.setViewportSize({width:1000,height:720});
            await page.screenshot({path:path.join(output,'library-'+lang+'-compact.png')});
            await page.setViewportSize({width:1440,height:960});
        }
        console.log('PASS browser localization, OS language/save policy, native hotkey acknowledgements, live hints and external project action; screenshots saved outside source.');
    } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
