'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert/strict');

const elements = new Map();
function element(id) {
    if (elements.has(id)) return elements.get(id);
    const node = { id, textContent: '', value: '', children: [], checked: false,
        get innerHTML() { return this.html || ''; },
        set innerHTML(value) { this.html = value; this.children = []; },
        disabled: false, hidden: false, type: '', options: [], dataset: {}, handlers: {},
        classList: { add() {}, remove() {}, contains() { return false; }, toggle() {} },
        appendChild(child) { this.children.push(child); }, append(...children) { this.children.push(...children); },
        setAttribute() {}, querySelectorAll() { return []; },
        showModal() { this.open = true; }, close() { this.open = false; this.handlers.close?.(); },
        addEventListener(event, handler) { this.handlers[event] = handler; } };
    elements.set(id, node);
    return node;
}
const messages = [];
const windowHandlers = {};
const context = vm.createContext({
    document: { documentElement: { setAttribute() {} }, getElementById: element,
        createElement: () => element('generated' + elements.size),
        querySelectorAll: () => [], querySelector: () => null, addEventListener() {} },
    window: { addEventListener(event, handler) { windowHandlers[event] = handler; }, chrome: { webview: {
        postMessage(value) { messages.push(JSON.parse(value)); }, addEventListener() {} } } },
    console, structuredClone, setTimeout: () => 1, clearTimeout() {}
});
const webDir = path.join(__dirname, '../src/ui/web');
vm.runInContext(fs.readFileSync(path.join(webDir, 'app.js'), 'utf8'), context);
const run = source => vm.runInContext(source, context);
const post = (type, payload) => context.onHostMessage(JSON.stringify({type, payload}));

const html = fs.readFileSync(path.join(webDir, 'index.html'), 'utf8');
assert.match(html, /<title>DXL/);
assert.match(html, /<input type="checkbox" id="adminLaunch"/);
assert.doesNotMatch(html, /<button[^>]+id="adminLaunch"/);
assert.equal(run('SETTING_DEFAULTS.nrSelfLayers'), 1);
assert.equal(run('SETTING_DEFAULTS.nrTrueLayers'), 1);
assert.equal(run('SETTING_DEFAULTS.nrOpticalFlow'), true);
assert.equal(run('SETTING_DEFAULTS.nrAutoRoute'), true);
assert.equal(run('SETTING_DEFAULTS.dlss5AtEvaluate'), true);
assert.equal(run('pruneDeadSettings({dlss5AtEvaluate:false}).dlss5AtEvaluate'), false);
assert.equal(run('pruneDeadSettings({nrSelfLayers:1.37}).nrSelfLayers'), 1.37);
assert.equal(run('pruneDeadSettings({nrSelfLayers:4}).nrSelfLayers'), undefined);
assert.equal(run('pruneDeadSettings({nrTrueLayers:2.8}).nrTrueLayers'), 3);

messages.length = 0;
element('adminLaunch').checked = true;
element('adminLaunch').handlers.change();
assert.equal(messages.length, 1);
assert.equal(messages[0].type, 'setAdminLaunch');
assert.equal(messages[0].on, 1);
assert.equal(element('adminLaunch').disabled, true);
post('launcherPreferences', {adminLaunch: true, elevated: false});
assert.equal(element('adminLaunch').checked, true);
assert.equal(element('adminLaunch').disabled, false);
element('adminLaunch').checked = false;
element('adminLaunch').handlers.change();
assert.equal(messages.at(-1).on, 0);
post('launcherPreferences', {adminLaunch: true}); // failed write acknowledges persisted value
assert.equal(element('adminLaunch').checked, true);
assert.equal(messages.some(m => m.type === 'applyProfile' || m.type === 'applySettings'), false);

post('settings', { theme: 'dark', lang: 'zh', watchGames: false, scanned: true,
    profiles: [
        {id:'one', name:'One', exe:'one.exe', exePath:'X:\\one.exe', settings:{}},
        {id:'two', name:'Two', exe:'two.exe', exePath:'X:\\two.exe', settings:{}}
    ] });
const status = (target, pid, selfLayers) => ({attached:true, target, pid, currentPid:pid,
    hooked:true, api:'D3D12', nrParamVersion:1, nrRoute:2, nrMotionSource:2, nrOpticalFlowMs:0.42,
    nrAtEvaluateWanted:true, nrAtEvaluateFrames:20, nrParamSelfLayers:selfLayers,
    nrParamTrueLayers:3, nrParamColourStrength:0.25, nrParamOpticalFlow:true,
    nrParamOpticalFlowQuality:0, nrParamSemanticMask:true, nrParamSemanticEnabled:0x20001,
    nrParamSemanticBg:0, nrParamSemanticIntensity0:0.7, nrParamSemanticIntensity17:0.8,
    nrParamSemanticDebug:false });
