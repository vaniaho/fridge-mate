const wsProto = location.protocol === 'https:' ? 'wss:' : 'ws:';
let ws = null;

const state = {
    inventory: [],
    history: []
};

// DOM Elements
const views = document.querySelectorAll('.view');
const navBtns = document.querySelectorAll('.nav-btn');
const dot = document.querySelector('.dot');
const connStatus = document.getElementById('conn-status');
const sysTime = document.getElementById('sys-time');
const sysSdcard = document.getElementById('sys-sdcard');
const sysHeap = document.getElementById('sys-heap');
const sysWifi = document.getElementById('sys-wifi');

// Inventory
const categoryContainer = document.getElementById('category-container');
const countSpan = document.getElementById('item-count');
const addModal = document.getElementById('add-modal');
const addForm = document.getElementById('add-form');

// Edit Modal
const editModal = document.getElementById('edit-modal');
const editForm = document.getElementById('edit-form');
const editCancelBtn = document.getElementById('edit-cancel-btn');

// Search & Sort
const searchInput = document.getElementById('search-input');
const sortSelect = document.getElementById('sort-select');

// Chat
const chatMessages = document.getElementById('chat-messages');
const chatForm = document.getElementById('chat-form');
const chatInput = document.getElementById('chat-input');
const chatSend = document.getElementById('chat-send');
const voiceLiveStatus = document.getElementById('voice-live-status');
const voiceLiveDot = document.getElementById('voice-live-dot');
const voiceModeSelect = document.getElementById('voice-mode-select');
const browserPlaybackToggle = document.getElementById('browser-playback');
let microphoneStream = null;
let microphoneContext = null;
let microphoneProcessor = null;
let microphoneSource = null;
let microphonePending = [];
let playbackContext = null;
let playbackCursor = 0;
let voiceActive = false;

// Settings
const settingsForm = document.getElementById('settings-form');
const timeStatus = document.getElementById('time-status');
const timeCurrent = document.getElementById('time-current');
const timeInput = document.getElementById('time-input');
const timeSyncBtn = document.getElementById('time-sync-btn');
const timeSetForm = document.getElementById('time-set-form');
const voiceRuntimeForm = document.getElementById('voice-runtime-form');
const voiceWakeEnabled = document.getElementById('voice-wake-enabled');
const voiceBargeEnabled = document.getElementById('voice-barge-enabled');
const voiceSensitivity = document.getElementById('voice-sensitivity');
const voiceSensitivityValue = document.getElementById('voice-sensitivity-value');
const voiceContinuous = document.getElementById('voice-continuous');
const voiceContinuousValue = document.getElementById('voice-continuous-value');

// Recipes
const recipeContainer = document.getElementById('recipe-container');
const recipeModal = document.getElementById('recipe-modal');
const recipeForm = document.getElementById('recipe-form');
const recipeIngList = document.getElementById('recipe-ingredients-list');

// Dashboard (天气 + 留言板)
const weatherRefreshBtn = document.getElementById('weather-refresh-btn');
const weatherForm = document.getElementById('weather-form');
const notesList = document.getElementById('notes-list');
const notesCount = document.getElementById('notes-count');
const notesForm = document.getElementById('notes-form');
const notesInput = document.getElementById('notes-input');

const iconMap = { '水果': '🍎', '蔬菜': '🥬', '肉类': '🥩', '饮品': '🥛', '其他': '📦', '家常菜': '🍲', '汤类': '🥣', '主食': '🍚', '素菜': '🥗' };

async function init() {
    setupRouter();
    setupEventListeners();
    setupVoiceListeners();
    // The embedded HTTPS server intentionally keeps a small TLS connection
    // pool. Avoid opening two API requests and the long-lived WebSocket at
    // exactly the same time during first paint.
    await fetchInventory();
    await fetchStatus();
    setupWebSocket();
}

// === Navigation ===
function setupRouter() {
    navBtns.forEach(btn => {
        btn.addEventListener('click', () => {
            navBtns.forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            const target = btn.getAttribute('data-target');
            views.forEach(v => v.classList.remove('active'));
            document.getElementById(target).classList.add('active');
            
            // Lazy load specific view data
            if (target === 'view-history') fetchHistory();
            if (target === 'view-settings') fetchSettings();
            if (target === 'view-recipes') fetchRecipes();
            if (target === 'view-voice') fetchVoiceConfig();
            if (target === 'view-dashboard') { fetchWeather(); fetchNotes(); }
        });
    });
}

// === System Status ===
async function fetchStatus() {
    try {
        const res = await fetch('/api/status');
        const data = await res.json();
        sysHeap.textContent = Math.round(data.free_heap / 1024) + ' KB';
        sysWifi.textContent = data.wifi_rssi + ' dBm';
        if (data.rtc_synced) {
            sysTime.textContent = data.sys_time.split(' ')[1] || data.sys_time;
        } else {
            sysTime.textContent = '未同步';
            sysTime.style.color = '#ff6b6b';
        }
        sysSdcard.textContent = data.sd_available ? '正常' : '未挂载';
        sysSdcard.style.color = data.sd_available ? 'inherit' : '#ff6b6b';
    } catch (e) {
        console.warn('Status fetch failed');
    }
}

// === Inventory ===
async function fetchInventory() {
    try {
        const res = await fetch('/api/inventory');
        state.inventory = await res.json();
        renderInventory();
    } catch (err) { console.error(err); }
}

