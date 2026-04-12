from flask import Blueprint, jsonify
from database import users_col

dashboard_bp = Blueprint('dashboard', __name__)

@dashboard_bp.route('/', methods=['GET'])
def get_dashboard_stats():
    try:
        # Statistics on the number of users by status
        total = users_col.count_documents({})
        active = users_col.count_documents({"status": "active"})
        inactive = users_col.count_documents({"status": "inactive"})
        leave = users_col.count_documents({"status": "leave"})
        
        # Statistics by departments
        pipeline = [{"$group": {"_id": "$dept", "count": {"$sum": 1}}}]
        dept = list(users_col.aggregate(pipeline)) # The number of employees by departments

        return jsonify({
            "dept": len(dept),
            "total": total,
            "active": active,
            "inactive": inactive,
            "leave": leave,
        }), 200
    except Exception as e:
        return jsonify({"error": str(e)}), 500