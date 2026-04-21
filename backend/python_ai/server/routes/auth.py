from flask import Blueprint, request, jsonify
from database import users_col
from passlib.hash import pbkdf2_sha256
import jwt
import datetime

auth_bp = Blueprint('auth', __name__)
SECRET_KEY = "KLOSE_SECRET_KEY"

@auth_bp.route('/login', methods=['POST'])
def login():
    data = request.get_json(force=True)
    user = users_col.find_one({"email": data.get('email')})

    if user and pbkdf2_sha256.verify(data.get('password'), user['password']):
        token = jwt.encode({
            'user_id': str(user['_id']),
            'role': user['role'],
            'exp': datetime.datetime.now() + datetime.timedelta(days=1)
        }, SECRET_KEY, algorithm="HS256")
        
        return jsonify({
            "token": token,
            "user": {"username": user['username'], "role": user['role']}
        }), 200
    
    return jsonify({"message": "Invalid email or password"}), 401

@auth_bp.route('/register', methods=['POST'])
def register():
    data = request.get_json(force=True)
    if users_col.find_one({"email": data.get('email')}):
        return jsonify({"message": "Email is existing"}), 400
    
    print(data)
    
    new_user = {
        "username": data.get('username'),
        "email": data.get('email'),
        "password": pbkdf2_sha256.hash(data.get('password')),
        "role": data.get('role'),
        "dept": data.get('dept'),
        "permissions": data.get('permission'),
        "status": data.get('status')
    }
    users_col.insert_one(new_user)
    return jsonify({"message": "Successfully"}), 201