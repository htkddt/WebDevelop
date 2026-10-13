import pytest
from app import app
import requests

@pytest.fixture
def client():
    app.config['TESTING'] = True
    with app.test_client() as client:
        yield client

def test_chat_without_ollama(client, monkeypatch):
    # Mock requests.post to raise exception (simulate Ollama not running)
    def mock_post(*args, **kwargs):
        raise requests.exceptions.ConnectionError("Connection refused")
    
    monkeypatch.setattr('requests.post', mock_post)
    response = client.post('/api/chat', json={'contents': 'Hello'})
    assert response.status_code == 200
    data = response.get_json()
    assert 'reply' in data
    assert 'error' in data['reply']

def test_chat_with_ollama(client, monkeypatch):
    # Mock requests.post to return a fake Ollama response
    def mock_post(*args, **kwargs):
        class MockResponse:
            status_code = 200
            def json(self):
                return {"response": "Ollama connected!"}
        return MockResponse()
    
    monkeypatch.setattr('requests.post', mock_post)
    response = client.post('/api/chat', json={'contents': 'Hello'})
    assert response.status_code == 200
    data = response.get_json()
    assert 'reply' in data
    assert 'Ollama connected!' in data['reply']