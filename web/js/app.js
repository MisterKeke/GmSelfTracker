import { request, setCSRF } from './api.js';
import { renderAuth } from './auth.js';
import { initWorkspace } from './workspace.js';

const root = document.querySelector('#app');

async function start() {
  root.innerHTML = '<main class="loading-state"><span>✦</span> Opening your save file…</main>';
  try {
    const user = await request('/api/auth/me');
    setCSRF(user.csrf_token);
    initWorkspace(root, user);
  } catch {
    setCSRF('');
    renderAuth(root, async (user) => initWorkspace(root, user));
  }
}

start();
