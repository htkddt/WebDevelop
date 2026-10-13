import { IsEmail, IsNotEmpty, MinLength } from 'class-validator';

export class RegisterDto {
  @IsNotEmpty()
  username: string;

  @IsEmail()
  email: string;

  @IsNotEmpty()
  @MinLength(6)
  password: string;

  @IsNotEmpty()
  role: string;

  @IsNotEmpty()
  dept: string;

  @IsNotEmpty()
  permission: string;

  @IsNotEmpty()
  status: string;
}
