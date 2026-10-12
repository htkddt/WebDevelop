import { Body, Controller, Post, Request, UseGuards } from '@nestjs/common';
import { ChatService } from './chat.service';
import { JwtAuthGuard } from '../auth/jwt-auth.guard';
import { ChatRequestDto } from './dto/chat-request.dto';

@Controller('chat')
@UseGuards(JwtAuthGuard)
export class ChatController {
  constructor(private readonly chatService: ChatService) {}

  @Post()
  async chat(@Body() body: ChatRequestDto, @Request() request: { user: { userId: string } }) {
    return this.chatService.chat(body, request.user.userId);
  }
}
