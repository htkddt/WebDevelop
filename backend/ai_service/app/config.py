from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """Configuration owned by the AI service, never exposed to the browser."""

    ai_service_token: str = ''
    ollama_url: str = 'http://localhost:11434/api/generate'
    ollama_model: str = 'llama3'
    request_timeout_seconds: float = 30.0
    max_message_characters: int = 8_000

    model_config = SettingsConfigDict(env_file='.env', env_file_encoding='utf-8')


@lru_cache
def get_settings() -> Settings:
    return Settings()
