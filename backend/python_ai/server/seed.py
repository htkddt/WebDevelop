from pymongo import MongoClient
from passlib.hash import pbkdf2_sha256
from datetime import datetime

def seed_data():
    # 1. Connect to MongoDB
    client = MongoClient("mongodb://localhost:27017/")
    db = client["179FC"]
    users_col = db["users"]

    # 2. Refresh old data
    print("Cleaning up the old database ...")
    users_col.delete_many({})

    # 3. Hashed Password with default password
    default_password = pbkdf2_sha256.hash("klose123")

    # 4. The list of ten user pattern
    sample_users = [
        # --- 1 ADMIN ---
        {
            "username": "admin_boss",
            "email": "admin@klose.dev",
            "password": default_password,
            "role": "admin",
            "dept": "Management",
            "permissions": "all",
            "status": "active"
        },
        
        # --- 3 MANAGERS ---
        {
            "username": "hoang_manager",
            "email": "hoang.sales@klose.dev",
            "password": default_password,
            "role": "manager",
            "dept": "Sales",
            "permissions": "dept",
            "status": "active"
        },
        {
            "username": "lan_manager",
            "email": "lan.hr@klose.dev",
            "password": default_password,
            "role": "manager",
            "dept": "HR",
            "permissions": "dept",
            "status": "active"
        },
        {
            "username": "minh_manager",
            "email": "minh.it@klose.dev",
            "password": default_password,
            "role": "manager",
            "dept": "IT",
            "permissions": "dept",
            "status": "active"
        },

        # --- 6 STAFFS ---
        {
            "username": "an_staff",
            "email": "an.nguyen@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "Sales",
            "permissions": "view",
            "status": "active"
        },
        {
            "username": "binh_staff",
            "email": "binh.le@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "Sales",
            "permissions": "view",
            "status": "active"
        },
        {
            "username": "chi_staff",
            "email": "chi.tran@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "HR",
            "permissions": "view",
            "status": "leave"
        },
        {
            "username": "dung_staff",
            "email": "dung.pham@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "Marketing",
            "permissions": "view",
            "status": "active"
        },
        {
            "username": "en_staff",
            "email": "en.vo@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "Marketing",
            "permissions": "view",
            "status": "active"
        },
        {
            "username": "phi_staff",
            "email": "phi.hoang@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "IT",
            "permissions": "view",
            "status": "inactive"
        }
    ]

    for user in sample_users:
        user["createdAt"] = datetime.now()

    # 5. Push data to MongoDB
    users_col.insert_many(sample_users)
    print("--- SUCCESSFULL ---")
    print(f"Created {len(sample_users)} user pattern.")
    print("Default password apply for all: klose123")

if __name__ == "__main__":
    seed_data()