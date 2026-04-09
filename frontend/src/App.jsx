import React from 'react';
import { BrowserRouter as Router } from 'react-router-dom';
import AppRoutes from './AppRoutes';
import './styles/Contents.css';

// Test release for github actions

function App() {
  return (
    <Router>
      <AppRoutes />
    </Router>
  );
}

export default App;