messages.length = 0;
post('status', status('one.exe', 101, 1.37));
assert.equal(element('nrRoSelfLayers').textContent, '1.37');
assert.equal(element('nrRoTrueLayers').textContent, '3');
assert.match(element('stNrWhere').textContent, /Evaluate/);
assert.match(element('stRealMotion').textContent, /光流.*0.42/);
assert.equal(run("saved.profiles[0].settings.nrSelfLayers"), 1.37);
assert.equal(run("saved.profiles[0].settings.nrSemInt17"), 0.8);
assert.equal(run("saved.profiles[0].settings.nrSemBgInt"), 0);
assert.equal(run("saved.profiles[0].settings.nrSemOn"), 0x20001);
assert.equal(messages.filter(m => m.type === 'applySettings').length, 1);
assert.equal(messages.find(m => m.type === 'applySettings').noReload, 1);
assert.equal(messages.find(m => m.type === 'applyProfile').noReload, 1);
messages.length = 0;
post('status', status('one.exe', 101, 1.1)); // Same version is an echo, not an edit
assert.equal(run("saved.profiles[0].settings.nrSelfLayers"), 1.37);
assert.equal(messages.some(m => m.type === 'applySettings' || m.type === 'applyProfile'), false);
post('status', status('two.exe', 102, 2.4)); // Same version, another target: do sync
assert.equal(run("saved.profiles[1].settings.nrSelfLayers"), 2.4);
assert.equal(run("saved.profiles[0].settings.nrSelfLayers"), 1.37);
assert.equal(messages.find(m => m.type === 'applyProfile').file, 'two.exe.json');
const resetStatus = { ...status('two.exe', 102, 1), nrParamVersion: 2,
    nrParamTrueLayers: 1, nrParamColourStrength: 0, nrParamOpticalFlow: false,
    nrParamSemanticMask: false, nrParamSemanticEnabled: 0, nrParamSemanticBg: 0 };
for (let i = 0; i < 18; ++i) resetStatus['nrParamSemanticIntensity' + i] = 0;
post('status', resetStatus);
const persistedModel = JSON.parse(run('JSON.stringify(saved)'));
post('settings', persistedModel); // Launcher settings reload must preserve zero/false/default endpoints
assert.equal(run("resolvedSettings(draft.profiles[1]).nrSelfLayers"), 1);
assert.equal(run("resolvedSettings(draft.profiles[1]).nrTrueLayers"), 1);
assert.equal(run("resolvedSettings(draft.profiles[1]).nrColourStrength"), 0);
assert.equal(run("resolvedSettings(draft.profiles[1]).nrOpticalFlow"), false);
assert.equal(run("resolvedSettings(draft.profiles[1]).nrSemanticMask"), false);
assert.equal(run("resolvedSettings(draft.profiles[1]).nrSemOn"), 0);
assert.equal(run("resolvedSettings(draft.profiles[1]).nrSemBgInt"), 0);
for (let i = 0; i < 18; ++i) assert.equal(run(`resolvedSettings(draft.profiles[1]).nrSemInt${i}`), 0);
messages.length = 0;
run('writeProfileFile(draft.profiles[1], true)');
const flat = messages.find(m => m.type === 'applyProfile').payload;
assert.equal(flat.nrColourStrength, 0);
assert.equal(flat.nrSemBgInt, 0);
assert.equal(flat.nrSemInt0, 0);
assert.equal(flat.nrSelfLayers, 1);
assert.equal(flat.nrTrueLayers, 1);
assert.equal(flat.nrAutoRoute, true);
assert.equal(flat.dlss5AtEvaluate, true);
console.log('PASS DXL branding, independent elevation checkbox, routing defaults, layer bounds, optical/semantic status, multi-target persistence without reload echoes');

assert.match(html, /id="scanGames"[\s\S]*id="cleanupInvalidGames"/);
assert.equal(run('MODEL_DEFAULTS.hotkeys.toggleAll'), 'Del');
assert.equal(run('MODEL_DEFAULTS.hotkeys.toggleOverlay'), 'End');
for (const [combo, vk, mods] of [['Del', 46, 0], ['Delete', 46, 0], ['End', 35, 0],
    ['Ctrl + End', 35, 2], ['Win + F24', 135, 8], ['X', 88, 0], ['Shift + Space', 32, 4]]) {
    assert.equal(run(`parseHotkeyForCore(${JSON.stringify(combo)}).vk`), vk);
    assert.equal(run(`parseHotkeyForCore(${JSON.stringify(combo)}).mods`), mods);
}
for (const combo of ['', 'Alt', 'F25', 'Del + End', 'Alt + Unknown', 'Del +'])
    assert.equal(run(`parseHotkeyForCore(${JSON.stringify(combo)}).vk`), 0);
