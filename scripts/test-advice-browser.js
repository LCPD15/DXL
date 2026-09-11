'use strict';
// Exercise real advice rendering and button events with a mock WebView host.
// No native launch, injection, or player configuration is accessed.
const assert = require('assert/strict');
const path = require('path');
const {pathToFileURL} = require('url');
const source = path.resolve(__dirname, '..');
const workspace = process.env.DXL_WORKSPACE || path.resolve(source, '../DXL-Workspace');
const {chromium} = require(path.join(workspace, 'tools/ui-qa/node_modules/playwright'));

(async () => {
    const browser = await chromium.launch({channel: 'msedge', headless: true});
    try {
        for (const lang of ['zh', 'en']) {
            const page = await browser.newPage();
            const errors = [];
            page.on('pageerror', error => errors.push(error.message));
            await page.addInitScript(() => {
                window.testMessages = [];
                window.chrome = {webview: {
                    postMessage(raw) { testMessages.push(JSON.parse(raw)); },
                    addEventListener() {}
                }};
            });
            await page.goto(pathToFileURL(path.join(source, 'src/ui/web/index.html')).href + '?systemLang=' + lang);
            await page.evaluate(lang => {
                onHostMessage({type: 'settings', payload: {lang, scanned: true, watchGames: false,
                    profiles: [{id: 'fixture', name: 'Advice fixture', exe: 'fixture.exe',
                        exePath: 'X:/Fixture/fixture.exe', args: '-dx12', settings: {srEnable: false}}]}});
                showPage('profile');
            }, lang);
            const status = async update => page.evaluate(update => onHostMessage({type: 'status', payload: {
                attached: true, target: 'fixture.exe', pid: 123, api: 'D3D12', hooked: true,
                injectedEarly: false, depthCandidates: 0, proxyActive: false, ...update
            }}), update);
            const button = page.locator('#adviceActions button');

            // This branch reproduced "t is not defined" before the fix.
            await status({});
            assert.equal(await button.count(), 1, 'late injection offers one launch action');
            assert.equal(await button.textContent(), lang === 'en' ? 'Launch from tool' : '从工具启动游戏');
            assert.match(await page.locator('#adviceText').textContent(), lang === 'en' ? /Injected a bit late,.*native depth/ : /注入偏晚，原生深度/);
            await button.click();
            const launches = await page.evaluate(() => testMessages.filter(message => message.type === 'launch'));
            assert.equal(launches.length, 1, 'action forwards exactly once to the existing launch control');
            assert.equal(launches[0].exePath, 'X:/Fixture/fixture.exe');
            assert.equal(launches[0].args, '-dx12');

            // Both advice branches render the action, and language changes must
            // rebuild it without relying on the translation observer to fix it.
            await page.evaluate(() => { activeProfile().settings.srEnable = true; activeProfile().settings.srMode = 'upscale'; });
            await status({injectedEarly: true, depthCandidates: 3});
            assert.equal(await page.locator('#advice').getAttribute('data-level'), 'warn');
            assert.match(await page.locator('#adviceText').textContent(), lang === 'en' ? /True upscaling not in effect/ : /真超分未生效/);
            await page.evaluate(lang => I18N.set(lang === 'en' ? 'zh' : 'en'), lang);
            assert.equal(await button.textContent(), lang === 'en' ? '从工具启动游戏' : 'Launch from tool');
            await page.evaluate(lang => I18N.set(lang), lang);
            assert.equal(await button.textContent(), lang === 'en' ? 'Launch from tool' : '从工具启动游戏');

            // No path means no unusable action; repeated statuses must remove
            // stale controls once recovered or detached.
            await page.evaluate(() => { activeProfile().exePath = ''; renderAdvice(lastStatus); });
            assert.equal(await button.count(), 0);
            await page.evaluate(() => { activeProfile().exePath = 'X:/Fixture/fixture.exe'; });
            await status({injectedEarly: true, depthCandidates: 3, proxyActive: true});
            assert.equal(await button.count(), 0);
            assert.equal(await page.locator('#adviceText').textContent(), lang === 'en' ? 'All good.' : '运行正常。');
            await status({attached: false});
            assert.equal(await button.count(), 0);
            await page.evaluate(() => showPage('library'));
            await status({});
            assert.equal(await page.locator('#advice').isVisible(), false, 'library keeps advice hidden');

            // The launcher also supports running without the optional translator.
            const fallback = await page.evaluate(() => {
                delete window.I18N;
                renderAdvice(lastStatus);
                // Read before the previously installed translator's observer runs.
                return document.querySelector('#adviceActions button').textContent;
            });
            assert.equal(fallback, '从工具启动游戏');
            assert.deepEqual(errors, [], 'no browser errors while rendering advice');
            await page.close();
        }
        console.log('PASS advice: late injection/upscale actions, Chinese/English switching, one launch dispatch, missing path, recovery/detach, library visibility and translator fallback');
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