function renderInventory() {
    let filteredList = state.inventory;
    
    // 1. Search Filter
    const searchTerm = searchInput.value.trim().toLowerCase();
    if (searchTerm) {
        filteredList = filteredList.filter(item => item.name.toLowerCase().includes(searchTerm));
    }

    // 2. Sort
    const sortVal = sortSelect.value;
    filteredList.sort((a, b) => {
        // Calculate approx expire timestamp if backend doesn't provide it
        const aExp = a.entry_time + (a.expire_days * 24 * 3600);
        const bExp = b.entry_time + (b.expire_days * 24 * 3600);
        
        if (sortVal === 'expire_asc') return aExp - bExp;
        if (sortVal === 'expire_desc') return bExp - aExp;
        if (sortVal === 'entry_desc') return b.entry_time - a.entry_time;
        if (sortVal === 'entry_asc') return a.entry_time - b.entry_time;
        return 0;
    });

    countSpan.textContent = `${filteredList.length} 项`;
    categoryContainer.innerHTML = '';
    
    // Group by category
    const grouped = {};
    filteredList.forEach(item => {
        if (!grouped[item.category]) grouped[item.category] = [];
        grouped[item.category].push(item);
    });

    for (const cat in grouped) {
        const groupDiv = document.createElement('div');
        groupDiv.className = 'category-group';
        groupDiv.innerHTML = `<div class="category-title">${cat} (${grouped[cat].length})</div>`;
        
        const grid = document.createElement('div');
        grid.className = 'inventory-grid';
        
        grouped[cat].forEach(item => {
            const card = document.createElement('div');
            card.className = 'item-card';
            let expireClass = '';
            if (item.expire_days <= 1) expireClass = 'expire-danger';
            else if (item.expire_days <= 3) expireClass = 'expire-warning';
            
            card.innerHTML = `
                <div class="card-header">
                    <div class="item-icon">${iconMap[item.category] || '📦'}</div>
                    <div class="card-actions">
                        <button class="icon-btn edit" onclick="openEditModal('${item.name}')" title="编辑">✏️</button>
                        <button class="icon-btn remove" onclick="removeAllIngredient('${item.name}', ${item.quantity})" title="全部取出">🗑️</button>
                    </div>
                </div>
                <div class="item-name">${item.name}</div>
                <div class="item-expire ${expireClass}">保质期 ${item.expire_days} 天</div>
                <div class="item-controls">
                    <button class="qty-btn" onclick="decreaseIngredient('${item.name}', ${item.quantity})">-</button>
                    <span class="item-quantity">${item.quantity}</span>
                    <button class="qty-btn" onclick="increaseIngredient('${item.name}')">+</button>
                </div>
            `;
            grid.appendChild(card);
        });
        groupDiv.appendChild(grid);
        categoryContainer.appendChild(groupDiv);
    }
}

async function requestInventoryWrite(data, confirmationMessage) {
    if (confirmationMessage && !confirm(confirmationMessage)) {
        return false;
    }

    const payload = { ...data, confirmed: true };
    try {
        const res = await fetch('/api/inventory', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        let response = {};
        try {
            response = await res.json();
        } catch (_) {
            response = {};
        }

        if (!res.ok || response.status !== 'ok') {
            alert(response.error || '库存操作失败，请稍后重试');
            return false;
        }
        return true;
    } catch (_) {
        alert('网络错误，库存操作未执行');
        return false;
    }
}

async function removeIngredient(name) {
    const ok = await requestInventoryWrite(
        { action: 'remove', name, quantity: 1 },
        `确认取出 1 个 ${name} 吗？`
    );
    if (ok) fetchInventory();
}

async function decreaseIngredient(name, currentQty) {
    const message = currentQty <= 1
        ? `确认取出最后 1 个 ${name} 吗？这会将其从列表中移除。`
        : `确认取出 1 个 ${name} 吗？`;
    const ok = await requestInventoryWrite(
        { action: 'remove', name, quantity: 1 },
        message
    );
    if (ok) fetchInventory();
}

async function increaseIngredient(name) {
    // We can just call add with quantity 1, but we need to pass category and expire_days
    // A better way is to find the item in state and use its existing metadata
    const item = state.inventory.find(i => i.name === name);
    if (!item) {
        alert('未找到该食材，请刷新库存后重试');
        return;
    }
    const ok = await requestInventoryWrite(
        {
            action: 'add',
            name,
            category: item.category,
            quantity: 1,
            expire_days: item.expire_days
        },
        `确认增加 1 个 ${name} 吗？`
    );
    if (ok) fetchInventory();
}

async function removeAllIngredient(name, currentQty) {
    const ok = await requestInventoryWrite(
        { action: 'clear', name },
        `确认清空全部 ${currentQty} 个 ${name} 吗？此操作不可撤销。`
    );
    if (ok) fetchInventory();
}

// Unix to YYYY-MM-DDTHH:mm string for datetime-local
function unixToDatetimeLocal(unixSeconds) {
    const d = new Date(unixSeconds * 1000);
    // Pad to standard local ISO string
    const pad = (n) => n.toString().padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

// string to Unix seconds
function datetimeLocalToUnix(str) {
    return Math.floor(new Date(str).getTime() / 1000);
}

function openEditModal(name) {
    const item = state.inventory.find(i => i.name === name);
    if (!item) return;
    
    document.getElementById('edit-name').value = item.name;
    document.getElementById('edit-name-display').value = item.name;
    document.getElementById('edit-category').value = item.category;
    document.getElementById('edit-quantity').value = item.quantity;
    document.getElementById('edit-expire').value = item.expire_days;
    document.getElementById('edit-entry-time').value = unixToDatetimeLocal(item.entry_time);
    
    editModal.classList.add('active');
}

// === History ===
async function fetchHistory() {
    try {
        const res = await fetch('/api/history');
        const data = await res.json();
        const list = document.getElementById('history-list');
        list.innerHTML = '';
        if(data.length === 0) {
            list.innerHTML = '<div class="text-muted" style="text-align:center;padding:20px;">暂无记录</div>';
            return;
        }
        data.forEach(rec => {
            const date = new Date(rec.timestamp * 1000).toLocaleString();
            const isAdd = rec.action === 'ADD';
            list.innerHTML += `
                <div class="history-item">
                    <div class="hist-info">
                        <span class="hist-action ${isAdd ? 'add' : 'remove'}">
                            ${isAdd ? '放入' : '取出'} ${rec.item_name} x ${rec.quantity}
                        </span>
                        <span class="hist-date">${date}</span>
                    </div>
                </div>
            `;
        });
    } catch (e) { console.error(e); }
}

// === Chat ===
function addChatMessage(text, isUser = false) {
    const div = document.createElement('div');
    div.className = `msg ${isUser ? 'user-msg' : 'bot-msg'}`;
    div.textContent = text;
    chatMessages.appendChild(div);
    chatMessages.scrollTop = chatMessages.scrollHeight;
    return div;
}

let streamingAssistantMessage = null;

function appendAssistantStream(text) {
    if (!text) return;
    if (!streamingAssistantMessage) {
        streamingAssistantMessage = addChatMessage('');
    }
    streamingAssistantMessage.textContent += text;
    chatMessages.scrollTop = chatMessages.scrollHeight;
}

function finishAssistantStream(text) {
    if (streamingAssistantMessage) {
        if (text) streamingAssistantMessage.textContent = text;
        streamingAssistantMessage = null;
    } else if (text) {
        addChatMessage(text);
    }
}

function sendWsJson(payload) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(payload));
        return true;
    }
    return false;
}

