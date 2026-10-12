import { BadRequestException, UnauthorizedException } from '@nestjs/common';
import { AuthService } from '../../backend/nestjs_ai/src/auth/auth.service';

jest.mock('bcrypt', () => ({
  hash: jest.fn(),
  compare: jest.fn(),
}));

import * as bcrypt from 'bcrypt';

describe('AuthService', () => {
  const jwtService = { sign: jest.fn(() => 'signed-jwt') };
  let userModel: any;
  let service: AuthService;

  beforeEach(() => {
    jest.clearAllMocks();
    userModel = jest.fn();
    userModel.findOne = jest.fn();
    service = new AuthService(userModel, jwtService as any);
  });

  it('returns a token and a public user profile for valid credentials', async () => {
    const user = {
      _id: { toString: () => 'user-1' }, username: 'Kiet', email: 'kiet@klose.dev',
      password: 'hashed-password', role: 'staff', dept: 'Engineering', permissions: 'view', status: 'active',
    };
    userModel.findOne.mockReturnValue({ exec: jest.fn().mockResolvedValue(user) });
    (bcrypt.compare as jest.Mock).mockResolvedValue(true);

    await expect(service.login({ email: user.email, password: 'secret' })).resolves.toEqual({
      token: 'signed-jwt',
      user: {
        id: 'user-1', username: 'Kiet', email: 'kiet@klose.dev', role: 'staff',
        dept: 'Engineering', permissions: 'view', status: 'active',
      },
    });
    expect(jwtService.sign).toHaveBeenCalledWith({ userId: user._id, role: 'staff' });
  });

  it('rejects missing users and invalid passwords without signing a token', async () => {
    userModel.findOne.mockReturnValue({ exec: jest.fn().mockResolvedValue(null) });

    await expect(service.login({ email: 'missing@klose.dev', password: 'secret' }))
      .rejects.toBeInstanceOf(UnauthorizedException);
    expect(jwtService.sign).not.toHaveBeenCalled();
  });

  it('rejects duplicate email addresses before password hashing', async () => {
    userModel.findOne.mockReturnValue({ exec: jest.fn().mockResolvedValue({ _id: 'existing' }) });

    await expect(service.register({
      username: 'Kiet', email: 'kiet@klose.dev', password: 'secret1', role: 'staff',
      dept: 'Engineering', permissions: 'view', status: 'active',
    })).rejects.toBeInstanceOf(BadRequestException);
    expect(bcrypt.hash).not.toHaveBeenCalled();
  });

  it('hashes passwords and saves a new user without exposing the hash', async () => {
    const save = jest.fn().mockResolvedValue(undefined);
    userModel.findOne.mockReturnValue({ exec: jest.fn().mockResolvedValue(null) });
    userModel.mockImplementation((data: unknown) => ({ ...(data as object), save }));
    (bcrypt.hash as jest.Mock).mockResolvedValue('hashed-password');

    const result = await service.register({
      username: 'Kiet', email: 'kiet@klose.dev', password: 'secret1', role: 'staff',
      dept: 'Engineering', permissions: 'view', status: 'active',
    });

    expect(userModel).toHaveBeenCalledWith(expect.objectContaining({ password: 'hashed-password' }));
    expect(save).toHaveBeenCalled();
    expect(result).toEqual({ message: 'Successfully registered' });
  });
});
