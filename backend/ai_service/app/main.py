from typing import Annotated

from fastapi import Depends, FastAPI, Header, HTTPException, status

from app.config import Settings, get_settings
from app.providers.ollama import UpstreamChatError
from app.schemas import ChatRequest, ChatResponse
from app.service import ChatService

app = FastAPI(title='179FC AI Service', version='1.0.0')


def verify_internal_token(
    x_internal_service_token: Annotated[str | None, Header()] = None,
    settings: Settings = Depends(get_settings),
) -> None:
    """Require a shared service token outside local development."""
    if settings.ai_service_token and x_internal_service_token != settings.ai_service_token:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail='Invalid service token.')


@app.get('/healthz', tags=['operations'])
async def healthcheck() -> dict[str, str]:
    return {'status': 'ok'}


@app.post(
    '/v1/chat/completions',
    response_model=ChatResponse,
    dependencies=[Depends(verify_internal_token)],
    tags=['chat'],
)
async def create_chat_completion(
    request: ChatRequest,
    settings: Settings = Depends(get_settings),
) -> ChatResponse:
    try:
        return await ChatService(settings).chat(request)
    except UpstreamChatError as error:
        raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail=str(error)) from error