function resampleTo16k(input, sourceRate) {
    if (sourceRate === 16000) return input;
    const ratio = sourceRate / 16000;
    const output = new Float32Array(Math.floor(input.length / ratio));
    for (let i = 0; i < output.length; i++) {
        const pos = i * ratio;
        const left = Math.floor(pos);
        const right = Math.min(left + 1, input.length - 1);
        const fraction = pos - left;
        output[i] = input[left] * (1 - fraction) + input[right] * fraction;
    }
    return output;
}

function sendMicrophoneSamples(floatSamples) {
    const samples = resampleTo16k(floatSamples, microphoneContext.sampleRate);
    for (const sample of samples) {
        microphonePending.push(Math.max(-1, Math.min(1, sample)));
    }
    while (microphonePending.length >= 320) {
        const pcm = new Int16Array(320);
        for (let i = 0; i < pcm.length; i++) {
            const sample = microphonePending.shift();
            pcm[i] = sample < 0 ? sample * 32768 : sample * 32767;
        }
        if (ws && ws.readyState === WebSocket.OPEN) ws.send(pcm.buffer);
    }
}

async function startBrowserVoice() {
    if (voiceActive) {
        sendWsJson({ type: 'voice.interrupt' });
        return;
    }
    if (!window.isSecureContext || !navigator.mediaDevices) {
        addChatMessage(
            `浏览器麦克风需要安全连接。请改用 https://${location.hostname}/ 访问并接受设备证书；证书也可从 http://${location.hostname}/device-cert.pem 下载。`
        );
        return;
    }
    try {
        microphoneStream = await navigator.mediaDevices.getUserMedia({
            audio: { channelCount: 1, echoCancellation: true,
                     noiseSuppression: true, autoGainControl: true }
        });
        microphoneContext = new AudioContext();
        microphoneSource = microphoneContext.createMediaStreamSource(microphoneStream);
        microphoneProcessor = microphoneContext.createScriptProcessor(1024, 1, 1);
        microphoneProcessor.onaudioprocess = event => {
            sendMicrophoneSamples(event.inputBuffer.getChannelData(0));
        };
        microphoneSource.connect(microphoneProcessor);
        microphoneProcessor.connect(microphoneContext.destination);
        microphonePending = [];
        voiceActive = true;
        sendWsJson({ type: 'voice.start', mode: voiceModeSelect.value });
    } catch (error) {
        addChatMessage(`无法使用浏览器麦克风：${error.message || error}`);
    }
}

function stopBrowserCapture(closeSession = false) {
    if (microphoneProcessor) microphoneProcessor.disconnect();
    if (microphoneSource) microphoneSource.disconnect();
    if (microphoneStream) microphoneStream.getTracks().forEach(track => track.stop());
    if (microphoneContext) microphoneContext.close();
    microphoneProcessor = null;
    microphoneSource = null;
    microphoneStream = null;
    microphoneContext = null;
    microphonePending = [];
    if (voiceActive) sendWsJson({ type: closeSession ? 'voice.close' : 'voice.stop' });
    voiceActive = false;
}

