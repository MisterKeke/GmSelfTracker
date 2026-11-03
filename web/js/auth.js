import { request, setCSRF } from './api.js';

export function renderAuth(root, onAuthenticated) {
  let mode = 'login';
  root.innerHTML = '';

  const draw = (message = '') => {
    const registering = mode === 'register';
    root.innerHTML = `
      <main class="auth-screen">
        <section class="auth-card" aria-labelledby="auth-heading">
          <div class="auth-inner">
            <div class="auth-logo" aria-hidden="true">🎮 ✦</div>
            <h1 class="auth-title pixel-title" id="auth-heading">GAME TRACKER</h1>
            <p class="auth-copy">A little pixel world for every game you love.<br>Track your way, your rules.</p>
            <p class="auth-notice" role="alert">${escapeHTML(message)}</p>
            <form id="auth-form" class="form-grid" autocomplete="on">
              <div class="form-field wide">
                <label for="auth-username">Username</label>
                <input id="auth-username" name="username" required minlength="3" maxlength="32" pattern="[A-Za-z0-9_-]{3,32}" autocomplete="username" placeholder="Your adventurer name">
              </div>
              <div class="form-field wide">
                <label for="auth-password">Password</label>
                <input id="auth-password" name="password" required minlength="10" maxlength="72" type="password" autocomplete="${registering ? 'new-password' : 'current-password'}" placeholder="At least 10 characters">
              </div>
              ${registering ? `<div class="form-field wide"><label for="auth-confirm">Confirm password</label><input id="auth-confirm" name="confirm_password" required minlength="10" maxlength="72" type="password" autocomplete="new-password" placeholder="Type your password again"></div>` : ''}
              <div class="form-field wide"><button class="pixel-button gold" type="submit">${registering ? 'CREATE ACCOUNT' : 'ENTER YOUR LIBRARY'} ✦</button></div>
            </form>
            <button class="auth-switch" type="button" data-auth-switch>${registering ? 'Already have an account? Sign in' : 'New around here? Create an account'}</button>
            <p class="auth-copy" style="font-size:11px;margin:7px auto 0">Your collection is stored locally on this device.</p>
          </div>
        </section>
      </main>`;

    root.querySelector('[data-auth-switch]').addEventListener('click', () => { mode = registering ? 'login' : 'register'; draw(); });
    root.querySelector('#auth-form').addEventListener('submit', async (event) => {
      event.preventDefault();
      const form = event.currentTarget;
      const data = Object.fromEntries(new FormData(form));
      if (registering && data.password !== data.confirm_password) { draw('Those passwords do not match.'); return; }
      const submit = form.querySelector('button[type=submit]');
      submit.disabled = true;
      submit.textContent = registering ? 'CREATING YOUR ACCOUNT…' : 'OPENING YOUR LIBRARY…';
      try {
        const user = await request(`/api/auth/${registering ? 'register' : 'login'}`, { method: 'POST', body: data });
        setCSRF(user.csrf_token);
        await onAuthenticated(user);
      } catch (error) {
        draw(error.message);
      }
    });
  };
  draw();
}

function escapeHTML(value) { return String(value).replace(/[&<>"']/g, (char) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char])); }
