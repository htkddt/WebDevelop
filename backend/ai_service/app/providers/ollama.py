import httpx

from app.config import Settings


class UpstreamChatError(Exception):
    """Raised when the configured model provider cannot produce a response."""


class OllamaProvider:
    name = 'ollama'

    def __init__(self, settings: Settings):
        self._settings = settings

    async def reply(self, message: str) -> str:
        prompt = (
            'You are Klose Bot, a friendly assistant for an employee-management '
            'application. Give useful, concise answers.\n\n'
            f'User: {message}\nAssistant:'
        )

        try:
            async with httpx.AsyncClient(timeout=self._settings.request_timeout_seconds) as client:
                response = await client.post(
                    self._settings.ollama_url,
                    json={
                        'model': self._settings.ollama_model,
                        'prompt': prompt,
                        'stream': False,
                    },
                )
                response.raise_for_status()
        except httpx.HTTPError as error:
            raise UpstreamChatError('The model provider is unavailable.') from error

        reply = response.json().get('response')
        if not isinstance(reply, str) or not reply.strip():
            raise UpstreamChatError('The model provider returned an invalid response.')

        return reply.strip()