function playPcm16(arrayBuffer) {
    if (!browserPlaybackToggle.checked) return;
    if (!playbackContext) playbackContext = new AudioContext({ sampleRate: 16000 });
    const source = playbackContext.createBufferSource();
    const samples = new Int16Array(arrayBuffer);
    const buffer = playbackContext.createBuffer(1, samples.length, 16000);
    const channel = buffer.getChannelData(0);
    for (let i = 0; i < samples.length; i++) channel[i] = samples[i] / 32768;
    source.buffer = buffer;
    source.connect(playbackContext.destination);
    playbackCursor = Math.max(playbackCursor, playbackContext.currentTime + 0.02);
    source.start(playbackCursor);
    playbackCursor += buffer.duration;
}

function handleVoiceEvent(message) {
    const state = message.state || 'idle';
    if (message.text) voiceLiveStatus.textContent = message.text;
    else voiceLiveStatus.textContent = state;
    voiceLiveDot.className = 'voice-live-dot';
    if (state === 'listening' || state === 'thinking') voiceLiveDot.classList.add('active');
    if (state === 'speaking') voiceLiveDot.classList.add('speaking');

    if (message.type === 'voice.asr.final') {
        finishAssistantStream();
        addChatMessage(message.text, true);
    }
    if (message.type === 'voice.assistant.delta') appendAssistantStream(message.text);
    if (message.type === 'voice.error') addChatMessage(`语音错误：${message.text}`);
    if (message.type === 'voice.assistant.final') {
        finishAssistantStream(message.text);
        fetchInventory();
    }
}

// === Dashboard: Weather & Notes ===
async function fetchWeather() {
    try {
        const res = await fetch('/api/weather');
        const data = await res.json();
        renderWeather(data);
    } catch (e) { console.error('Weather fetch failed', e); }
}

function renderWeather(data) {
    const tempEl = document.getElementById('weather-temp');
    const textEl = document.getElementById('weather-text');
    const cityEl = document.getElementById('weather-city');
    const updEl = document.getElementById('weather-updated');

    if (data.valid) {
        tempEl.textContent = Math.round(data.temp) + '°';
        textEl.textContent = data.text || '--';
        cityEl.textContent = data.city || '--';
        if (data.updated) {
            const d = new Date(data.updated * 1000);
            updEl.textContent = '更新于 ' + d.toLocaleTimeString();
        }
    } else {
        tempEl.textContent = '--°';
        textEl.textContent = '未获取';
        cityEl.textContent = '--';
        updEl.textContent = '未刷新';
    }

    // 填充配置表单（Key 脱敏，显示占位）
    const cfg = data.config || {};
    document.getElementById('wx-url').value = cfg.wx_url || '';
    document.getElementById('wx-key').value = '';
    document.getElementById('wx-key').placeholder = (cfg.wx_key && cfg.wx_key === '********') ? '已配置（留空不修改）' : '未配置';
    document.getElementById('wx-city').value = cfg.wx_city || '';
    document.getElementById('wx-location').value = cfg.wx_location || '';
    document.getElementById('wx-temp-path').value = cfg.wx_temp_path || '';
    document.getElementById('wx-text-path').value = cfg.wx_text_path || '';
}

async function fetchNotes() {
    try {
        const res = await fetch('/api/notes');
        const data = await res.json();
        renderNotes(data);
    } catch (e) { console.error('Notes fetch failed', e); }
}

function renderNotes(notes) {
    notesCount.textContent = `${notes.length} 条`;
    notesList.innerHTML = '';
    if (notes.length === 0) {
        notesList.innerHTML = '<div class="text-muted" style="text-align:center;padding:20px;">暂无留言</div>';
        return;
    }
    notes.forEach(note => {
        const item = document.createElement('div');
        item.className = 'note-item';
        const date = new Date(note.timestamp * 1000).toLocaleString();
        item.innerHTML = `
            <div class="note-info">
                <div class="note-text">${escapeHtml(note.text)}</div>
                <div class="note-date text-muted">${date}</div>
            </div>
            <button class="icon-btn remove" onclick="deleteNote(${note.timestamp})" title="删除">🗑️</button>
        `;
        notesList.appendChild(item);
    });
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

async function deleteNote(timestamp) {
    if (!confirm('确定删除这条留言吗？')) return;
    await fetch('/api/notes', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'delete', timestamp: timestamp })
    });
    fetchNotes();
}

// === Recipes ===
async function fetchRecipes() {
    try {
        const res = await fetch('/api/recipes/match');
        const data = await res.json();
        renderRecipes(data);
    } catch (e) { console.error(e); }
}

function renderRecipes(matches) {
    recipeContainer.innerHTML = '';
    if (matches.length === 0) {
        recipeContainer.innerHTML = '<div class="text-muted" style="text-align:center;padding:20px;">暂无食谱数据</div>';
        return;
    }
    
    matches.forEach(match => {
        const r = match.recipe;
        const isReady = match.missing_count === 0;
        
        let ingHtml = r.ingredients.map(ing => {
            const isMissing = match.missing_items.includes(ing.name);
            return `<span class="recipe-ing ${isMissing ? 'missing' : 'have'}">${ing.name}x${ing.min_quantity}</span>`;
        }).join('');

        const card = document.createElement('div');
        card.className = `item-card recipe-card ${isReady ? 'ready' : 'not-ready'}`;
        card.innerHTML = `
            <div class="card-header">
                <div class="item-icon">${iconMap[r.category] || '🍽️'}</div>
                <div class="card-actions">
                    <button class="icon-btn edit" onclick="openRecipeEdit('${r.name}')" title="编辑">✏️</button>
                    <button class="icon-btn remove" onclick="deleteRecipe('${r.name}')" title="删除">🗑️</button>
                </div>
            </div>
            <div class="item-name">${r.name} <span style="font-size:0.8rem;color:#888;">${r.category}</span></div>
            <div class="recipe-progress">
                <div class="progress-bar"><div class="progress-fill" style="width: ${match.coverage * 100}%;"></div></div>
                <div class="progress-text">${isReady ? '食材齐全！' : `还差 ${match.missing_count} 样`}</div>
            </div>
            <div class="recipe-ings">${ingHtml}</div>
            <div class="recipe-brief">${r.brief}</div>
        `;
        recipeContainer.appendChild(card);
    });
}