post('settings', {watchGames:false, scanned:true, lang:'zh', hotkeys: {
    toggleAll:'Alt + D', toggleOverlay:'Alt + 0', toggleInject:'Ctrl + F12'},
    profiles:[{id:'default',exePath:'',settings:{}}]});
assert.equal(run('draft.hotkeys.toggleAll'), 'Del');
assert.equal(run('draft.hotkeys.toggleOverlay'), 'End');
assert.equal(run('draft.hotkeys.toggleInject'), 'Ctrl + F12');
assert.equal(run('draft.hotkeyRevision'), 0); // Does not override a newer per-game custom binding
messages.length = 0;
run('writeProfileFile(draft.profiles[0], true)');
let shortcutFlat = messages.find(m => m.type === 'applyProfile').payload;
assert.equal(shortcutFlat.hkEnable, 46);
assert.equal(shortcutFlat.hkEnableMods, 0);
assert.equal(shortcutFlat.hkUi, 35);
assert.equal(shortcutFlat.hkUiMods, 0);
assert.equal(shortcutFlat.hotkeyRevision, 0);
const captureButton = element('testShortcut');
captureButton.dataset.hotkey = 'toggleOverlay';
run("capturing = document.getElementById('testShortcut')");
windowHandlers.keydown({key:'End', preventDefault(){}, ctrlKey:false,altKey:false,shiftKey:false,metaKey:false});
assert.equal(run('draft.hotkeys.toggleOverlay'), 'End');
assert.ok(run('draft.hotkeyRevision') > 0);
post('settings', {watchGames:false, scanned:true, lang:'zh', hotkeys:{toggleAll:'Ctrl + K',toggleOverlay:'Home'},
    profiles:[{id:'default',exePath:'',settings:{}}]});
assert.equal(run('draft.hotkeys.toggleAll'), 'Ctrl + K');
assert.equal(run('draft.hotkeys.toggleOverlay'), 'Home');

post('settings', {watchGames:false, scanned:true, lang:'zh', profiles:[
    {id:'default', name:'Default', exePath:'', settings:{}},
    {id:'manual', name:'Manual', exePath:'', settings:{}},
    {id:'gone', name:'Gone', exe:'gone.exe', exePath:'C:\\old\\gone.exe', settings:{}},
    {id:'valid', name:'Valid', exe:'valid.exe', exePath:'C:\\valid.exe', settings:{}},
    {id:'changed', name:'Changed', exe:'changed.exe', exePath:'C:\\old.exe', settings:{}},
    {id:'offline', name:'Offline', exe:'offline.exe', exePath:'Z:\\offline.exe', settings:{}}
]});
run("activeId = 'gone'");
messages.length = 0;
element('cleanupInvalidGames').handlers.click();
assert.equal(element('cleanupInvalidGames').disabled, true);
assert.equal(messages.filter(m => m.type === 'cleanupInvalidGames').length, 1);
assert.ok(!messages[0].items.includes('default|'));
assert.ok(!messages[0].items.includes('manual|'));
run("draft.profiles.find(p => p.id === 'changed').exePath = 'C:\\\\new.exe'");
post('invalidGames', {missing:[{id:'gone',exePath:'C:\\old\\gone.exe'},
    {id:'changed',exePath:'C:\\old.exe'}, {id:'default',exePath:''}], unknown:1});
assert.equal(element('cleanupInvalidGames').disabled, false);
assert.equal(run("draft.profiles.some(p => p.id === 'gone')"), false);
assert.equal(run("draft.profiles.some(p => p.id === 'changed')"), true);
assert.equal(run("draft.profiles.some(p => p.id === 'offline')"), true);
assert.equal(run("draft.profiles.some(p => p.id === 'manual')"), true);
assert.equal(run('activeId'), 'default');
assert.equal(messages.find(m => m.type === 'applySettings').noReload, 1);
assert.equal(messages.some(m => m.type === 'applyProfile' || m.type === 'deleteProfile'), false);
assert.ok(messages.some(m => m.type === 'watchList'));
console.log('PASS single keys, stock default migration/custom preservation, hotkey revision, missing-game pruning with no reload or game-file deletion');

