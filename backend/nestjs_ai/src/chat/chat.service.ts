import { BadGatewayException, Injectable } from '@nestjs/common';
import { ConfigService } from '@nestjs/config';
import axios from 'axios';
import { ChatRequestDto } from './dto/chat-request.dto';

interface AiServiceResponse {
  reply: string;
  conversation_id: string;
}

@Injectable()
export class ChatService {
  private readonly aiServiceUrl: string;
  private readonly aiServiceToken: string;

  constructor(configService: ConfigService) {
    this.aiServiceUrl = configService.getOrThrow<string>('AI_SERVICE_URL');
    this.aiServiceToken = configService.getOrThrow<string>('AI_SERVICE_TOKEN');
  }

  async chat(request: ChatRequestDto, userId: string) {
    try {
      const response = await axios.post<AiServiceResponse>(
        `${this.aiServiceUrl}/v1/chat/completions`,
        {
          message: request.contents,
          conversation_id: request.conversationId,
          user_id: userId,
        },
        {
          headers: { 'X-Internal-Service-Token': this.aiServiceToken },
          timeout: 30_000,
        },
      );

      // `header` remains for the current frontend chat payload contract.
      return {
        header: '',
        reply: response.data.reply,
        conversationId: response.data.conversation_id,
      };
    } catch {
      throw new BadGatewayException('The AI service is unavailable.');
    }
  }
}