async function deleteRecipe(name) {
    if(!confirm(`确定要删除食谱 ${name} 吗？`)) return;
    await fetch('/api/recipes', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ action: 'remove', name: name })
    });
    fetchRecipes();
}

function openRecipeEdit(name) {
    // 简单的方案：从当前的 DOM 列表中抓取，或者再请求一次全量。
    // 这里我们直接通过后端 /api/recipes/match (或保存一份全局 state) 获取
    // 为了简单，我们只实现打开空弹窗让用户新增，因为完全复用 DOM 也不难，这里简化。
    alert("请先实现在前端维护完整的 recipes 列表来支持编辑填充。此处为演示。");
}

chatForm.addEventListener('submit', async (e) => {
    e.preventDefault();
    const text = chatInput.value.trim();
    if (!text) return;
    addChatMessage(text, true);
    chatInput.value = '';
    chatSend.disabled = true;
    chatSend.textContent = '...';

    try {
        const res = await fetch('/api/chat', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ text })
        });
        if (res.ok) {
            await res.json();
        } else {
            addChatMessage("抱歉，大模型处理失败了。");
        }
    } catch (err) {
        addChatMessage("网络错误，请求失败。");
    }
    chatSend.disabled = false;
    chatSend.textContent = '发送';
});

// === Settings ===
async function fetchSettings() {
    try {
        const res = await fetch('/api/settings');
        const data = await res.json();
        document.getElementById('set-ssid').value = data.wifi_ssid || '';
        document.getElementById('set-pass').value = data.wifi_pass || '';
        fetchTimeSettings();
        fetchVoiceRuntimeSettings();
    } catch(e) { console.error(e); }
}

function fillTimePanel(data) {
    if (!data) return;
    if (timeStatus) {
        timeStatus.textContent = data.synced ? '已同步' : '未同步';
        timeStatus.style.color = data.synced ? 'inherit' : '#ff6b6b';
    }
    if (timeCurrent) {
        timeCurrent.textContent = data.sys_time || '--';
    }
    if (sysTime && data.sys_time) {
        sysTime.textContent = data.sys_time.split(' ')[1] || data.sys_time;
        sysTime.style.color = data.synced ? 'inherit' : '#ff6b6b';
    }
}

async function fetchTimeSettings() {
    try {
        const res = await fetch('/api/time');
        const data = await res.json();
        fillTimePanel(data);
    } catch(e) { console.error(e); }
}

function datetimeLocalToPayload(value) {
    if (!value) return '';
    return value.length === 16 ? value + ':00' : value;
}

function fillVoiceRuntimeSettings(data) {
    if (!data) return;
    const sensitivity = Number.isFinite(data.wake_sensitivity)
        ? data.wake_sensitivity : 70;
    const seconds = Math.round((data.continuous_ms || 0) / 1000);
    if (voiceWakeEnabled) voiceWakeEnabled.checked = !!data.wake_enabled;
    if (voiceBargeEnabled) {
        voiceBargeEnabled.checked = !!data.tts_barge_in_enabled;
    }
    if (voiceSensitivity) voiceSensitivity.value = sensitivity;
    if (voiceSensitivityValue) {
        voiceSensitivityValue.textContent = `${sensitivity}%`;
    }
    if (voiceContinuous) voiceContinuous.value = seconds;
    if (voiceContinuousValue) {
        voiceContinuousValue.textContent = `${seconds} 秒`;
    }
}

async function fetchVoiceRuntimeSettings() {
    try {
        const res = await fetch('/api/voice/settings');
        const data = await res.json();
        fillVoiceRuntimeSettings(data);
    } catch(e) { console.error(e); }
}

settingsForm.addEventListener('submit', async (e) => {
    e.preventDefault();
    const data = {
        wifi_ssid: document.getElementById('set-ssid').value,
        wifi_pass: document.getElementById('set-pass').value
    };
    try {
        const res = await fetch('/api/settings', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(data)
        });
        if (res.ok) {
            alert('网络设置已保存，重新连接可能需要重启设备。');
        } else {
            alert('保存失败！');
        }
    } catch(e) { alert('网络错误'); }
});

if (voiceSensitivity && voiceSensitivityValue) {
    voiceSensitivity.addEventListener('input', () => {
        voiceSensitivityValue.textContent = `${voiceSensitivity.value}%`;
    });
}

if (voiceContinuous && voiceContinuousValue) {
    voiceContinuous.addEventListener('input', () => {
        voiceContinuousValue.textContent = `${voiceContinuous.value} 秒`;
    });
}

