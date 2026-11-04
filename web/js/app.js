import { ApiError, request, setCSRF } from './api.js';
import { renderAuth } from './auth.js';
import { initWorkspace } from './workspace.js';

const root = document.querySelector('#app');

async function start() {
  root.innerHTML = '<main class="loading-state"><span>✦</span> Opening your save file…</main>';
  try {
    const user = await request('/api/auth/me');
    if (!user || typeof user !== 'object' || typeof user.csrf_token !== 'string') {
      throw new ApiError('The server returned an invalid session response.', 200, 'invalid_response');
    }
    setCSRF(user.csrf_token);
    initWorkspace(root, user);
  } catch (error) {
    if (error instanceof ApiError && error.status === 401) {
      setCSRF('');
      renderAuth(root, async (user) => initWorkspace(root, user));
      return;
    }
    root.innerHTML = `<main class="auth-screen"><section class="auth-card"><div class="auth-inner"><div class="auth-logo" aria-hidden="true">🎮 ✦</div><h1 class="auth-title">COULD NOT OPEN THE LIBRARY</h1><p class="auth-copy" role="alert">${escapeHTML(error?.message || 'The session check failed.')}</p><button class="pixel-button gold" type="button" data-retry-session>TRY AGAIN</button></div></section></main>`;
    root.querySelector('[data-retry-session]').addEventListener('click', start);
  }
}

function escapeHTML(value) {
  return String(value).replace(/[&<>"']/g, (char) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));
}

start();