post('settings', {watchGames:false, scanned:true, lang:'zh', hotkeyRevision: 100,
    hotkeys:{toggleAll:'Alt + D', toggleOverlay:'Alt + 0'},
    profiles:[{id:'default',exePath:'',settings:{}}]});
assert.equal(run('draft.hotkeys.toggleAll'), 'Alt + D');
assert.equal(run('draft.hotkeys.toggleOverlay'), 'Alt + 0');
console.log('PASS explicit rebinds to previous stock keys survive settings reload');

const revisionBeforeLauncherKey = run('draft.hotkeyRevision');
captureButton.dataset.hotkey = 'toggleInject';
run("capturing = document.getElementById('testShortcut')");
windowHandlers.keydown({key:'F7', preventDefault(){},ctrlKey:false,altKey:false,shiftKey:false,metaKey:false});
assert.equal(run('draft.hotkeys.toggleInject'), 'F7');
assert.equal(run('draft.hotkeyRevision'), revisionBeforeLauncherKey);
captureButton.dataset.hotkey = 'toggleDebugView';
run("capturing = document.getElementById('testShortcut')");
windowHandlers.keydown({key:'F7', preventDefault(){},ctrlKey:false,altKey:false,shiftKey:false,metaKey:false});
assert.equal(run('draft.hotkeys.toggleDebugView'), 'Alt + V');
assert.equal(run('capturing !== null'), true);
assert.equal(run('draft.hotkeyRevision'), revisionBeforeLauncherKey);
console.log('PASS launcher-only shortcut edits preserve core revision and duplicate bindings are rejected');

assert.match(html, /id="launchBtn"[\s\S]*id="homeLaunchArgs"/);
post('settings', {watchGames:false, scanned:true, lang:'zh', profiles:[
    {id:'default', name:'Default', exePath:'', args:'-default-only', settings:{}},
    {id:'one', name:'One', exe:'one.exe', exePath:'C:\\Games with spaces\\游戏\\one.exe', args:'-old', settings:{}},
    {id:'two', name:'Two', exe:'two.exe', exePath:'C:\\two.exe', args:'', settings:{injectTiming:'late'}}
]});
run("activeId = 'one'; renderAll()");
const homeArgs = element('homeLaunchArgs');
const profileArgs = element('profileArgs');
assert.equal(homeArgs.value, '-old');
const editedLaunchArgs = '-force-d3d12 -title "two words" -path "C:\\保存 路径\\slot"';
homeArgs.value = editedLaunchArgs;
homeArgs.handlers.input({target:homeArgs});
assert.equal(profileArgs.value, editedLaunchArgs);
assert.equal(run('activeProfile().args'), editedLaunchArgs);
messages.length = 0;
element('launchBtn').handlers.click(); // Do not fire the 600ms autosave first.
const launched = messages.find(m => m.type === 'launch');
assert.equal(launched.args, editedLaunchArgs);
assert.equal(launched.exePath, 'C:\\Games with spaces\\游戏\\one.exe');
assert.equal(launched.timing, 'early');
assert.equal(messages.find(m => m.type === 'applySettings').payload.profiles[1].args, editedLaunchArgs);
assert.ok(messages.findIndex(m => m.type === 'applySettings') < messages.findIndex(m => m.type === 'launch'));
assert.equal(run('saved.profiles[1].args'), editedLaunchArgs);
run("activeId = 'two'; renderAll()");
assert.equal(homeArgs.value, ''); // Default profile launch args must never be inherited.
assert.equal(profileArgs.value, '');
profileArgs.value = '-second';
profileArgs.handlers.input({target:profileArgs});
assert.equal(homeArgs.value, '-second');
assert.equal(run('draft.profiles[1].args'), editedLaunchArgs);
assert.equal(run('draft.profiles[0].args'), '-default-only');
messages.length = 0;
run('launchProfile(activeProfile())'); // Sidebar uses the same launch entry point.
assert.equal(messages.find(m => m.type === 'launch').args, '-second');
assert.equal(messages.find(m => m.type === 'launch').timing, 'late');
const launchSavedModel = JSON.parse(run('JSON.stringify(saved)'));
post('settings', launchSavedModel);
run("activeId = 'one'; renderAll()");
assert.equal(homeArgs.value, editedLaunchArgs);
homeArgs.value = '';
homeArgs.handlers.input({target:homeArgs});
messages.length = 0;
element('launchBtn').handlers.click();
assert.equal(messages.find(m => m.type === 'launch').args, '');
assert.equal(profileArgs.value, '');
console.log('PASS per-game launch arguments: home/profile sync, immediate launch persistence, quoted UTF-8 transport, profile isolation, empty args and reload');

