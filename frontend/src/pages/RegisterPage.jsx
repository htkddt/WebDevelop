import React, { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import RegisterView from '../views/RegisterView';
import './../styles/Register.css';

const RegisterPage = ({ setView }) => {
  const [loading, setLoading] = useState(false);
  const navigate = useNavigate();

  const API_URL = window.location.hostname === "localhost"
    ? "http://localhost:5000/api/auth/register"
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
        navigate('/login');
      } else {
        alert("Username is existing.");
      }
    } catch (error) {
      console.error("Connection error Backend:", error);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="app-content">
      <div className="register-page-wrapper">
        <RegisterView
          onRegister={handleRegister}
          loading={loading}
        />
      </div>
    </div>

  );
};

export default RegisterPage;