from flask import Flask, jsonify
from flask_cors import CORS

# Allow Frontend (port 3000) call data from Backend (port 5000)
app = Flask(__name__)
CORS(app)

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

# --- Endpoints ---
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
    app.run(debug=True, port=5000)