assert.match(html, /class="page library-page is-active" data-page="library"/);
assert.match(html, /data-page="library"[\s\S]*id="profileList"/);
run("libraryMetadataAsked.clear(); libraryFilter = 'all'; profileFilter = ''; showPage('library')");
post('settings', {watchGames:false, scanned:true, lang:'zh', profiles:[
    {id:'default', name:'Default', exePath:'', args:'-not-inherited', settings:{}},
    {id:'steam', name:'Custom game name', exe:'steam.exe', exePath:'D:\\SteamLibrary\\steamapps\\common\\Game\\steam.exe', args:'-dx12', favorite:true, settings:{nrIntensity:0.23}},
    {id:'epic', name:'Epic Game', exe:'epic.exe', exePath:'D:\\Epic\\epic.exe', source:'Epic', pinned:false, settings:{}},
    {id:'manual', name:'Manual Game', exe:'manual.exe', exePath:'D:\\manual.exe', source:'Manual', settings:{}}
]});
assert.equal(run('currentPage'), 'library');
assert.equal(run('gameSource(draft.profiles[1])'), 'Steam');
assert.equal(run('draft.profiles[1].pinned'), true);
assert.equal(run('sortedProfiles().length'), 3);
assert.equal(element('profileList').children.length, 3);
messages.length = 0;
run('renderProfileList(); renderProfileList()');
assert.equal(messages.some(m => m.type === 'requestLibraryMetadata'), false); // Same paths already requested once.
post('libraryMetadata', {steam:{exePath:'D:\\SteamLibrary\\steamapps\\common\\Game\\steam.exe', source:'Steam', steamAppId:'620', coverUrl:'covercache/cover_portal.jpg'}});
assert.equal(run('draft.profiles[1].steamAppId'), '620');
assert.equal(run('draft.profiles[1].args'), '-dx12');
assert.equal(run('draft.profiles[1].settings.nrIntensity'), 0.23);
assert.equal(run('coverCandidates(draft.profiles[1])[0]'), 'covercache/cover_portal.jpg');
assert.match(run('coverCandidates(draft.profiles[1])[1]'), /^https:\/\/shared\.fastly\.steamstatic\.com\/store_item_assets\/steam\/apps\/620\/library_600x900\.jpg$/);
post('libraryMetadata', {steam:{exePath:'D:\\old.exe', source:'Epic', steamAppId:'999'}});
assert.equal(run('draft.profiles[1].steamAppId'), '620');
post('libraryMetadata', {removed:{source:'Steam', steamAppId:'1'}});
assert.equal(run('draft.profiles.length'), 4); // Metadata must not restore removed entries.
post('gameCover', {id:'steam', coverUrl:'covercache/custom.jpg'});
post('gameCover', {id:'steam', exePath:'D:\\old.exe', coverUrl:'covercache/stale.jpg'});
assert.equal(run('draft.profiles[1].coverUrl'), 'covercache/custom.jpg');
post('gameCover', {id:'steam', exePath:'d:\\steamlibrary\\steamapps\\common\\game\\steam.exe', coverUrl:'covercache/custom.jpg'});
post('libraryMetadata', {steam:{coverUrl:'covercache/automatic.jpg'}});
assert.equal(run('draft.profiles[1].coverUrl'), 'covercache/custom.jpg');
post('gameCover', {id:'steam', coverUrl:'javascript:alert(1)'});
assert.equal(run('draft.profiles[1].coverUrl'), 'covercache/custom.jpg');
post('scannedGames', {games:[{exePath:'d:\\steamlibrary\\steamapps\\common\\game\\steam.exe', name:'Store title', source:'Steam', steamAppId:'620', exe:'steam.exe'}]});
assert.equal(run('draft.profiles.length'), 4);
assert.equal(run('draft.profiles[1].name'), 'Custom game name');
assert.equal(run('draft.profiles[1].args'), '-dx12');
assert.equal(run('draft.profiles[1].pinned'), true);
assert.equal(run('draft.profiles[1].settings.nrIntensity'), 0.23);
element('libraryFilter').handlers.change({target:{value:'favorites'}});
assert.equal(run('sortedProfiles().length'), 1);
element('libraryFilter').handlers.change({target:{value:'Epic'}});
assert.equal(run('sortedProfiles()[0].id'), 'epic');
element('profileSearch').value = 'no match';
element('profileSearch').handlers.input();
assert.equal(run('sortedProfiles().length'), 0);
run("libraryFilter = 'all'; profileFilter = ''");
const steamCard = run('buildProfileRow(draft.profiles[1])');
const coverImage = steamCard.children[0].children[1];
assert.equal(coverImage.src, 'covercache/custom.jpg');
coverImage.handlers.error();
assert.match(coverImage.src, /library_600x900\.jpg$/);
coverImage.handlers.error();
assert.match(coverImage.src, /header\.jpg$/);
coverImage.handlers.error();
assert.equal(coverImage.hidden, true); // Every failed image leaves the usable icon/title card.
steamCard.children[1].children[1].children[0].handlers.click();
assert.equal(run('draft.profiles[1].pinned'), false);
steamCard.children[0].handlers.click();
assert.equal(run('currentPage'), 'profile');
assert.equal(run('activeId'), 'steam');
messages.length = 0;
element('changeGameCover').handlers.click();
assert.equal(messages[0].type, 'pickGameCover');
assert.equal(messages[0].id, 'steam');
assert.equal(messages[0].exePath, 'D:\\SteamLibrary\\steamapps\\common\\Game\\steam.exe');
element('backToLibrary').handlers.click();
assert.equal(run('currentPage'), 'library');
element('defaultProfile').handlers.click();
assert.equal(run('currentPage'), 'profile');
assert.equal(run('activeId'), 'default');
assert.equal(element('changeGameCover').disabled, true);
run("draft.profiles[1].exePath = 'D:\\\\new\\\\steam.exe'");
messages.length = 0;
run('requestLibraryMetadata(); requestLibraryMetadata()');
assert.equal(messages.filter(m => m.type === 'requestLibraryMetadata').length, 1);
console.log('PASS library platform groups, favorite migration/filter, search, detail navigation, metadata refresh/isolation, custom covers and complete image fallback');

