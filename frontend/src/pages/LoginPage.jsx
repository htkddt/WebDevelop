import React, { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import LoginView from '../views/LoginView';
import './../styles/Login.css';

const LoginPage = () => {
  // Proccessing API logic
  // const handleLoginSubmit = (data) => {
  //   console.log("Login data:", data);
  // };

  const [loading, setLoading] = useState(false);
  const [error, setError] = useState("");
  const navigate = useNavigate();

  const API_URL = window.location.hostname === "localhost"
    ? "http://localhost:5000/api/auth/login"
    : "https://webdevelop-gnyi.onrender.com/api/auth/login";

  const handleLogin = async (credentials) => {
    setLoading(true);
    setError("");

    try {
      const response = await fetch(API_URL, {
        method: "POST",
        headers: { "type": "json" },
        body: JSON.stringify(credentials),
      });
      const data = await response.json();
      if (response.ok) {
        localStorage.setItem('token', data.token);
        localStorage.setItem('user', JSON.stringify(data.user));
        // Triggered to Leftbar for checking token and show if login is successful
        //window.dispatchEvent(new Event("storage"));
        navigate('/dashboard');
      } else {
        setError(data.message);
      }
    } catch (err) {
      setError(err);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="app-content">
      <div className="login-page-wrapper">
        <LoginView onLogin={handleLogin} loading={loading} errorMessage={error} />
      </div>
    </div>
  );
};

export default LoginPage;