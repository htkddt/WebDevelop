import React, { useState } from 'react';
import { Link } from 'react-router-dom';
import '../styles/Register.css';

const RegisterView = ({ onRegister, onSwitchLogin, loading }) => {
  const [formData, setFormData] = useState({
    username: '',
    email: '',
    password: '',
    role: '',
    department: '',
    permission: "view",
    status: "active"
  });

  const handleChange = (e) => {
    setFormData({ ...formData, [e.target.name]: e.target.value });
  };

  const handleSubmit = (e) => {
    e.preventDefault();
    if (formData.password.length < 6) {
      alert("Mật khẩu phải ít nhất 6 ký tự nhe ní!");
      return;
    }
    onRegister({
      ...formData,
      email: `${formData.username}@klose.dev`
    });
  };

  return (
    <div className="register-page-wrapper">
      <div className="register-card">
        <h2 className="register-title">Create Account</h2>
        <form className="register-form" onSubmit={handleSubmit}>

          <div className="form-group">
            <label>Full Name</label>
            <input name="username" type="text" className="form-input" onChange={handleChange}
              autoComplete="one-time-code"
              required
            />
          </div>

          <div className="form-group">
            <label>Email</label>
            <div className="email-input-wrapper">
              <input name="email" type="text" className="form-input" onChange={handleChange}
                autoComplete="one-time-code"
                required
              />
              <span className="email-suffix">@klose.dev</span>
            </div>
          </div>

          <div className="form-group">
            <label>Password</label>
            <input name="password" type="password" className="form-input" onChange={handleChange}
              autoComplete="one-time-code"
              required
            />
          </div>

          <div className="form-group">
            <label>Department</label>
            <input name="dept" type="text" className="form-input" onChange={handleChange}
              autoComplete="one-time-code"
              required
            />
          </div>

          <button type="submit" className="register-submit-btn" disabled={loading}>
            {loading ? "Registering ..." : "Sign Up"}
          </button>
        </form>
        <p className="signup-text">
          Already have an account? <Link to="/login" className="signup-link">Login</Link>
        </p>
      </div>
    </div>
  );
};

export default RegisterView;