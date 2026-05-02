import os
import google.generativeai as genai
from dotenv import load_dotenv
from flask import Flask, request, jsonify
from flask_cors import CORS

from routes.auth import auth_bp
from routes.users import users_bp
from routes.dashboard import dashboard_bp
from routes.chat import chat_bp

# Allow Frontend (port 3000) call data from Backend (port 5000)
app = Flask(__name__)
CORS(app)

app.register_blueprint(auth_bp, url_prefix='/api/auth')
app.register_blueprint(users_bp, url_prefix='/api/users')
app.register_blueprint(dashboard_bp, url_prefix='/api/dashboard')
app.register_blueprint(chat_bp, url_prefix='/api/chat')

load_dotenv()
GEMINI_API_KEY=os.getenv("GEMINI_API_KEY")

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

# ------------------------------ GET ------------------------------
# @app.route('/api/dashboard', methods=['GET'])
# def get_dashboard():
#   return jsonify(dashboard_stats)

# @app.route('/api/employees', methods=['GET'])
# def get_employees():
#   return jsonify(employees_data)

# @app.route('/api/permissions', methods=['GET'])
# def get_permissions_list():
#   return jsonify(['Admin', 'Editor', 'Viewer'])

if __name__ == '__main__':
  port = int(os.environ.get("PORT", 5000))
  local = not os.environ.get("RENDER")
  app.run(host='0.0.0.0', port=port, debug=local)