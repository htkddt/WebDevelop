import { useState } from 'react';
import '../styles/Login.css';
import { Eye, EyeOff } from 'lucide-react';

const EyeIcon = ({ show }) => (
  <span style={{ fontSize: '1.2rem', color: '#9ca3af', cursor: 'pointer' }}>
    {show ? <EyeOff size={20} /> : <Eye size={20} />}
  </span>
);

export const LoginView = () => {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [showPassword, setShowPassword] = useState(false);

  return (
    <div className="login-page-wrapper">
      <div className="login-card">
        <h2 className="login-title">Login to your account</h2>

        <form className="login-form">
          <div className="form-group">
            <label htmlFor="email">Email</label>
            <input
              id="email"
              type="email"
              value={email}
              onChange={(e) => setEmail(e.target.value)}
              placeholder="Your email address"
              className="form-input"
              required
            />
          </div>

          <div className="form-group">
            <div className="password-label-wrapper">
              <label htmlFor="password">Password</label>
              <a href="#" className="forgot-link">Forgot ?</a>
            </div>
            <div className="password-input-wrapper">
              <input
                id="password"
                type={showPassword ? 'text' : 'password'}
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                placeholder="Enter your password"
                className="form-input password-input"
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

          <button type="submit" className="login-submit-btn">
            Login now
          </button>
        </form>

        <p className="signup-text">
          Don't Have An Account ? <a href="#" className="signup-link">Sign Up</a>
        </p>
      </div>
    </div>
  );
};

export default LoginView;