import { API_ENDPOINTS } from '../config/api';
import { apiRequest } from './apiClient';

export const authApi = {
  login: (credentials) => apiRequest(API_ENDPOINTS.auth.login, {
    method: 'POST',
    body: JSON.stringify(credentials),
  }),
  register: (user) => apiRequest(API_ENDPOINTS.auth.register, {
    method: 'POST',
    body: JSON.stringify(user),
  }),
};

export const dashboardApi = {
  getStats: () => apiRequest(API_ENDPOINTS.dashboard),
};

export const employeesApi = {
  getAll: () => apiRequest(API_ENDPOINTS.employees),
};

export const chatApi = {
  sendMessage: (contents) => apiRequest(API_ENDPOINTS.chat, {
    method: 'POST',
    body: JSON.stringify({ contents }),
  }),
};
