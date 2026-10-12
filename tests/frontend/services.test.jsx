import { ApiError, apiRequest } from '../../frontend/src/services/apiClient';
import { authApi, chatApi, dashboardApi, employeesApi } from '../../frontend/src/services/api';
import { authStorage } from '../../frontend/src/services/authStorage';

const jsonResponse = (body, ok = true, status = 200) => ({
  ok,
  status,
  headers: { get: () => 'application/json; charset=utf-8' },
  json: jest.fn().mockResolvedValue(body),
});

describe('browser API client', () => {
  beforeEach(() => {
    global.fetch = jest.fn();
  });

  it('persists and clears an authenticated session', () => {
    authStorage.setSession({ token: 'jwt-token', user: { id: 'u1', username: 'Kiet' } });

    expect(authStorage.getToken()).toBe('jwt-token');
    expect(JSON.parse(localStorage.getItem('user'))).toEqual({ id: 'u1', username: 'Kiet' });

    authStorage.clear();
    expect(authStorage.getToken()).toBeNull();
    expect(localStorage.getItem('user')).toBeNull();
  });

  it('adds JSON and bearer-token headers, then returns JSON', async () => {
    authStorage.setSession({ token: 'jwt-token', user: {} });
    fetch.mockResolvedValue(jsonResponse({ id: 'u1' }));

    await expect(apiRequest('/users', { method: 'POST', body: JSON.stringify({ name: 'Kiet' }) }))
      .resolves.toEqual({ id: 'u1' });

    expect(fetch).toHaveBeenCalledWith('http://api.test/api/users', expect.objectContaining({
      method: 'POST',
      headers: expect.objectContaining({
        'Content-Type': 'application/json',
        Authorization: 'Bearer jwt-token',
      }),
    }));
  });

  it('turns an unsuccessful response into an ApiError with server detail', async () => {
    fetch.mockResolvedValue(jsonResponse({ message: 'Invalid credentials' }, false, 401));

    await expect(apiRequest('/auth/login')).rejects.toEqual(
      expect.objectContaining({ name: 'ApiError', message: 'Invalid credentials', status: 401 }),
    );
  });

  it('uses the intended endpoint and body for each public API feature', async () => {
    fetch.mockResolvedValue(jsonResponse({ ok: true }));

    await authApi.login({ email: 'kiet@klose.dev', password: 'secret' });
    await authApi.register({ username: 'Kiet' });
    await dashboardApi.getStats();
    await employeesApi.getAll();
    await chatApi.sendMessage('Hello');

    expect(fetch.mock.calls.map(([url]) => url)).toEqual([
      'http://api.test/api/auth/login',
      'http://api.test/api/auth/register',
      'http://api.test/api/dashboard',
      'http://api.test/api/users/employees',
      'http://api.test/api/chat',
    ]);
    expect(fetch.mock.calls[4][1].body).toBe(JSON.stringify({ contents: 'Hello' }));
  });

  it('keeps ApiError available to feature code', () => {
    const error = new ApiError('Unavailable', 503);
    expect(error).toBeInstanceOf(Error);
    expect(error.status).toBe(503);
  });
});
