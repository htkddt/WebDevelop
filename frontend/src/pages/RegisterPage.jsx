import React, { useState } from 'react';
import RegisterView from '../views/RegisterView';
import './../styles/Register.css';

const RegisterPage = ({ setView }) => {
  const [loading, setLoading] = useState(false);

  const API_URL = window.location.hostname === "localhost"
    ? "http://localhost:5000/api/auth/login"
    : "https://webdevelop-gnyi.onrender.com/api/auth/register";

  const handleRegister = async (userData) => {
    setLoading(true);
    try {
      const response = await fetch(API_URL, {
        method: 'POST',
        headers: { 'type': 'json' },
        body: JSON.stringify(userData),
      });

      if (response.ok) {
        alert("Successful! Giờ đăng nhập nhe ní.");
        setView('login'); // Đóng register và hiện login
      } else {
        alert("Lỗi rồi, username chắc bị trùng đó!");
      }
    } catch (error) {
      console.error("Lỗi kết nối:", error);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="app-content">
      <div className="register-page-wrapper">
        <RegisterView
          onRegister={handleRegister}
          onSwitchLogin={() => setView('login')}
          loading={loading}
        />
      </div>
    </div>

  );
};

export default RegisterPage;