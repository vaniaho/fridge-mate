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
const sysHeap = document.getElementById('sys-heap');
const sysWifi = document.getElementById('sys-wifi');

// Inventory
const categoryContainer = document.getElementById('category-container');
const countSpan = document.getElementById('item-count');
const addModal = document.getElementById('add-modal');
const addForm = document.getElementById('add-form');

// Chat
const chatMessages = document.getElementById('chat-messages');
const chatForm = document.getElementById('chat-form');
const chatInput = document.getElementById('chat-input');
const chatSend = document.getElementById('chat-send');

// Settings
const settingsForm = document.getElementById('settings-form');

const iconMap = { '水果': '🍎', '蔬菜': '🥬', '肉类': '🥩', '饮品': '🥛', '其他': '📦' };

function init() {
    setupRouter();
    setupEventListeners();
    fetchInventory();
    fetchStatus();
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
    countSpan.textContent = `${state.inventory.length} 项`;
    categoryContainer.innerHTML = '';
    
    // Group by category
    const grouped = {};
    state.inventory.forEach(item => {
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
                <button class="remove-btn" onclick="removeIngredient('${item.name}')" title="取出1个">
                    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="5" y1="12" x2="19" y2="12"></line></svg>
                </button>
                <div class="item-icon">${iconMap[item.category] || '📦'}</div>
                <div class="item-name">${item.name}</div>
                <div class="item-quantity">数量: ${item.quantity}</div>
                <div class="item-expire ${expireClass}">保质期 ${item.expire_days} 天</div>
            `;
            grid.appendChild(card);
        });
        groupDiv.appendChild(grid);
        categoryContainer.appendChild(groupDiv);
    }
}

async function removeIngredient(name) {
    await fetch('/api/inventory', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'remove', name: name, quantity: 1 })
    });
    fetchInventory();
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
            const data = await res.json();
            addChatMessage(data.reply || "指令已执行！");
            fetchInventory(); // Refresh immediately
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
        document.getElementById('set-url').value = data.llm_url || '';
        document.getElementById('set-key').value = data.llm_key || '';
        document.getElementById('set-model').value = data.llm_model || '';
    } catch(e) { console.error(e); }
}

settingsForm.addEventListener('submit', async (e) => {
    e.preventDefault();
    const data = {
        wifi_ssid: document.getElementById('set-ssid').value,
        wifi_pass: document.getElementById('set-pass').value,
        llm_url: document.getElementById('set-url').value,
        llm_key: document.getElementById('set-key').value,
        llm_model: document.getElementById('set-model').value,
    };
    try {
        const res = await fetch('/api/settings', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(data)
        });
        if (res.ok) {
            alert('设置已保存，部分配置可能需要重启生效。');
        } else {
            alert('保存失败！');
        }
    } catch(e) { alert('网络错误'); }
});

// === Global Listeners & WS ===
function setupEventListeners() {
    document.getElementById('add-btn').addEventListener('click', () => {
        addModal.classList.add('active');
    });
    document.getElementById('cancel-btn').addEventListener('click', () => {
        addModal.classList.remove('active');
        addForm.reset();
    });
    addForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const data = {
            action: 'add',
            name: document.getElementById('name').value,
            category: document.getElementById('category').value,
            quantity: parseInt(document.getElementById('quantity').value),
            expire_days: parseInt(document.getElementById('expire').value)
        };
        const res = await fetch('/api/inventory', {
            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data)
        });
        if (res.ok) {
            addModal.classList.remove('active');
            addForm.reset();
            fetchInventory();
            if (document.getElementById('view-history').classList.contains('active')) fetchHistory();
        }
    });
    setInterval(fetchStatus, 5000); // Poll status every 5s
}

function setupWebSocket() {
    if (location.protocol === 'file:') return;
    ws = new WebSocket(`${wsProto}//${location.host}/ws`);
    ws.onopen = () => { dot.classList.add('connected'); connStatus.textContent = "已连接"; };
    ws.onclose = () => { dot.classList.remove('connected'); connStatus.textContent = "离线重连中..."; setTimeout(setupWebSocket, 3000); };
    ws.onmessage = (e) => {
        if (e.data === 'update') {
            fetchInventory();
            if (document.getElementById('view-history').classList.contains('active')) fetchHistory();
        }
    };
}

window.removeIngredient = removeIngredient;
init();
