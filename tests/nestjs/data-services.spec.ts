import { DashboardService } from '../../backend/nestjs_ai/src/dashboard/dashboard.service';
import { UsersService } from '../../backend/nestjs_ai/src/users/users.service';
import { DatabaseConfig } from '../../backend/nestjs_ai/src/config/database.config';

describe('data services', () => {
  it('aggregates dashboard statistics using the expected filters', async () => {
    const countDocuments = jest.fn()
      .mockReturnValueOnce({ exec: jest.fn().mockResolvedValue(10) })
      .mockReturnValueOnce({ exec: jest.fn().mockResolvedValue(8) })
      .mockReturnValueOnce({ exec: jest.fn().mockResolvedValue(2) });
    const userModel = {
      countDocuments,
      distinct: jest.fn().mockReturnValue({ exec: jest.fn().mockResolvedValue(['Engineering', 'HR']) }),
    };

    await expect(new DashboardService(userModel as any).getStats()).resolves.toEqual({
      total: 10, active: 8, inactive: 2, departments: 2,
    });
    expect(countDocuments).toHaveBeenNthCalledWith(1);
    expect(countDocuments).toHaveBeenNthCalledWith(2, { status: 'active' });
    expect(countDocuments).toHaveBeenNthCalledWith(3, { status: 'inactive' });
  });

  it('returns employees while explicitly excluding their password', async () => {
    const exec = jest.fn().mockResolvedValue([{ username: 'Kiet' }]);
    const find = jest.fn().mockReturnValue({ exec });
    const service = new UsersService({ find } as any);

    await expect(service.getAllEmployees()).resolves.toEqual([{ username: 'Kiet' }]);
    expect(find).toHaveBeenCalledWith({}, { password: 0 });
  });

  it('uses MONGO_URL from configuration for Mongoose', () => {
    const configService = { getOrThrow: jest.fn().mockReturnValue('mongodb://mongo.test/179fc') };

    expect(new DatabaseConfig(configService as any).createMongooseOptions()).toEqual({
      uri: 'mongodb://mongo.test/179fc',
    });
    expect(configService.getOrThrow).toHaveBeenCalledWith('MONGO_URL');
  });
});
