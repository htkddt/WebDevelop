import { Injectable } from '@nestjs/common';
import { ConfigService } from '@nestjs/config';

@Injectable()
export class DatabaseConfig {
  constructor(private readonly configService: ConfigService) {}

  createMongooseOptions() {
    return {
      uri: this.configService.get<string>('MONGO_URL', 'mongodb://localhost:27017/179FC'),
    };
  }
}