run("libraryFilter='all'; profileFilter=''; draft.profiles.find(p=>p.id==='epic').pinned=true; renderProfileList()");
const groups=element('profileList').children.filter(e=>e.className==='library-group');
const labels=groups.map(g=>g.children[0].textContent);
assert.match(labels[0], /收藏/);
assert.match(labels[1], /手动添加/);
assert.ok(labels.indexOf(labels.find(x=>x.startsWith('Steam'))) > 1);
assert.equal(groups.flatMap(g=>g.children[1].children).length,run('sortedProfiles().length'));
const compact=run('buildProfileRow(draft.profiles[1])');
assert.equal(compact.children.length,2);
assert.equal(compact.children[1].className,'game-footer');
assert.equal(compact.children[1].children[0].children.length,1);
assert.equal(compact.children[1].children[1].children.length,2);
assert.equal(run('SETTING_DEFAULTS.nrSemInt0'),0);
console.log('PASS favorites/manual/store order without duplicates, compact title/favorite/play footer, zero semantic defaults');
messages.length=0;
run("showPage('extras')");
assert.equal(messages[0].type,'getExtensions');
assert.equal(element('actionsBar').hidden,true);
run("onHostMessage({type:'extensions',payload:{installed:false,downloadUrl:'',folder:'test-folder'}})");
assert.equal(element('downloadSemantic').disabled,true);
assert.match(element('semanticInstallStatus').textContent,/组件缺失/);
run("onHostMessage({type:'extensions',payload:{installed:true,downloadUrl:'https://example.com/semantic.zip'}})");
assert.equal(element('downloadSemantic').disabled,false);
assert.match(element('semanticInstallStatus').textContent,/已安装/);
messages.length=0;
element('downloadSemantic').handlers.click();
element('openSemanticFolder').handlers.click();
assert.equal(messages[0].type,'downloadSemantic');
assert.equal(messages[1].type,'openSemanticFolder');
assert.equal(run('SETTING_DEFAULTS.nrSemanticFeather'),8);
console.log('PASS extras installed/missing state, blank download URL, host actions and feather default');

