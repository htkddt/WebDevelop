import os
from pymongo import MongoClient
from dotenv import load_dotenv

load_dotenv()
MONGO_URL=os.getenv("MONGO_URL")
if MONGO_URL is None:
    MONGO_URL = "mongodb://localhost:27017"

client = MongoClient(MONGO_URL)

# Locate to database of project
db = client["179FC"]

# Export collections to apply for the other files
users_col = db["users"]
# chat_history_col = db["chat_history"]