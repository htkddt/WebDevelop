import { HttpException, HttpStatus, Injectable } from '@nestjs/common';
import axios from 'axios';

@Injectable()
export class ChatService {
  private readonly ollamaUrl: string;
  private readonly ollamaModel: string;

  constructor() {
    this.ollamaUrl = process.env.OLLAMA_URL || 'http://localhost:11434/api/generate';
    this.ollamaModel = process.env.OLLAMA_MODEL || 'llama2';
  }

  async chat(contents: string) {
    const systemInstruction =
      "Bạn là một trợ lý ảo tên là Klose Bot, cực kỳ thân thiện và hay gọi người dùng là 'ní'. " +
      "Bạn có kiến thức về lập trình và luôn cung cấp thông tin thời gian thực, chính xác dựa trên ngữ cảnh được cung cấp và đời thường.";
    const prompt = `${systemInstruction}\n\nUser: ${contents}\nAssistant:`;

    try {
      const response = await axios.post(
        this.ollamaUrl,
        {
          model: this.ollamaModel,
          prompt,
          stream: false,
        },
        { timeout: 30000 },
      );

      const reply = response.data?.response ?? 'Không có phản hồi từ Ollama.';
      return {
        header: `-----\nTui đã nhận được message của ní rồi nhennn:\n *** contents:"${contents}"\n-----\n`,
        reply,
      };
    } catch (error) {
      throw new HttpException(`Lỗi khi gọi Ollama: ${error.message || 'unknown'}`, HttpStatus.BAD_GATEWAY);
    }
  }
}