if (voiceRuntimeForm) {
    voiceRuntimeForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const payload = {
            wake_enabled: !!voiceWakeEnabled?.checked,
            wake_sensitivity: parseInt(voiceSensitivity?.value || '70', 10),
            tts_barge_in_enabled: !!voiceBargeEnabled?.checked,
            continuous_ms: parseInt(voiceContinuous?.value || '10', 10) * 1000
        };
        try {
            const res = await fetch('/api/voice/settings', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify(payload)
            });
            const data = await res.json().catch(() => ({}));
            if (res.ok && data.status === 'ok') {
                fillVoiceRuntimeSettings(data);
                alert('语音设置已保存');
            } else {
                alert('语音设置保存失败');
            }
        } catch(e) {
            alert('网络错误');
        }
    });
}

if (timeSyncBtn) {
    timeSyncBtn.addEventListener('click', async () => {
        timeSyncBtn.disabled = true;
        const oldText = timeSyncBtn.textContent;
        timeSyncBtn.textContent = '同步中...';
        try {
            const res = await fetch('/api/time/sync', { method: 'POST' });
            const data = await res.json();
            fillTimePanel(data);
            if (data.status !== 'ok') {
                alert('时间同步失败：' + (data.error || '未知错误'));
            }
        } catch(e) {
            alert('时间同步请求失败');
        }
        timeSyncBtn.disabled = false;
        timeSyncBtn.textContent = oldText;
    });
}

if (timeSetForm) {
    timeSetForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const datetime = datetimeLocalToPayload(timeInput ? timeInput.value : '');
        if (!datetime) {
            alert('请选择日期和时间');
            return;
        }
        try {
            const res = await fetch('/api/time', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({ datetime })
            });
            const data = await res.json();
            fillTimePanel(data);
            if (res.ok && data.status === 'ok') {
                alert('系统时间已更新');
            } else {
                alert('设置时间失败：' + (data.error || '无效时间'));
            }
        } catch(e) {
            alert('设置时间请求失败');
        }
    });
}

// === Global Listeners & WS ===
function setupEventListeners() {
    document.getElementById('add-btn').addEventListener('click', () => {
        addModal.classList.add('active');
    });
    document.getElementById('cancel-btn').addEventListener('click', () => {
        addModal.classList.remove('active');
        addForm.reset();
    });

    document.getElementById('history-fab-btn').addEventListener('click', () => {
        // Find the history nav button if we still want to keep nav active states, 
        // or just directly switch view. We removed it from nav, so just switch view.
        navBtns.forEach(b => b.classList.remove('active'));
        views.forEach(v => v.classList.remove('active'));
        document.getElementById('view-history').classList.add('active');
        fetchHistory();
    });
    
    editCancelBtn.addEventListener('click', () => {
        editModal.classList.remove('active');
        editForm.reset();
    });

    document.getElementById('add-recipe-btn').addEventListener('click', () => {
        document.getElementById('recipe-modal-title').textContent = "新增食谱";
        document.getElementById('recipe-old-name').value = '';
        recipeForm.reset();
        recipeIngList.innerHTML = '';
        addRecipeIngRow(); // add at least one row
        recipeModal.classList.add('active');
    });

    document.getElementById('recipe-cancel-btn').addEventListener('click', () => {
        recipeModal.classList.remove('active');
    });

    document.getElementById('add-recipe-ing-btn').addEventListener('click', () => {
        addRecipeIngRow();
    });

    // Search and Sort Listeners
    searchInput.addEventListener('input', renderInventory);
    sortSelect.addEventListener('change', renderInventory);
    addForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const data = {
            action: 'add',
            name: document.getElementById('name').value,
            category: document.getElementById('category').value,
            quantity: parseInt(document.getElementById('quantity').value),
            expire_days: parseInt(document.getElementById('expire').value)
        };
        const ok = await requestInventoryWrite(
            data,
            `确认存入 ${data.quantity} 个 ${data.name} 吗？\n分类：${data.category}\n保质期：${data.expire_days} 天`
        );
        if (ok) {
            addModal.classList.remove('active');
            addForm.reset();
            fetchInventory();
            if (document.getElementById('view-history').classList.contains('active')) fetchHistory();
        }
    });

    editForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const data = {
            action: 'update',
            name: document.getElementById('edit-name').value,
            category: document.getElementById('edit-category').value,
            quantity: parseInt(document.getElementById('edit-quantity').value),
            expire_days: parseInt(document.getElementById('edit-expire').value),
            entry_time: datetimeLocalToUnix(document.getElementById('edit-entry-time').value)
        };
        const ok = await requestInventoryWrite(
            data,
            `确认将 ${data.name} 修改为 ${data.quantity} 个吗？\n分类：${data.category}\n保质期：${data.expire_days} 天`
        );
        if (ok) {
            editModal.classList.remove('active');
            editForm.reset();
            fetchInventory();
            if (document.getElementById('view-history').classList.contains('active')) fetchHistory();
        }
    });

    recipeForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const oldName = document.getElementById('recipe-old-name').value;
        const data = {
            action: oldName ? 'update' : 'add',
            old_name: oldName,
            name: document.getElementById('recipe-name').value,
            category: document.getElementById('recipe-category').value,
            brief: document.getElementById('recipe-brief').value,
            ingredients: []
        };
        const rows = recipeIngList.querySelectorAll('.recipe-ing-row');
        rows.forEach(row => {
            const n = row.querySelector('.ing-name').value;
            const q = row.querySelector('.ing-qty').value;
            if(n) {
                data.ingredients.push({ name: n, min_quantity: parseInt(q) });
            }
        });

        const res = await fetch('/api/recipes', {
            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data)
        });
        if (res.ok) {
            recipeModal.classList.remove('active');
            fetchRecipes();
        } else {
            alert('保存失败');
        }
    });

    // Dashboard listeners
    weatherRefreshBtn.addEventListener('click', async () => {
        weatherRefreshBtn.disabled = true;
        weatherRefreshBtn.textContent = '刷新中...';
        try {
            await fetch('/api/weather/refresh', { method: 'POST' });
            fetchWeather();
        } catch (e) {}
        weatherRefreshBtn.disabled = false;
        weatherRefreshBtn.textContent = '刷新';
    });

    weatherForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const data = {
            wx_url: document.getElementById('wx-url').value,
            wx_key: document.getElementById('wx-key').value,
            wx_city: document.getElementById('wx-city').value,
            wx_location: document.getElementById('wx-location').value,
            wx_temp_path: document.getElementById('wx-temp-path').value,
            wx_text_path: document.getElementById('wx-text-path').value,
        };
        try {
            const res = await fetch('/api/weather', {
                method: 'POST', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(data)
            });
            const r = await res.json();
            fetchWeather();
            alert(r.refreshed ? '配置已保存，天气已刷新。' : '配置已保存，但刷新失败（请检查配置或网络）。');
        } catch (e) { alert('保存失败'); }
    });

    notesForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const text = notesInput.value.trim();
        if (!text) return;
        await fetch('/api/notes', {
            method: 'POST', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'add', text: text })
        });
        notesInput.value = '';
        fetchNotes();
    });

    setInterval(fetchStatus, 5000); // Poll status every 5s
}