post('settings', {watchGames: true, scanned:true, lang:'zh', profiles:[
    {id:'default', name:'Default', settings:{}},
    {id:'remove', name:'Remove me', exe:'remove.exe', settings:{injectTiming:'late'}},
    {id:'keep', name:'Keep me', exe:'keep.exe', settings:{}}
]});
messages.length = 0;
run("activeId = 'remove'; renderAll(); pushWatchList()");
assert.equal(messages.find(m => m.type === 'watchList').lateNames, 'remove.exe*keep.exe');
messages.length = 0;
element('deleteProfile').handlers.click();
assert.equal(element('deleteProfileDialog').open, true);
assert.equal(run('draft.profiles.length'), 3);
assert.equal(messages.length, 0, 'opening confirmation cannot save or delete');
element('cancelDeleteProfile').handlers.click();
assert.equal(run('draft.profiles.length'), 3);
element('deleteProfile').handlers.click();
element('deleteProfileDialog').handlers.cancel();
element('deleteProfileDialog').close();
element('confirmDeleteProfile').handlers.click();
assert.equal(run('draft.profiles.length'), 3, 'Escape/cancel invalidates pending deletion');
element('deleteProfile').handlers.click();
run("activeId = 'keep'; renderAll()");
element('confirmDeleteProfile').handlers.click();
assert.equal(run("draft.profiles.some(p => p.id === 'remove')"), false);
assert.equal(run("draft.profiles.some(p => p.id === 'keep')"), true, 'confirmation deletes its named profile, not a later selection');
run('persistAll()');
assert.equal(messages.filter(m => m.type === 'watchList').at(-1).names, 'keep.exe');
run("activeId = 'default'; renderAll()");
assert.equal(element('deleteProfile').disabled, true);
element('deleteProfile').handlers.click();
assert.equal(element('deleteProfileDialog').open, false);
console.log('PASS confirmed profile removal: cancel/Escape, named target, default protection and watch-list persistence');

assert.equal(run("resolvedInjectionTiming({exe:'SanAndreas.EXE',settings:{}})"),'late');
assert.equal(run("resolvedInjectionTiming({exe:'sanandreas.exe',settings:{injectTiming:'early'}})"),'early');
assert.equal(run("resolvedInjectionTiming({exePath:'C:/Games/SanAndreas.exe',settings:{injectTiming:'auto'}})"),'late');
assert.equal(run("resolvedInjectionTiming({exe:'other.exe',settings:{}})"),'early');
post('settings',{watchGames:true,scanned:true,profiles:[{id:'gta',name:'GTA SA',exe:'SanAndreas.exe',exePath:'C:/Games/SanAndreas.exe',settings:{}}]});
messages.length=0;
run("activeId='gta';launchProfile(activeProfile())");
assert.equal(messages.find(m=>m.type==='launch').timing,'late');
assert.equal(messages.filter(m=>m.type==='watchList').at(-1).lateNames,'sanandreas.exe');
console.log('PASS GTA SA automatic compatibility uses the same late policy in launch and monitor; explicit early and other games preserved');

for (const exe of ['mafiadefinitiveedition.exe', 'unlisted-game.exe']) {
    assert.equal(run(`resolvedInjectionTiming({exe:'${exe}',settings:{}},'watch')`),'late');
    assert.equal(run(`resolvedInjectionTiming({exe:'${exe}',settings:{}},'launch')`),'early');
    assert.equal(run(`resolvedInjectionTiming({exe:'${exe}',settings:{injectTiming:'early'}},'watch')`),'early');
    assert.equal(run(`resolvedInjectionTiming({exe:'${exe}',settings:{injectTiming:'late'}},'launch')`),'late');
}
console.log('PASS source-aware Automatic timing for every game, explicit Early override and saved late compatibility value');

assert.equal(run('MODEL_DEFAULTS.hotkeys.toggleInject'),'Alt + F8');
for (const [hotkeys,version,expected] of [
    [{toggleInject:'Alt + 9'},0,'Alt + F8'],
    [{toggleInject:'Ctrl + F12'},0,'Ctrl + F12'],
    [{toggleInject:'Alt + 9'},1,'Alt + 9'],
    [{toggleInject:'Alt + 9',toggleDebugView:'Alt + F8'},0,'Alt + 9']
]) {
    post('settings',{lang:'zh',scanned:true,watchGames:false,injectHotkeyVersion:version,hotkeys});
    assert.equal(run('draft.hotkeys.toggleInject'),expected);
    assert.equal(run('draft.injectHotkeyVersion'),1);
}
post('hotkeysRegistered',{toggleInject:'Alt + F8'});
run("draft.hotkeys.toggleAll='F6';draft.hotkeys.toggleOverlay='F7';renderAll()");
let hint=run('computeAdvice({attached:false}).text');
assert.match(hint,/未启动游戏.*F6.*F7.*Alt \+ F8.*加载工具/);
post('hotkeysRegistered',{toggleInject:'Ctrl + F10'});
hint=run('computeAdvice({attached:false}).text');
assert.match(hint,/Ctrl \+ F10/);assert.doesNotMatch(hint,/Alt \+ F8/);
post('hotkeysRegistered',{toggleInject:''});
assert.match(run('computeAdvice({attached:false}).text'),/未注册成功/);
post('settings',{lang:'zh',watchGames:false,scanned:true,profiles:[
    {id:'default',name:'Default',settings:{}},
    {id:'yysls',name:'Where Winds Meet',exe:'yysls.exe',settings:{fgBufferCountThreshold:8}},
    {id:'another',name:'Another game',exe:'another.exe',settings:{}}
]});
messages.length=0;run('persistAll()');
assert.equal(messages.find(m=>m.type==='applyProfile'&&m.file==='yysls.exe.json').payload.fgBufferCountThreshold,8);
assert.equal(messages.find(m=>m.type==='applyProfile'&&m.file==='another.exe.json').payload.fgBufferCountThreshold,undefined);
for (const value of [-1,0,1,17,999,2.5,'bad'])
    assert.equal(run(`pruneDeadSettings({fgBufferCountThreshold:${JSON.stringify(value)}}).fgBufferCountThreshold`),undefined);
