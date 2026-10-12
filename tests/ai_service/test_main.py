import asyncio
from unittest.mock import AsyncMock, patch

import httpx
import pytest
from fastapi.testclient import TestClient

from app.config import Settings, get_settings
from app.main import app
from app.providers.ollama import OllamaProvider, UpstreamChatError
from app.schemas import ChatRequest
from app.service import ChatService


@pytest.fixture(autouse=True)
def isolated_settings() -> None:
    """Keep route tests independent from a developer's local .env file."""
    app.dependency_overrides[get_settings] = lambda: Settings(ai_service_token='test-service-token')
    yield
    app.dependency_overrides.clear()


@pytest.fixture
def client() -> TestClient:
    return TestClient(app)


def test_healthcheck_is_public(client: TestClient) -> None:
    response = client.get('/healthz')

    assert response.status_code == 200
    assert response.json() == {'status': 'ok'}


def test_chat_requires_the_internal_service_token(client: TestClient) -> None:
    response = client.post('/v1/chat/completions', json={'message': 'Hello'})

    assert response.status_code == 401
    assert response.json()['detail'] == 'Invalid service token.'


def test_chat_returns_provider_response_for_a_valid_internal_request(client: TestClient) -> None:
    with patch('app.main.ChatService.chat', new=AsyncMock(return_value={
        'reply': 'Hello from Ollama.', 'conversation_id': 'conversation-1', 'provider': 'ollama',
    })):
        response = client.post(
            '/v1/chat/completions',
            headers={'X-Internal-Service-Token': 'test-service-token'},
            json={'message': 'Hello', 'conversation_id': 'conversation-1', 'user_id': 'user-1'},
        )

    assert response.status_code == 200
    assert response.json() == {
        'reply': 'Hello from Ollama.', 'conversation_id': 'conversation-1', 'provider': 'ollama',
    }


@pytest.mark.parametrize('payload', [{'message': ''}, {'message': 'x' * 8_001}])
def test_chat_rejects_invalid_message_lengths(client: TestClient, payload: dict[str, str]) -> None:
    response = client.post(
        '/v1/chat/completions',
        headers={'X-Internal-Service-Token': 'test-service-token'},
        json=payload,
    )

    assert response.status_code == 422


def test_chat_translates_upstream_provider_errors_to_bad_gateway(client: TestClient) -> None:
    with patch('app.main.ChatService.chat', new=AsyncMock(side_effect=UpstreamChatError('Provider down'))):
        response = client.post(
            '/v1/chat/completions',
            headers={'X-Internal-Service-Token': 'test-service-token'},
            json={'message': 'Hello'},
        )

    assert response.status_code == 502
    assert response.json()['detail'] == 'Provider down'


def test_chat_service_preserves_an_existing_conversation_id() -> None:
    async def run() -> object:
        settings = Settings()
        request = ChatRequest(message='Hello', conversation_id='conversation-1')
        with patch('app.service.OllamaProvider.reply', new=AsyncMock(return_value='Reply')):
            return await ChatService(settings).chat(request)

    response = asyncio.run(run())

    assert response.reply == 'Reply'
    assert response.conversation_id == 'conversation-1'
    assert response.provider == 'ollama'


def test_ollama_provider_sends_the_expected_non_streaming_prompt() -> None:
    recorded: dict[str, object] = {}

    class FakeClient:
        async def __aenter__(self) -> 'FakeClient':
            return self

        async def __aexit__(self, *_: object) -> None:
            return None

        async def post(self, url: str, json: dict[str, object]) -> httpx.Response:
            recorded['url'] = url
            recorded['json'] = json
            return httpx.Response(200, json={'response': '  Useful reply  '}, request=httpx.Request('POST', url))

    async def run() -> str:
        with patch('app.providers.ollama.httpx.AsyncClient', return_value=FakeClient()):
            return await OllamaProvider(Settings(ollama_url='http://ollama.test/generate', ollama_model='test-model')).reply('Where is my leave balance?')

    reply = asyncio.run(run())

    assert reply == 'Useful reply'
    assert recorded['url'] == 'http://ollama.test/generate'
    payload = recorded['json']
    assert payload['model'] == 'test-model'
    assert payload['stream'] is False
    assert 'User: Where is my leave balance?' in payload['prompt']


def test_ollama_provider_rejects_an_empty_provider_response() -> None:
    class FakeClient:
        async def __aenter__(self) -> 'FakeClient':
            return self

        async def __aexit__(self, *_: object) -> None:
            return None

        async def post(self, url: str, json: dict[str, object]) -> httpx.Response:
            return httpx.Response(200, json={'response': '   '}, request=httpx.Request('POST', url))

    async def run() -> None:
        with patch('app.providers.ollama.httpx.AsyncClient', return_value=FakeClient()):
            with pytest.raises(UpstreamChatError, match='invalid response'):
                await OllamaProvider(Settings()).reply('Hello')

    asyncio.run(run())
