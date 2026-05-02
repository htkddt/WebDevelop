import os
import requests
from flask import Blueprint, request, jsonify
from dotenv import load_dotenv

load_dotenv()
OLLAMA_URL = os.getenv("OLLAMA_URL", "http://localhost:11434/api/generate")
OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "llama3")

chat_bp = Blueprint('chat', __name__)

@chat_bp.route('', methods=['POST'])
def chat():
    data = request.get_json(force=True)
    msg = data.get("contents")
    try:
        system_prompt = (
            "Bạn là một trợ lý ảo tên là Klose Bot, cực kỳ thân thiện và hay gọi người dùng là 'ní'."
            "Bạn có kiến thức về lập trình và luôn cung cấp thông tin thời gian thực, chính xác dựa trên ngữ cảnh được cung cấp và đời thường."
        )
        full_prompt = f"{system_prompt}\n\nUser: {msg}\nAssistant:"
        response = requests.post(OLLAMA_URL, json={
            "model": OLLAMA_MODEL,
            "prompt": full_prompt,
            "stream": False
        }, timeout=30)
        if response.status_code == 200:
            result = response.json()
            reply = result.get("response", "Not responsding.")
        else:
            reply = f"ERROR: {response.status_code} - {response.text}"
    except Exception as e:
        reply = f"ERROR: {e}"

    header = request.headers.get("type")
    return jsonify({
        "header": f"-----\nTui đã nhận được message của ní rồi nhennn:\n *** type:\"{header}\"\n *** contents:\"{msg}\"\n-----\n",
        "reply": reply
    })