assert.equal(run('pruneDeadSettings({fgBufferCountThreshold:16}).fgBufferCountThreshold'),16);
// Upgrade a real legacy model with no per-game override. Only the exact game
// changes default; its generated old flat 4 is replaced by an omitted key.
messages.length=0;
post('settings',{lang:'zh',scanned:true,profiles:[
    {id:'yysls',name:'Renamed profile',exe:'yysls.exe',settings:{}},
    {id:'custom4',name:'Explicit four',exePath:'X:/custom/YYSLS.EXE',settings:{fgBufferCountThreshold:4}},
    {id:'custom12',name:'Custom twelve',exe:'yysls.exe',settings:{fgBufferCountThreshold:12}},
    {id:'lookalike',name:'燕云十六声',exe:'yysls_launcher.exe',settings:{}},
    {id:'other',name:'Other',exe:'other.exe',settings:{fgBufferCountThreshold:5}},
]});
assert.equal(run('resolvedSettings(draft.profiles[0]).fgBufferCountThreshold'),8);
assert.equal(run('resolvedSettings(draft.profiles[1]).fgBufferCountThreshold'),4);
assert.equal(run('resolvedSettings(draft.profiles[2]).fgBufferCountThreshold'),12);
assert.equal(run('resolvedSettings(draft.profiles[3]).fgBufferCountThreshold'),4);
assert.equal(run('resolvedSettings(draft.profiles[4]).fgBufferCountThreshold'),5);
const migrated=messages.filter(m=>m.type==='applyProfile');
assert.equal(migrated.length,1);assert.equal(migrated[0].file,'yysls.exe.json');
assert.equal(migrated[0].noReload,1);assert.equal('fgBufferCountThreshold' in migrated[0].payload,false);
assert.equal(run("Object.hasOwn(saved.profiles[0].settings,'fgBufferCountThreshold')"),false);
const migratedModel=messages.find(m=>m.type==='applySettings').payload;
messages.length=0;post('settings',migratedModel);
assert.equal(messages.some(m=>m.type==='applyProfile'||m.type==='applySettings'),false,'migration is idempotent');
for(const [value,expected] of [[4,4],[16,16],[2.5,8],['bad',8],[-1,8]]) {
    post('settings',{lang:'zh',scanned:true,profiles:[{id:'yy',exe:'yysls.exe',settings:{fgBufferCountThreshold:value}}]});
    assert.equal(run('resolvedSettings(activeProfile()).fgBufferCountThreshold'),expected);
}
// The actual executable path wins over stale metadata, and the flat filename
// follows the same identity as compatibility defaults.
assert.equal(run("defaultFgThreshold({exe:'yysls.exe',exePath:'X:/other.exe'})"),4);
assert.equal(run("profileFileName({id:'old',exe:'other.exe',exePath:'X:/YYSLS.EXE'})"),'yysls.exe.json');
assert.equal(run("needsReFrameworkSetup({id:'re',exePath:'X:/RE4.EXE'})"),true);
assert.equal(run("needsReFrameworkSetup({id:'re',exe:'onimushawots_demo.exe'})"),true);
for(const exe of ['re4.exe.backup','my_re4.exe','re8demo.exe','pragmata.exe','MonsterHunterWorld.exe'])
    assert.equal(run(`needsReFrameworkSetup({name:'Resident Evil 4',exe:${JSON.stringify(exe)}})`),false);
assert.equal(run("needsReFrameworkSetup({exe:'re4.exe',exePath:'X:/other.exe'})"),false);
messages.length=0;
element('reFrameworkDownload').handlers.click({preventDefault(){}});
element('reShadeDownload').handlers.click({preventDefault(){}});
assert.deepEqual(messages.map(m=>m.type),['openReFrameworkPage','openReShadePage']);
console.log('PASS manual-injection default migration/custom preservation/conflicts, dynamic idle shortcuts, per-game FG threshold persistence and invalid-value fallback');
