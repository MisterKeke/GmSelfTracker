let csrfToken = '';

export function setCSRF(token) { csrfToken = token || ''; }

export async function request(path, options = {}) {
  const headers = new Headers(options.headers || {});
  const method = (options.method || 'GET').toUpperCase();
  let body = options.body;
  if (body !== undefined && !(body instanceof FormData)) {
    headers.set('Content-Type', 'application/json');
    body = JSON.stringify(body);
  }
  if (method !== 'GET' && method !== 'HEAD' && csrfToken) headers.set('X-CSRF-Token', csrfToken);
  let response;
  try {
    response = await fetch(path, { ...options, method, body, headers, credentials: 'same-origin' });
  } catch {
    throw new Error('The local server could not be reached. Check that it is still running.');
  }
  if (response.status === 204) return null;
  const contentType = response.headers.get('content-type') || '';
  const payload = contentType.includes('application/json') ? await response.json() : null;
  if (!response.ok) throw new Error(payload?.error?.message || `Request failed (${response.status}).`);
  return payload;
}

export async function getItems(path) {
  const result = await request(path);
  return result?.items || [];
}