function addRecipeIngRow(name = '', qty = 1) {
    const div = document.createElement('div');
    div.className = 'recipe-ing-row';
    div.style.display = 'flex';
    div.style.gap = '8px';
    div.style.marginBottom = '8px';
    div.innerHTML = `
        <input type="text" class="ing-name" placeholder="食材名" required value="${name}" style="flex:2;">
        <input type="number" class="ing-qty" placeholder="数量" min="1" required value="${qty}" style="flex:1;">
        <button type="button" class="btn btn-secondary btn-small" onclick="this.parentElement.remove()">X</button>
    `;
    recipeIngList.appendChild(div);
}

function setupWebSocket() {
    if (location.protocol === 'file:') return;
    ws = new WebSocket(`${wsProto}//${location.host}/ws`);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => { dot.classList.add('connected'); connStatus.textContent = "已连接"; };
    ws.onclose = () => { dot.classList.remove('connected'); connStatus.textContent = "离线重连中..."; setTimeout(setupWebSocket, 3000); };
    ws.onmessage = (e) => {
        if (e.data instanceof ArrayBuffer) {
            playPcm16(e.data);
            return;
        }
        let message = null;
        try { message = JSON.parse(e.data); } catch (_) {}
        if (message && message.type && message.type.startsWith('voice.')) {
            handleVoiceEvent(message);
            return;
        }
        if (e.data === 'update') {
            fetchInventory();
            if (document.getElementById('view-history').classList.contains('active')) fetchHistory();
            if (document.getElementById('view-dashboard').classList.contains('active')) fetchNotes();
        }
    };
}

// === Voice Config ===
let voiceConfigData = {
    asr: { current: {}, history: [] },
    llm: { current: {}, history: [] },
    tts: { current: {}, history: [] },
    realtime: { current: {}, history: [] }
};

async function fetchVoiceConfig() {
    try {
        const res = await fetch('/api/voice/config');
        voiceConfigData = await res.json();
        renderVoiceConfig();
    } catch (e) { console.error('Voice config fetch failed', e); }
}

function fillVoiceForm(type, cfg) {
    const section = document.querySelector(`.voice-config-form[data-type="${type}"]`);
    if (!section) return;
    section.querySelector('.voice-provider').value = cfg.provider || '';
    section.querySelector('.voice-url').value = cfg.url || '';
    section.querySelector('.voice-key').value = '';  // 不回显真实 key
    section.querySelector('.voice-key').placeholder = cfg.has_key ? '已配置（留空不修改）' : '未配置';
    section.querySelector('.voice-model').value = cfg.model || '';
    section.querySelector('.voice-extra').value = cfg.extra || '';

    const tag = document.getElementById(`${type}-provider-tag`);
    if (tag) tag.textContent = cfg.provider || '未配置';
}

function renderVoiceHistory(type, list) {
    const container = document.getElementById(`${type}-history`);
    if (!container) return;
    container.innerHTML = '';

    if (!list || list.length === 0) {
        container.innerHTML = '<div class="voice-history-title">无历史配置</div>';
        return;
    }

    const title = document.createElement('div');
    title.className = 'voice-history-title';
    title.textContent = '历史配置（点击切换）';
    container.appendChild(title);

    list.forEach((cfg, index) => {
        const item = document.createElement('div');
        item.className = 'voice-history-item';
        item.innerHTML = `
            <div class="voice-history-info">
                <div class="voice-history-model">${cfg.provider} / ${cfg.model}</div>
                <div class="voice-history-url">${cfg.url}</div>
            </div>
            <div class="voice-history-actions">
                <button class="voice-btn-use" data-type="${type}" data-index="${index}">切换</button>
                <button class="voice-btn-delete" data-type="${type}" data-index="${index}">删除</button>
            </div>
        `;
        container.appendChild(item);
    });
}

