import os
import google.generativeai as genai

base_dir = os.path.dirname(os.path.abspath(__file__))
file_path = os.path.join(base_dir, "API_KEY_LOCAL.txt")

with open(file_path, "r", encoding="utf-8") as f:
    API_KEY = f.readline().strip()
print(API_KEY)
genai.configure(api_key=API_KEY)

for m in genai.list_models():
    if 'generateContent' in m.supported_generation_methods:
        print(f"Tên chuẩn: {m.name}")