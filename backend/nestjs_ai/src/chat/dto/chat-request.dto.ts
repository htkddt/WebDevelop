import { IsNotEmpty, IsOptional, IsString, MaxLength } from 'class-validator';

export class ChatRequestDto {
  @IsString()
  @IsNotEmpty()
  @MaxLength(8000)
  contents!: string;

  @IsOptional()
  @IsString()
  @MaxLength(128)
  conversationId?: string;
}
