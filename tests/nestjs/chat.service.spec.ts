import { BadGatewayException } from '@nestjs/common';
import axios from 'axios';
import { ChatService } from '../../backend/nestjs_ai/src/chat/chat.service';

jest.mock('axios');

describe('ChatService', () => {
  const configService = {
    getOrThrow: jest.fn((key: string) => ({
      AI_SERVICE_URL: 'http://ai.test', AI_SERVICE_TOKEN: 'shared-token',
    }[key])),
  };

  beforeEach(() => jest.clearAllMocks());

  it('forwards the authenticated user and optional conversation to the AI service', async () => {
    (axios.post as jest.Mock).mockResolvedValue({
      data: { reply: 'Hello!', conversation_id: 'conversation-1' },
    });
    const service = new ChatService(configService as any);

    await expect(service.chat({ contents: 'Hi', conversationId: 'existing' }, 'user-1')).resolves.toEqual({
      header: '', reply: 'Hello!', conversationId: 'conversation-1',
    });
    expect(axios.post).toHaveBeenCalledWith('http://ai.test/v1/chat/completions', {
      message: 'Hi', conversation_id: 'existing', user_id: 'user-1',
    }, {
      headers: { 'X-Internal-Service-Token': 'shared-token' }, timeout: 30_000,
    });
  });

  it('hides upstream failures behind a stable gateway error', async () => {
    (axios.post as jest.Mock).mockRejectedValue(new Error('connection refused'));
    const service = new ChatService(configService as any);

    await expect(service.chat({ contents: 'Hi' }, 'user-1')).rejects.toBeInstanceOf(BadGatewayException);
  });
});
