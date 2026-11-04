let csrfToken = '';

export function setCSRF(token) { csrfToken = token || ''; }

export class ApiError extends Error {
  constructor(message, status = 0, code = 'unknown_error') {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
  }
}

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
  } catch (error) {
    if (error?.name === 'AbortError') throw error;
    throw new ApiError('The local server could not be reached. Check that it is still running.', 0, 'network_error');
  }
  if (response.status === 204) return null;
  const contentType = response.headers.get('content-type') || '';
  let payload = null;
  if (contentType.includes('application/json')) {
    try { payload = await response.json(); }
    catch { throw new ApiError('The server returned an invalid response.', response.status, 'invalid_response'); }
  }
  if (!response.ok) throw new ApiError(payload?.error?.message || `Request failed (${response.status}).`, response.status, payload?.error?.code || 'http_error');
  if (!contentType.includes('application/json')) throw new ApiError('The server returned an unexpected response.', response.status, 'invalid_response');
  return payload;
}

export async function getItems(path) {
  const items = [];
  const url = new URL(path, window.location.origin);
  let offset = Number(url.searchParams.get('offset') || 0);
  while (true) {
    url.searchParams.set('offset', offset);
    const result = await request(url.pathname + url.search);
    items.push(...(result?.items || []));
    if (!result?.limit || result.items.length < result.limit) return items;
    offset += result.limit;
    if (offset > 1000000) throw new Error('This collection exceeds the supported pagination range.');
    url.searchParams.set('offset', offset);
  }
}
