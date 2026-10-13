import { useState } from 'react';
import { Eye, EyeOff } from 'lucide-react';
import { Link } from 'react-router-dom';
import '../styles/Register.css';
import '../styles/Login.css';

const EyeIcon = ({ show }) => (
  <span style={{ fontSize: '1.2rem', color: '#9ca3af', cursor: 'pointer' }}>
    {show ? <EyeOff size={20} /> : <Eye size={20} />}
  </span>
);

const RegisterView = ({ onRegister, loading }) => {
  const [showPassword, setShowPassword] = useState(false);
  const [formData, setFormData] = useState({
    username: '',
    email: '',
    password: '',
    role: "staff",
    dept: '',
    permission: "view",
    status: "active"
  });

  const handleChange = (e) => {
    setFormData({ ...formData, [e.target.name]: e.target.value });
  };

  const handleSubmit = (e) => {
    e.preventDefault();
    if (formData.password.length < 6) {
      alert("The password must be at least 6 characters long.");
      return;
    }
    onRegister({
      ...formData,
      email: `${formData.email}@klose.dev`
    });
  };

  return (
    <div className="register-page-wrapper">
      <div className="register-card">
        <h2 className="register-title">Create Account</h2>
        <form className="register-form" onSubmit={handleSubmit}>

          <div className="form-group">
            <label>Full Name</label>
            <input
              tabIndex={1}
              name="username"
              type="text"
              className="form-input"
              onChange={handleChange}
              placeholder="Enter your fullname"
              autoComplete="one-time-code"
              required
            />
          </div>

          <div className="form-group">
            <label>Email</label>
            <div className="email-input-wrapper">
              <input
                tabIndex={2}
                name="email"
                type="text"
                className="form-input"
                onChange={handleChange}
                placeholder="[Last_name].[First_name]"
                autoComplete="one-time-code"
                required
              />
              <span className="email-suffix">@klose.dev</span>
            </div>
          </div>

          <div className="form-group">
            <div className="password-label-wrapper">
              <label>Password</label>
            </div>
            <div className="password-input-wrapper">
              <input
                tabIndex={3}
                name="password"
                type={showPassword ? 'text' : 'password'}
                className="form-input" onChange={handleChange}
                placeholder="Enter your password"
                autoComplete="one-time-code"
                required
              />
              <button
                type="button"
                className="password-toggle-btn"
                onClick={() => setShowPassword(!showPassword)}
              >
                <EyeIcon show={showPassword} />
              </button>
            </div>
          </div>

          <div className="form-group">
            <label>Department</label>
            <input
              tabIndex={4}
              name="dept"
              type="text"
              className="form-input"
              onChange={handleChange}
              placeholder="Enter your department"
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