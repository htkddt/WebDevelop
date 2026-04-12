from flask import Blueprint, jsonify
from database import users_col

users_bp = Blueprint('users', __name__)

@users_bp.route('/employees', methods=['GET'])
def get_all_users():
    try:
        users = list(users_col.find({}, {"password": 0}))
        for user in users:
            user["_id"] = str(user["_id"])
        return jsonify(users), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500

# @users_bp.route('/permissions', methods=['GET'])
# def get_permissions_list():
#     all_perms = ["view_dashboard", "manage_employees", "edit_permissions", "chat_ai"]
#     return jsonify(all_perms), 200