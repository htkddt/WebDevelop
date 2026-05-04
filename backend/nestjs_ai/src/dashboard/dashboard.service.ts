import { Injectable } from '@nestjs/common';
import { InjectModel } from '@nestjs/mongoose';
import { Model } from 'mongoose';
import { User, UserDocument } from '../user/user.model';

@Injectable()
export class DashboardService {
  constructor(@InjectModel(User.name) private readonly userModel: Model<UserDocument>) {}

  async getStats() {
    const total = await this.userModel.countDocuments().exec();
    const active = await this.userModel.countDocuments({ status: 'active' }).exec();
    const inactive = await this.userModel.countDocuments({ status: 'inactive' }).exec();
    const depts = await this.userModel.distinct('dept').exec();

    return {
      total,
      active,
      inactive,
      departments: depts.length,
    };
  }
}
