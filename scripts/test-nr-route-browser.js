'use strict';
const assert = require('assert/strict');
const path = require('path');
const {pathToFileURL} = require('url');
const source = path.resolve(__dirname, '..');
const workspace = process.env.DXL_WORKSPACE || path.resolve(source, '../DXL-Workspace');
const {chromium} = require(path.join(workspace, 'tools/ui-qa/node_modules/playwright'));

(async () => {
    const browser = await chromium.launch({channel:'msedge', headless:true});
    try {
        for (const lang of ['zh', 'en']) {
            const page = await browser.newPage();
            const errors = [];
            page.on('pageerror', e => errors.push(e.message));
            await page.addInitScript(() => {
                window.chrome = {webview:{postMessage(){}, addEventListener(){}}};
            });
            await page.goto(pathToFileURL(path.join(source, 'src/ui/web/index.html')).href + '?systemLang=' + lang);
            await page.evaluate(lang => {
                onHostMessage(JSON.stringify({type:'settings', payload:{lang, scanned:true,
                    profiles:[{id:'default', name:'Default', settings:{}}]}}));
            }, lang);
            const status = async (route, nrState = 'active', attached = true) => {
                await page.evaluate(({route, nrState, attached}) => {
                    onHostMessage(JSON.stringify({type:'status', payload:{attached, pid:123,
                        target:'route-fixture.exe', api:'D3D12', nrRoute:route, nrState,
                        masterEnabled:true, nrAtEvaluateWanted:true, nrAtEvaluateFrames:123}}));
                }, {route, nrState, attached});
                return page.locator('#stNrWhere').textContent();
            };
            const paused = await status(3, 'standby');
            assert.match(paused, lang === 'en' ? /NR paused: waiting for native SR/ : /NR 暂停：等待游戏 SR 恢复/);
            assert.match(paused, lang === 'en' ? /FG state is unknown/ : /FG 状态未知/);
            assert.match(paused, lang === 'en' ? /fallback is unverified/ : /自动回退尚未验证/);
            assert.equal(await page.locator('#stNr').textContent(), lang === 'en' ? 'Pending' : '待生效');
            assert.match(await status(2), /Evaluate.*123/);
            assert.doesNotMatch(await page.locator('#stNrWhere').textContent(), /paused|暂停/);
            assert.match(await status(1), /Present/);
            assert.doesNotMatch(await status(2, 'disabled'), /paused|暂停/);
            assert.equal(await status(3, 'standby', false), '—');
            assert.deepEqual(errors, []);
            await page.close();
        }
        console.log('PASS NR route status: bilingual uncertain-FG pause, standby, Evaluate recovery, Present, disabled and detached states; no browser errors');
    } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
