import React from 'react';

export const LoginForm = () => (
  <div className="login-card">
    <h2 style={{ textAlign: 'center', marginBottom: '30px' }}>⚽ 179FC</h2>
    <div className="form-group">
      <label>Username/e-mail</label>
      <input type="text" placeholder="Username/e-mail" className="custom-input" />
    </div>
    <div className="form-group" style={{ marginTop: '20px' }}>
      <label>Password</label>
      <input type="password" placeholder="••••••••" className="custom-input" />
    </div>
    <button className="login-btn" style={{ marginTop: '30px', width: '100%' }}>Login</button>
  </div>
);