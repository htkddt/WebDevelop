from unittest.mock import AsyncMock, patch

from fastapi.testclient import TestClient

from app.main import app

client = TestClient(app)


def test_healthcheck() -> None:
    response = client.get('/healthz')

    assert response.status_code == 200
    assert response.json() == {'status': 'ok'}


def test_chat_returns_provider_response() -> None:
    with patch('app.service.OllamaProvider.reply', new=AsyncMock(return_value='Hello from Ollama.')):
        response = client.post('/v1/chat/completions', json={'message': 'Hello'})

    assert response.status_code == 200
    assert response.json()['reply'] == 'Hello from Ollama.'
    assert response.json()['provider'] == 'ollama'
