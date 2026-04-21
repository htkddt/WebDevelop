import os
from dotenv import load_dotenv

from pymongo import MongoClient
from passlib.hash import pbkdf2_sha256
from datetime import datetime

# load_dotenv()
# MONGO_URL=os.getenv("MONGO_URL")
# if MONGO_URL is None:
#     MONGO_URL = "mongodb://localhost:27017"

MONGO_URL = "mongodb+srv://htkddt:klose123@cluster.ig79etu.mongodb.net/"
#MONGO_URL = "mongodb://localhost:27017"

def seed_data():
    # 1. Connect to MongoDB
    client = MongoClient(MONGO_URL)
    db = client["179FC"]
    users_col = db["users"]

    # 2. Refresh old data
    print("Cleaning up the old database ...")
    users_col.delete_many({})

    # 3. Hashed Password with default password
    default_password = pbkdf2_sha256.hash("webdevelop2026")

    # 4. The list of ten user pattern
    sample_users = [
                # --- ADMIN ---
        {
            "username": "Admin User Test 1",
            "email": "admin1@klose.dev",
            "password": default_password,
            "role": "admin",
            "dept": "Management",
            "permissions": "all",
            "status": "active"
        },

        {
            "username": "Admin User Test 2",
            "email": "admin2@klose.dev",
            "password": default_password,
            "role": "admin",
            "dept": "Management",
            "permissions": "all",
            "status": "active"
        },
        
                # --- MANAGERS ---
        {
            "username": "Manager User Test 1",
            "email": "manager1@klose.dev",
            "password": default_password,
            "role": "manager",
            "dept": "Sales",
            "permissions": "dept",
            "status": "active"
        },
        {
            "username": "Manager User Test 2",
            "email": "manager2@klose.dev",
            "password": default_password,
            "role": "manager",
            "dept": "HR",
            "permissions": "dept",
            "status": "inactive"
        },
        {
            "username": "Manager User Test 3",
            "email": "manager3@klose.dev",
            "password": default_password,
            "role": "manager",
            "dept": "IT",
            "permissions": "dept",
            "status": "active"
        },

                # --- STAFFS ---
        {
            "username": "Staff User Test 1",
            "email": "staff1@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "IT",
            "permissions": "view",
            "status": "inactive"
        },
        {
            "username": "Staff User Test 2",
            "email": "staff2@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "Sales",
            "permissions": "view",
            "status": "active"
        },
        {
            "username": "Staff User Test 3",
            "email": "staff3@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "HR",
            "permissions": "view",
            "status": "active"
        },
        {
            "username": "Staff User Test 4",
            "email": "staff4@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "HR",
            "permissions": "view",
            "status": "active"
        },
        {
            "username": "Staff User Test 5",
            "email": "staff5@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "Marketing",
            "permissions": "view",
            "status": "inactive"
        },
        {
            "username": "Staff User Test 6",
            "email": "staff6@klose.dev",
            "password": default_password,
            "role": "staff",
            "dept": "Marketing",
            "permissions": "view",
            "status": "active"
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
    # try:
    #     client = MongoClient(MONGO_URL)
    #     client.admin.command('ping')
    #     print("Successfull")
    #     print("DBs:", client.list_database_names())
        
    # except Exception as e:
    #     print(f"ERROR: {e}")