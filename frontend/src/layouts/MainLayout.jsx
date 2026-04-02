import React from 'react';
import Sidebar from '../components/Sidebar';

export const MainLayout = ({ children, activeTab, onSelect }) => {
  return (
    <div className="app-container">
      {/* Bên trái: Sidebar cố định */}
      <Sidebar activeTab={activeTab} onSelect={onSelect} />

      {/* Bên phải: Vùng trắng full màn hình còn lại */}
      <main className="main-content">
        {children}
      </main>
    </div>
  );
};

export default MainLayout;