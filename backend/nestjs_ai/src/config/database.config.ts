import { Injectable } from '@nestjs/common';
import { ConfigService } from '@nestjs/config';

@Injectable()
export class DatabaseConfig {
  constructor(private readonly configService: ConfigService) {}

  createMongooseOptions() {
    return {
      uri: this.configService.getOrThrow<string>('MONGO_URL'),
    };
  }
}
