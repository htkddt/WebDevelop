from uuid import uuid4

from pydantic import BaseModel, Field


class ChatRequest(BaseModel):
    message: str = Field(min_length=1, max_length=8_000)
    conversation_id: str | None = Field(default=None, max_length=128)
    user_id: str | None = Field(default=None, max_length=128)


class ChatResponse(BaseModel):
    reply: str
    conversation_id: str = Field(default_factory=lambda: str(uuid4()))
    provider: str
