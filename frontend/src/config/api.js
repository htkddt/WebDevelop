const LOCAL_API_BASE_URL = 'http://localhost:5000/api';
// Production must set VITE_API_BASE_URL to the public NestJS /api URL at build time.
export const API_BASE_URL = import.meta.env.VITE_API_BASE_URL || LOCAL_API_BASE_URL;

export const API_ENDPOINTS = Object.freeze({
  auth: {
    login: '/auth/login',
    register: '/auth/register',
  },
  chat: '/chat',
  dashboard: '/dashboard',
  employees: '/users/employees',
});
