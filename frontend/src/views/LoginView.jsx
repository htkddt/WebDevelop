import { useState } from 'react';
import { Eye, EyeOff } from 'lucide-react';
import { Link } from 'react-router-dom';
import '../styles/Login.css';

const EyeIcon = ({ show }) => (
  <span style={{ fontSize: '1.2rem', color: '#9ca3af', cursor: 'pointer' }}>
    {show ? <EyeOff size={20} /> : <Eye size={20} />}
  </span>
);

export const LoginView = ({ onLogin, loading, errorMessage }) => {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [showPassword, setShowPassword] = useState(false);

  const handleSubmit = (e) => {
    e.preventDefault();
    onLogin({ email: `${email}@klose.dev`, password });
  };

  return (
    <div className="login-page-wrapper">
      <div className="login-card">
        <h2 className="login-title">Login to your account</h2>

        <form className="login-form" onSubmit={handleSubmit}>
          <div className="form-group">
            <label htmlFor="email">Email</label>
            <div className="email-input-wrapper">
              <input
                id="email"
                tabIndex={1}
                type="text"
                value={email}
                onChange={(e) => setEmail(e.target.value)}
                placeholder="Your email address"
                className={`form-input email-input ${errorMessage ? 'input-error' : ''}`}
                style={errorMessage ? { borderColor: '#ef4444' } : {}}
                autoComplete="one-time-code"
                required
              />
              <span className="email-suffix">@klose.dev</span>
            </div>
          </div>

          <div className="form-group">
            <div className="password-label-wrapper">
              <label htmlFor="password">Password</label>
              <a href="#" className="forgot-link">Forgot ?</a>
            </div>
            <div className="password-input-wrapper">
              <input
                id="password"
                tabIndex={2}
                type={showPassword ? 'text' : 'password'}
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                placeholder="Enter your password"
                className={`form-input password-input ${errorMessage ? 'input-error' : ''}`}
                style={errorMessage ? { borderColor: '#ef4444' } : {}}
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
            {errorMessage && (
              <p style={{
                color: '#ef4444',
                fontSize: '0.85rem',
                marginTop: '8px',
                textAlign: 'left',
                fontWeight: '500'
              }}>
                {errorMessage}. Please try again!
              </p>
            )}
          </div>

          <button type="submit" className="login-submit-btn" disabled={loading}>
            {loading ? "Checking ..." : "Login now"}
          </button>
        </form>

        <p className="signup-text">
          Don't Have An Account ? <Link to="/register" className="signup-link">Sign Up</Link>
        </p>
      </div>
    </div>
  );
};

export default LoginView;