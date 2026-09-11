'use strict';
(() => {
    const dialog = document.getElementById('updateDialog');
    const title = document.getElementById('updateTitle');
    const description = document.getElementById('updateDescription');
    const notes = document.getElementById('updateNotes');
    const accept = document.getElementById('updateAccept');
    const later = document.getElementById('updateLater');
    let state = {state: 'none'};
    function render() {
        const en = window.I18N?.lang() === 'en';
        const text = (zh, english) => en ? english : zh;
        const ready = state.state === 'downloaded';
        const busy = ['downloading', 'installing'].includes(state.state);
        title.textContent = state.state === 'error' ? text('更新未完成', 'Update not completed')
            : text('DXL 更新', 'DXL update') + (state.version ? ' · ' + state.version : '');
        description.textContent = state.state === 'error'
            ? text('更新失败。请检查网络连接，并退出游戏和其他 DXL 窗口后重试。已下载的完整包会保留。', 'The update failed. Check your connection and close games and other DXL windows before retrying. Completed downloads are kept.')
            : state.state === 'downloading' ? text('正在后台下载并校验更新包…', 'Downloading and verifying the update in the background…')
            : state.state === 'installing' ? text('正在启动更新程序，DXL 将关闭并在安装后重启…', 'Starting the updater. DXL will close and restart after installation…')
            : ready ? (state.local ? text('检测到本地有更新的版本，是否安装？', 'A newer downloaded version was found. Install it now?')
                : text('下载完成，是否安装并重启 DXL？', 'Download complete. Install and restart DXL now?'))
            : text('发现新版本。以下为 GitHub 上的更新说明。', 'A new version is available. Release notes from GitHub are shown below.');
        // Release text is untrusted input. Never render it as HTML or execute links.
        notes.textContent = state.body || text('此版本未提供更新说明。', 'No release notes were provided.');
        notes.hidden = state.state === 'error';
        accept.hidden = state.state === 'error';
        accept.disabled = busy;
        accept.textContent = ready || state.state === 'installing' ? text('安装并重启', 'Install and restart')
            : state.state === 'downloading' ? text('下载中…', 'Downloading…') : text('下载更新', 'Download update');
        later.textContent = ready ? text('暂不安装', 'Not now') : text('关闭', 'Close');
    }
    window.DxlUpdate = {
        receive(value) {
            if (!value || value.state === 'none') return;
            state = value;
            render();
            if (!dialog.open) dialog.showModal();
        }
    };
    accept.addEventListener('click', () => {
        if (!['available', 'downloaded'].includes(state.state)) return;
        const install = state.state === 'downloaded';
        if (install && typeof persistAll === 'function') persistAll();
        state = {...state, state: install ? 'installing' : 'downloading'};
        render();
        host.post(install ? 'installUpdate' : 'downloadUpdate', null, {version: state.version});
    });
    later.addEventListener('click', () => dialog.close());
    window.addEventListener('dxl-language-changed', render);
    host.post('checkUpdates');
})();
