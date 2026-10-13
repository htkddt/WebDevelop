import { BadRequestException, Injectable, UnauthorizedException } from '@nestjs/common';
import { InjectModel } from '@nestjs/mongoose';
import { Model } from 'mongoose';
import { JwtService } from '@nestjs/jwt';
import * as bcrypt from 'bcrypt';
import { User, UserDocument } from '../user/user.model';
import { LoginDto } from './dto/login.dto';
import { RegisterDto } from './dto/register.dto';

@Injectable()
export class AuthService {
  constructor(
    @InjectModel(User.name) private readonly userModel: Model<UserDocument>,
    private readonly jwtService: JwtService,
  ) {}

  private async hashPassword(password: string): Promise<string> {
    return bcrypt.hash(password, 10);
  }

  private async verifyPassword(password: string, hashed: string): Promise<boolean> {
    return bcrypt.compare(password, hashed);
  }

  async login(loginDto: LoginDto) {
    const user = await this.userModel.findOne({ email: loginDto.email }).exec();
    if (!user || !(await this.verifyPassword(loginDto.password, user.password))) {
      throw new UnauthorizedException('Invalid email or password');
    }
    const token = this.jwtService.sign({ userId: user._id, role: user.role });
    return {
      token,
      user: {
        username: user.username,
        email: user.email,
        role: user.role,
        dept: user.dept,
        permission: user.permission,
        status: user.status,
      },
    };
  }

  async register(registerDto: RegisterDto) {
    const existing = await this.userModel.findOne({ email: registerDto.email }).exec();
    if (existing) {
      throw new BadRequestException('Email already exists');
    }
    const password = await this.hashPassword(registerDto.password);
    const user = new this.userModel({ ...registerDto, password });
    await user.save();
    return { message: 'Successfully registered' };
  }
}
