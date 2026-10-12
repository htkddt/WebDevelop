from app.config import Settings
from app.providers.ollama import OllamaProvider
from app.schemas import ChatRequest, ChatResponse
from uuid import uuid4


class ChatService:
    """Application boundary for future RAG, memory and provider routing."""

    def __init__(self, settings: Settings):
        self._provider = OllamaProvider(settings)

    async def chat(self, request: ChatRequest) -> ChatResponse:
        reply = await self._provider.reply(request.message)
        return ChatResponse(
            reply=reply,
            conversation_id=request.conversation_id or str(uuid4()),
            provider=self._provider.name,
        )
