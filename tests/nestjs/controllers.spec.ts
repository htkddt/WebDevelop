import { AuthController } from '../../backend/nestjs_ai/src/auth/auth.controller';
import { ChatController } from '../../backend/nestjs_ai/src/chat/chat.controller';
import { DashboardController } from '../../backend/nestjs_ai/src/dashboard/dashboard.controller';
import { UsersController } from '../../backend/nestjs_ai/src/users/users.controller';

describe('HTTP controllers', () => {
  it('delegates auth payloads to AuthService', async () => {
    const authService = { login: jest.fn().mockResolvedValue({ token: 'jwt' }), register: jest.fn().mockResolvedValue({ message: 'ok' }) };
    const controller = new AuthController(authService as any);
    const login = { email: 'kiet@klose.dev', password: 'secret' };
    const registration = { ...login, username: 'Kiet', role: 'staff', dept: 'Engineering', permissions: 'view', status: 'active' };

    await expect(controller.login(login)).resolves.toEqual({ token: 'jwt' });
    await expect(controller.register(registration)).resolves.toEqual({ message: 'ok' });
    expect(authService.login).toHaveBeenCalledWith(login);
    expect(authService.register).toHaveBeenCalledWith(registration);
  });

  it('delegates protected feature requests with the JWT subject', async () => {
    const chatService = { chat: jest.fn().mockResolvedValue({ reply: 'Hi' }) };
    const dashboardService = { getStats: jest.fn().mockResolvedValue({ total: 1 }) };
    const usersService = { getAllEmployees: jest.fn().mockResolvedValue([{ username: 'Kiet' }]) };

    await expect(new ChatController(chatService as any).chat({ contents: 'Hello' }, { user: { userId: 'u1' } })).resolves.toEqual({ reply: 'Hi' });
    await expect(new DashboardController(dashboardService as any).getStats()).resolves.toEqual({ total: 1 });
    await expect(new UsersController(usersService as any).getEmployees()).resolves.toEqual([{ username: 'Kiet' }]);
    expect(chatService.chat).toHaveBeenCalledWith({ contents: 'Hello' }, 'u1');
  });
});