function renderVoiceConfig() {
    ['asr', 'llm', 'tts', 'realtime'].forEach(type => {
        const section = voiceConfigData[type];
        if (section) {
            fillVoiceForm(type, section.current || {});
            renderVoiceHistory(type, section.history || []);
        }
    });
}

async function saveVoiceConfig(type, formData) {
    const current = voiceConfigData[type]?.current || {};
    const cfg = {
        provider: (formData.provider || current.provider || '').trim(),
        url: (formData.url || current.url || '').trim(),
        model: (formData.model || current.model || '').trim(),
        extra: (formData.extra || current.extra || '').trim()
    };
    const newKey = (formData.key || '').trim();
    if (newKey && newKey !== '********' && newKey !== 'sk-********') {
        cfg.key = newKey;
    }

    try {
        const res = await fetch('/api/voice/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ type, config: cfg })
        });
        const result = await res.json().catch(() => ({}));
        if (res.ok) {
            if (!result.persisted) {
                alert('设备未确认配置已持久化');
                return;
            }
            await fetchVoiceConfig();
            const saved = voiceConfigData[type]?.current || {};
            const fieldsMatch =
                saved.provider === cfg.provider &&
                saved.url === cfg.url &&
                saved.model === cfg.model &&
                saved.extra === cfg.extra &&
                (!newKey || saved.has_key);
            alert(fieldsMatch
                ? `${type.toUpperCase()} 配置已保存并写入 NVS`
                : `${type.toUpperCase()} 保存后校验不一致，请检查设备日志`);
        } else {
            alert(`保存失败：${result.error || result.code || res.status}`);
        }
    } catch (e) { alert('网络错误'); }
}

async function useVoiceHistory(type, index) {
    const cfg = voiceConfigData[type]?.history?.[index];
    if (!cfg) return;
    await saveVoiceConfig(type, cfg);
}

async function deleteVoiceHistory(type, index) {
    if (!confirm('确定删除这条历史配置吗？')) return;
    try {
        const res = await fetch('/api/voice/config/delete', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ type, index })
        });
        if (res.ok) fetchVoiceConfig();
        else alert('删除失败');
    } catch (e) { alert('网络错误'); }
}

function setupVoiceListeners() {
    document.querySelectorAll('.voice-config-form').forEach(form => {
        form.addEventListener('submit', async (e) => {
            e.preventDefault();
            const type = form.dataset.type;
            const data = {
                provider: form.querySelector('.voice-provider').value,
                url: form.querySelector('.voice-url').value,
                key: form.querySelector('.voice-key').value,
                model: form.querySelector('.voice-model').value,
                extra: form.querySelector('.voice-extra').value
            };
            await saveVoiceConfig(type, data);
        });
    });

    document.getElementById('tts-test-btn').addEventListener('click', async () => {
        const text = document.getElementById('tts-test-text').value.trim();
        if (!text) return;
        const btn = document.getElementById('tts-test-btn');
        btn.disabled = true;
        btn.textContent = '测试中...';
        try {
            const res = await fetch('/api/voice/test-tts', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ text })
            });
            const data = await res.json();
            if (data.status === 'ok') {
                alert(`TTS 测试成功，返回 ${data.audio_bytes} 字节 ${data.format} 音频`);
            } else {
                alert(`TTS 测试失败：${data.error || '未知错误'}`);
            }
        } catch (e) { alert('网络错误'); }
        btn.disabled = false;
        btn.textContent = '测试 TTS';
    });

    document.getElementById('chat-voice-btn').addEventListener('click',
        startBrowserVoice);
    document.getElementById('voice-stop-btn').addEventListener('click', () => {
        stopBrowserCapture(true);
        voiceLiveStatus.textContent = '语音会话已结束';
        voiceLiveDot.className = 'voice-live-dot';
    });
    document.getElementById('voice-interrupt-btn').addEventListener('click', () => {
        sendWsJson({ type: 'voice.interrupt' });
    });

    document.addEventListener('click', (e) => {
        if (e.target.classList.contains('voice-btn-use')) {
            useVoiceHistory(e.target.dataset.type, parseInt(e.target.dataset.index));
        }
        if (e.target.classList.contains('voice-btn-delete')) {
            deleteVoiceHistory(e.target.dataset.type, parseInt(e.target.dataset.index));
        }
    });
}

// Expose legacy helpers
window.removeIngredient = removeIngredient;
window.decreaseIngredient = decreaseIngredient;
window.increaseIngredient = increaseIngredient;
window.removeAllIngredient = removeAllIngredient;
window.openEditModal = openEditModal;
window.deleteNote = deleteNote;
window.openRecipeEdit = async (name) => {
    // To implement edit, we can fetch all recipes (not match) or find in match array
    try {
        const res = await fetch('/api/recipes/match');
        const data = await res.json();
        const match = data.find(m => m.recipe.name === name);
        if(!match) return;
        
        document.getElementById('recipe-modal-title').textContent = "编辑食谱";
        document.getElementById('recipe-old-name').value = match.recipe.name;
        document.getElementById('recipe-name').value = match.recipe.name;
        document.getElementById('recipe-category').value = match.recipe.category;
        document.getElementById('recipe-brief').value = match.recipe.brief;
        recipeIngList.innerHTML = '';
        match.recipe.ingredients.forEach(ing => {
            addRecipeIngRow(ing.name, ing.min_quantity);
        });
        recipeModal.classList.add('active');
    } catch(e) {}
};
window.deleteRecipe = deleteRecipe;

init();
