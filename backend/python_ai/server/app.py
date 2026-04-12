import os
import google.generativeai as genai
from dotenv import load_dotenv
from flask import Flask, request, jsonify
from flask_cors import CORS

load_dotenv()
GEMINI_API_KEY=os.getenv("GEMINI_API_KEY")

# Allow Frontend (port 3000) call data from Backend (port 5000)
app = Flask(__name__)
CORS(app)
# CORS(app, origins=["https://htkddt.github.io/"])

# Create gemini model
# _BASE_DIR = os.path.dirname(os.path.abspath(__file__))
# _API_KEY_FILE_PATH = os.path.join(_BASE_DIR, "API_KEY_LOCAL.txt")
# with open(_API_KEY_FILE_PATH, "r", encoding="utf-8") as f:
#     API_KEY = f.readline().strip()
genai.configure(api_key=GEMINI_API_KEY)
model = genai.GenerativeModel(
  model_name='gemini-flash-latest',
  system_instruction=(
    "Bạn là một trợ lý ảo tên là Klose Bot, cực kỳ thân thiện và hay gọi người dùng là 'ní'."
    "Bạn có kiến thức về lập trình và luôn cung cấp thông tin thời gian thực, chính xác dựa trên ngữ cảnh được cung cấp và đời thường."
  )
)

# --- Mock Data ---
employees_data = [
  {"id": 1, "name": "Nguyễn Văn A", "position": "Developer", "status": "Active"},
  {"id": 2, "name": "Trần Thị B", "position": "Designer", "status": "On Leave"},
  {"id": 3, "name": "Lê Văn C", "position": "Manager", "status": "Active"},
]

dashboard_stats = {
  "totalEmployees": 150,
  "pendingLeave": 5,
  "activeProjects": 12
}

# ------------------------------ POST ------------------------------
@app.route('/api/chat', methods=['POST'])
def chat():
  header = request.headers.get("type")
  data = request.get_json(force=True)
  msg = data.get("contents")
  # prompt = f"Bạn là 1 trợ lý ảo của anh Klose, hãy phản hồi đoạn message sau \"{msg}\" một cách rõ ràng, cụ thể, không dài dòng và tự nhiên đời thường."
  
  try:
    response = model.generate_content(str(msg))
    reply = response.text
  except Exception as e:
    reply = f"Gemini model error: {e}"
      
  return jsonify({"header": "-----\nTui đã nhận được message của ní rồi nhennn:\n "
    f" *** type:\"{header}\"\n  *** contents:\"{msg}\"\n-----\n",
                "reply": reply})

# ------------------------------ GET ------------------------------
@app.route('/api/dashboard', methods=['GET'])
def get_dashboard():
  return jsonify(dashboard_stats)

@app.route('/api/employees', methods=['GET'])
def get_employees():
  return jsonify(employees_data)

@app.route('/api/permissions', methods=['GET'])
def get_permissions_list():
  return jsonify(['Admin', 'Editor', 'Viewer'])

if __name__ == '__main__':
  port = int(os.environ.get("PORT", 5000))
  local = not os.environ.get("RENDER")
  app.run(host='0.0.0.0', port=port, debug=local)