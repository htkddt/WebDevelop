import os
from flask import Flask
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

if __name__ == '__main__':
  port = int(os.environ.get("PORT", 5000))
  local = not os.environ.get("RENDER")
  app.run(host='0.0.0.0', port=port, debug=local)