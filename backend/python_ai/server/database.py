from pymongo import MongoClient

client = MongoClient("mongodb://localhost:27017/")

# Locate to database of project
db = client["179FC"]

# Export collections to apply for the other files
users_col = db["users"]
# chat_history_col = db["chat_history"]