const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert/strict');
const elements = new Map();
const messages = [];
let language = 'zh';
const handlers = {};
const context = { window: { I18N: { lang: () => language }, addEventListener: (n,f) => handlers[n]=f },
    host: { post: (...args) => messages.push(args) },
    document: { getElementById(id) {
        if (!elements.has(id)) elements.set(id, {textContent:'', hidden:false, disabled:false, open:false, events:{},
            addEventListener(n,f) {this.events[n]=f;}, showModal() {this.open=true;}, close() {this.open=false;}});
        return elements.get(id);
    } } };
vm.runInNewContext(fs.readFileSync(path.join(__dirname,'../src/ui/web/updates.js'),'utf8'),context);
const receive = context.window.DxlUpdate.receive;
const el = id => elements.get(id);
assert.deepEqual(messages,[['checkUpdates']]);
receive({state:'none'}); assert.equal(el('updateDialog').open,false);
const hostile='<img src=x onerror=alert(1)> & [link](javascript:alert(1))';
receive({state:'available',version:'0.3',body:hostile});
assert.equal(el('updateNotes').textContent,hostile);
assert.equal(el('updateNotes').innerHTML,undefined);
el('updateAccept').events.click(); assert.equal(messages.at(-1)[0],'downloadUpdate');
assert.equal(el('updateAccept').disabled,true);
el('updateAccept').events.click(); assert.equal(messages.length,2);
receive({state:'downloaded',version:'0.3',body:'notes',local:false});
assert.match(el('updateDescription').textContent,/下载完成/);
el('updateLater').events.click(); assert.equal(el('updateDialog').open,false);
assert.equal(messages.length,2);
receive({state:'downloaded',version:'0.3',body:'notes',local:true});
assert.match(el('updateDescription').textContent,/本地/);
language='en';handlers['dxl-language-changed']();
for (const id of ['updateTitle','updateDescription','updateAccept','updateLater']) assert.doesNotMatch(el(id).textContent,/[\u4e00-\u9fff]/);
el('updateAccept').events.click(); assert.equal(messages.at(-1)[0],'installUpdate');
receive({state:'error'});assert.equal(el('updateAccept').hidden,true);
console.log('PASS updater UI: silent no-update, safe release notes, download/install consent, cached prompt, bilingual states');
